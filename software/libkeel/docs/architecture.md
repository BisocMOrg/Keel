<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Architecture

This document is for maintainers and for anyone porting libkeel to another platform. It explains how the
library is put together and why; [spec.md](spec.md) has every rule in full, and the section numbers below point
into it.

## 1. Layers

```
  Applications (C, C++)          Bindings: Python ctypes | Java FFM | C# (rules only)
            \                                 |
             +------------ include/keel/keel.h  (ABI 1.x, exported from libkeel.so.1) ----+
                                                                                          |
  src/core      portable C11: no system calls, no allocation, no threads                  |
                api.c       entry points: handle check, evaluation order, struct sizes    |
                registry.c  generation-checked handle tokens, lock-free validation        |
                safety.c    heartbeat, arming handshake, feedback rules, fault latch      |
                motion.c    duties, deadman timeout, deferred reversal, brake, lease      |
                servo.c     servo bursts      range.c     HC-SR04 classification          |
                leds.c      LED driver protocol   buttons.c   debounce                    |
                fault.c     status, class and cause tables   snapshot.c   diagnostics     |
             +------------ include/keel/port.h  (C only; libkeel.a and source builds) ----+
             |                              |                                 |
  src/backend/keel_r3            src/backend/sim                       your port
  GPIO v2, i2c-dev,              board model + the production          (bare metal,
  the PWM worker thread          safety-MCU policy, two clocks          an RTOS, a test double)
             |                              |
  src/backend/pwm_policy.c: the PWM pattern and lateness budget both backends share
  src/os: registry lock, process id and fork handling (POSIX, or none for single-threaded ports)
```

The split follows one idea: **the safety logic is written once, in portable C, and tested against the real MCU
firmware policy; everything platform-specific sits behind a narrow table of hooks.**

- `src/core` never calls the operating system. Clocks, sleeps, locks, thread ids and all I/O arrive through the
  backend table of `port.h`; the handle registry's lock comes from `src/os`. That is what lets the same core run
  on the robot, in the simulator, in the unit tests with a scripted fake, and on a microcontroller.
- Backends know nothing about the safety rules. They move lines and report failures; the few defences they add
  (interlocks, the stop epoch) are a second line behind the core, never a replacement for it.
- `keel.h` is the only header bindings see. It contains only fixed-width types, enumerators and explicitly padded
  structs, so it maps one to one onto ctypes, FFM and C# without hand-written glue.

The camera has a library of its own, `libkeel-camera.so.1` (header `keel/camera.h`, SPEC 26), because the camera
is on the Pi's connector, not on the board, and has nothing to do with the safety function. It follows the same ABI
conventions, uses `keel.h`'s status codes and nothing else of libkeel, and libkeel does not link it:

```
  keel/camera.h (ABI 1.x, exported from libkeel-camera.so.1)
  src/camera   camera_api.c      handle tokens, owner thread, struct sizes, option checks
               camera_session.c  queues, encoder, outputs and stills of one open camera
               capture_libcamera.cpp   libcamera (the only C++ file)   capture_synthetic.c   test camera
               encoder_v4l2.c    the Pi's H.264 and JPEG encoders (V4L2 memory-to-memory)
                                 encoder_synthetic.c   test encoder
               output.c          files, TCP, listening TCP, UDP   output_libav.c   containers (optional)
               h264_colour.c, jpeg_software.c, convert.c, diagnose.c, destination.c, pipeline.c
```

## 2. Source tree

| Path | Contents |
|---|---|
| `include/keel/keel.h` | the application and binding API (the ABI) |
| `include/keel/port.h` | the porting interface: backend hooks, storage, attach |
| `src/core/` | the portable core (above) and `internal.h`, its private declarations |
| `src/backend/hosted.h` | how `keel_open` reaches the built-in backends |
| `src/backend/keel_r3/` | the R3 backend for the Raspberry Pi 4 |
| `src/backend/sim/` | the simulator: `sim_board.c` (board model), `sim_backend.c` (hooks, locks, clocks), `mcu/` (the production MCU policy, hash-guarded) |
| `src/os/` | `os_posix.c` and `os_none.c` |
| `include/keel/camera.h`, `src/camera/` | the camera library (above) |
| `tools/` | `keel-live`, `keel-demo`, `keel-doctor` and `keel-camera` |
| `cmake/` | compiler options, the version scripts `keel.map` and `keel-camera.map`, the CMake package and pkg-config templates, backend, tool, camera and packaging modules, the arm64 toolchain |
| `tests/` | one directory per layer: `core/` (unit and reference), `integration/` (arbitration, errors, random walk, threads), `sim/`, `backend/`, `tools/`, `camera/`, `abi/`, `consumers/`, `bindings/` (conformance scenarios), `docs/`, `packaging/`, `hygiene/`; `common/` holds the shared assertions |
| `debian/`, `packaging/`, `install.sh`, `scripts/keel-setup` | the Debian packages, their system files and the installer |
| `docs/` | this document, the guides and the specification |

## 3. The life of a call

Every entry point follows the same evaluation order (SPEC 13.1), which is why a refusal never has a side effect
and a stop is never delayed:

1. **Process and handle.** The handle is a token `(generation << 8) | (slot + 1)`, never a pointer. The call
   compares the slot's atomic owner pid with the cached process id (a forked child gets `KEEL_E_NOT_OWNER`), then
   checks the slot's generation and closing bit, takes a reference and checks again, all without a lock. A
   default mutex is not fair, and a thread polling in a tight loop once delayed a disarm by 7 ms through the
   registry lock; validation therefore takes none.
2. **Stop calls lower ENABLE first.** `keel_disarm`, `keel_emergency_stop` and `keel_close` bump the device's
   atomic stop epoch and call the backend's `emergency_disable` before taking any core lock.
3. **Arguments, capability, thread, FAULTED, hold-off, state, conflict, timing**, all decided from cached state.
   Every REJECTED result is therefore decided without I/O and recorded in `snapshot.last_rejection`.
4. **Control I/O** under the CONTROL lock: a fresh `read_inputs` (whose `KEEL_IN_BACKEND_FAULT` bit is checked
   before anything else), then the clock, the tick window and the feedback rules.
5. **The action**, with every hook status passed through the class rules (SPEC 12.2): a FAULT-class status from
   a safety-path hook passes through, anything else latches `HAL_CONTRACT`; peripheral failures become
   `KEEL_E_PERIPHERAL`.

## 4. Locks and stop latency

| Domain | Protects | Rule |
|---|---|---|
| registry mutex (`src/os`) | publishing a handle, close's wait for calls in flight, fork | never held while a device lock is taken |
| CONTROL | safety state, heartbeat, handshake, motion, servo, buttons, fault record, snapshot | never held across a sleep or a blocking wait |
| LEDS | the LED frame cache and I2C | never taken while CONTROL is held |
| RANGE | ultrasonic state | never taken while CONTROL is held |
| backend lock | the backend's shadows and line writes | a leaf: nothing is called while holding it |

No path holds two domain locks at once, so there is no lock order to get wrong. The facts one domain needs from
another travel through atomics (READY drops to ranging, the latest echo rise to the snapshot). A tick raises the
heartbeat, **releases CONTROL for the 1 ms pulse**, then lowers it, so a stop from another thread never waits
for a pulse.

The **stop epoch** closes the remaining race: a control section that was already running when another thread
disarmed cannot undo the stop, because the backend refuses any rising ENABLE or drive command carrying an older
epoch, and every CONTROL section applies a pending stop on entry. The test `threads.disarm_latency` measures the
time from the `keel_disarm` call to ENABLE LOW under load with an I2C stub that hangs every transaction: p99.9
under 2 ms is the gate.

## 5. Faults

A FAULT-class detection captures the record first, then performs the Category-0 stop with the heartbeat stopped,
then latches `FAULTED` (SPEC 12). The first fault since the last clear wins. A backend that detects a failure on
its own thread (a late PWM wake) latches a record of its own and sets `KEEL_IN_BACKEND_FAULT`; the next entry
point that reads the inputs records the backend's cause before evaluating anything else, so a worker overrun is
reported as `PWM_OVERRUN`, never as the `ARMED_LOST` it causes a moment later.

## 6. Backends

**Built-in backends** (`keel_r3`, the simulator) allocate one block for their own state and the core's device
storage and attach the core with `keel__attach` (`src/backend/hosted.h`). `keel_open` selects them from
`keel_open_options.backend`, or from the `KEEL_BACKEND` environment variable with `KEEL_BACKEND_AUTO`, and never
falls back to the simulator silently.

**keel_r3** (SPEC 17) requests the lines through the GPIO character device v2, exclusively and with every output
starting LOW, refusing lines already used by kernel functions such as SPI0. The motor pins have no hardware PWM,
so a worker thread generates 100 Hz PWM at SCHED_FIFO priority 40 (best effort when refused). The worker is the
library's only thread. Late wakes are never fatal one by one; only a wake more than 30 ms late or a sustained
lateness budget latches.

**The simulator** (SPEC 18) models the host's pins, the motor bridges and the enable gate, the LED driver, the
servo port and the ultrasonic sensor, and steps the **production safety-MCU policy** every simulated millisecond.
That policy is the firmware's `safety_core.c`, copied unmodified into `src/backend/sim/mcu/` behind a provenance
header; a test hashes the copy, and a wrapper translation unit renames its global symbols so the static library
exports only `keel_` names. The simulator follows the real-time clock by default; with `KEEL_OPEN_VIRTUAL_CLOCK`
time moves only inside `keel_sleep_until`, which makes tests and binding scenarios deterministic.

**Ports** (SPEC 19) attach the core to caller-provided storage with `keel_port_attach` and a table of hooks:
the required clock, sleep, input, output, motor, emergency-disable and fault hooks, and optional locks, thread id,
LED, servo, ranging and statistics hooks. A capability declared without its hooks is refused at attach. A port
without lock hooks declares itself single-threaded, and the `none` OS layer replaces pthreads.

## 7. ABI and versioning

- `libkeel.so.1` exports exactly the 37 functions of `tests/abi/symbols.txt` under the version node `KEEL_1.0`;
  everything else, including the porting interface, is hidden by an explicit version script.
- Constants are enumerators with frozen values (`tests/abi/constants.golden`); struct offsets are pinned on four
  targets (`tests/abi/layout.c`); every struct is versioned by `struct_size` with the Linux
  `copy_struct_from_user` rule.
- The version has one source, the `KEEL_VERSION_*` macros in `keel.h`, which CMake parses. Minor versions only
  add; a new major means a new SONAME and a new runtime package name.

## 8. Tests

The suite is organised in layers with CTest labels (SPEC 23):

| Label | What it proves |
|---|---|
| `unit` | the PWM policy, the registry, the core on the single-threaded OS layer |
| `reference` | the core against the hash-guarded MCU policy, including the 1-70 ms schedule sweep |
| `arbitration` | every reachable cell of the request-versus-state table, from both thread columns |
| `errors` | every hook failure at every entry point keeps its class |
| `walk` | seeded 10,000-call random walks with per-millisecond invariants (`KEEL_TEST_SEED` replays one) |
| `threads` | stop versus cycle, ownership, close races and the disarm latency bound |
| `simulator` | the public API end to end against the simulator, and the hash guard |
| `backend` | the keel_r3 backend with its system calls replaced by a fake kernel |
| `abi` | exports, header compilation in C and C++, layouts, constants, documentation, and the ctypes binding |
| `consumers` | C, C++, the README example, Python ctypes and the installed package |
| `docs`, `packaging` | the documents' tables, the packaging metadata, `keel-setup` and `install.sh` |
| `camera` | the camera library and `keel-camera` against the test source: a synthetic Camera Module 2 and 3 and encoder (SPEC 26.9) |

Hardware qualification runs on the robot (`keel-live`, and `keel-camera` with a camera connected), outside CTest.

## 9. Build and release

CMake presets cover development (`dev`), sanitizers (`asan-ubsan`, `tsan`), `release`, `sim-only` and the arm64
cross build (`cross-arm64`). The Debian packages are built by debhelper from the same CMake install rules
(`debian/`, `cmake/packaging.cmake`). The camera library is built wherever CMake finds libcamera; because libcamera
has no stable ABI, its packages (`libkeel-camera1`, `libkeel-camera-dev`, `keel-camera`) are built for the robot's
Ubuntu release, and no board package depends on them (SPEC 26.11). The CI workflow at the repository root
(`.github/workflows/libkeel.yml`) runs every preset, the ABI and documentation gates, the consumers, the package
builds and a check that no other party's product names appear in the tree; on a `libkeel-v*` tag it publishes the
arm64 packages, `SHA256SUMS` and `install.sh` as a release. See [deploy.md](deploy.md).
