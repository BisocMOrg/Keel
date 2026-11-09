<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# libkeel 1.0 specification

Version 1.0.0, ABI 1.0, SONAME `libkeel.so.1`. Date: 2026-11-07.

libkeel is the C11 library that drives the Keel robot baseboard (R3) on a Raspberry Pi 4. C applications use
it directly. The Python and Java bindings (`software/bindings`) use it through the same header; section 21 gives
the rules for any binding.

The contract is `include/keel/keel.h` (application and binding API) together with `include/keel/port.h` (C-only porting
interface). This document explains those headers and adds the internal rules that the implementation, tests and
packaging must follow. The headers and this document must agree; a disagreement is a defect in both, fixed in one change.
Section 26 specifies the camera library `libkeel-camera` (`include/keel/camera.h`), a separate shared library built
and released from the same tree.

Contents:
1. Scope and identity
2. Board and MCU contract
3. Architecture
4. ABI rules
5. Status codes and classes
6. Handles, lifetime and processes
7. Thread ownership: stop from anywhere, go only from the loop
8. Locks and stop latency
9. Safety state machine
10. Motion
11. Stop vocabulary
12. Fault model and cause catalogue
13. Evaluation order and the arbitration table
14. Servo (J5)
15. Ranging (J1)
16. LEDs and buttons
17. The keel_r3 backend
18. Simulator
19. Porting interface
20. Snapshot and diagnostics
21. Bindings
22. Build, versioning and packaging
23. Tests and the 1.0 gate
24. Scope cuts and qualification items
25. Decisions taken in this specification
26. Camera library (libkeel-camera)

---

## 1. Scope and identity

| Item | Value |
|---|---|
| Product | Keel |
| Library | `libkeel` 1.0.0, ABI 1.0, SONAME `libkeel.so.1`, files `libkeel.so.1.0.0` and `libkeel.a` |
| Prefixes | functions and types `keel_`; enumerators and macros `KEEL_`; internal symbols `keel__` |
| Headers | `include/keel/keel.h` (the only header bindings use), `include/keel/port.h` (C only, not for bindings) |
| CMake package | `Keel`, targets `Keel::keel` (shared) and `Keel::keel_static` (static, defines `KEEL_STATIC`) |
| pkg-config | `keel` (the shared library) and `keel-static` (`libkeel.a` by path, `-DKEEL_STATIC`, the port API) |
| Backends | `keel_r3` (the R3 board), `simulator`; custom backends through `port.h` |
| Bindings | Python `keel-robot` (import `keel`, Debian package `python3-keel`) and Java `io.github.bisocmorg:keel` (module `io.github.bisocmorg.keel`), in `software/bindings/`; released with libkeel at the same version |
| Binding loader variable | `KEEL_LIBRARY` |
| Backend selection variable | `KEEL_BACKEND` = `r3` or `simulator` (used only with `KEEL_BACKEND_AUTO`) |
| Camera library | `libkeel-camera` 1.0.0, ABI 1.0, SONAME `libkeel-camera.so.1`, header `include/keel/camera.h` (section 26) |
| Debian packages | board: `libkeel1`, `libkeel-dev`, `keel-common` (arch all), `keel-tools`; camera: `libkeel-camera1`, `libkeel-camera-dev`, `keel-camera` |
| Tools | `keel-live`, `keel-demo`, `keel-doctor`, `keel-setup`, `keel-camera` |
| udev rule | `99-keel.rules` |
| Licence | MIT, copyright holder BisocM; `SPDX-License-Identifier: MIT` in every source file (vendor files keep theirs) |
| Location | `software/libkeel/` in the Keel monorepo; CI at `.github/workflows/libkeel.yml` with a path filter on `software/libkeel/**` |
| Platforms | linux-arm64 (the robot: Ubuntu 26.10, glibc 2.44, gcc 15) and linux-x86_64 (simulator, development, CI) |

Host protocol timing and board behaviour:
- the heartbeat: falls 30-70 ms apart, 1 ms HIGH, emitted only from the application's tick;
- the acknowledgement: GPIO26 LOW at least 150 ms and until READY, then HIGH, LOW, HIGH held 150 ms each, timed from
  the completion of each write, with all motor inputs and the servo request LOW throughout;
- the READY/ARMED checks, the 20 ms reversal dwell, 100 Hz motor PWM, left forward on BCM11 and right forward on BCM10.

## 2. Board and MCU contract

### 2.1 Pin map (BCM)

| Function | BCM | Direction | Notes |
|---|---|---|---|
| Heartbeat | 18 | out | to the MCU |
| MOTOR_EN (ENABLE) | 26 | out | to the MCU and the hardware AND gate |
| SERVO_REQUEST | 19 | out | claimed and held LOW on R3 |
| Servo signal (J5) | 12 | - | J5 not fitted on R3; never touched by keel_r3 |
| READY | 20 | in | MCU healthy and fault-free |
| ARMED | 21 | in | output of the motor-enable gate |
| Motor LP, LN, RP, RN | 8, 11, 10, 9 | out | left forward drives 11, right forward drives 10 |
| Buttons A, B, X, Y | 5, 6, 16, 24 | in | active low, pull-up |
| Button LEDs A, B, X, Y | 23, 22, 17, 27 | out | on/off |
| I2C1 SDA, SCL | 2, 3 | kernel | LED driver at 0x54 (IS31FL3218-compatible) |
| I2C_INT | 4 | - | not used, never touched |
| Ultrasonic TRIG, ECHO (J1) | 13, 25 | out, in | HC-SR04 |
| MCU SWD/NRST | 0, 1, 7 | - | never touched |
| UART | 14, 15 | - | never touched |

### 2.2 MCU contract (production policy `safety_core.c`)

- Heartbeat falls must be 20-80 ms apart; two valid falls qualify the heartbeat. The host uses 30-70 ms with a 1 ms HIGH.
- Acknowledgement: MOTOR_EN LOW for at least 100 ms, then HIGH, LOW, HIGH for at least 100 ms each (the host holds
  150 ms), with all motor inputs LOW and the servo request LOW. With a good heartbeat and ENABLE LOW and quiet motors the
  MCU enters its acknowledgement wait by itself after 100 ms, clearing its faults, so READY is HIGH in the steady
  DISARMED state.
- Any motor PWM edge or level during the acknowledgement latches F_PWM.
- Servo: a request rise is accepted only in ARMED with no PWM activity; the lease is 500 ms (F_SERVO_TIMEOUT); the
  cooldown is 2 s after every burst and after every MCU reset (F_COOLDOWN); motors and servo are mutually exclusive;
  request LOW ends the burst and disarms.
- Sensor 5 V is switched by the MCU, independent of arming: it needs a qualified heartbeat, no latched fault and no
  strike lockout.
- Feedback: MOTOR_EN_SAFE = MCU permit AND GPIO26, which drives ARMED. ARMED HIGH while GPIO26 is LOW is a board fault.

## 3. Architecture

```
 Applications (C/C++)        Bindings: Python ctypes | Java FFM (software/bindings) | C# (rules only)
          \                               |
           +----------------- include/keel/keel.h (ABI 1.x) --------------------------------+
                                                                                            |
 src/core   portable C11: no OS calls, no allocation, no threads                           |
   api.c        entry points: handle validation, ownership, struct-size rule, locking      |
   registry.c   generation-checked handle table (64 slots), refcounts, owner pid           |
   safety.c     heartbeat scheduler, acknowledgement, feedback rules, stop epoch           |
   motion.c     arbitration, deadman timeout, deferred reversal, brake, lease              |
   servo.c      burst machine, grant monitoring, cooldown                                  |
   range.c      HC-SR04 classification (edges -> status, distance), sensor-power inference |
   leds.c       IS31FL3218 protocol and frame cache                                        |
   buttons.c    20 ms debounce and edge accumulation                                       |
   fault.c      fault record, status/class/cause tables, pass-through rule                 |
   snapshot.c   snapshot assembly                                                          |
            | include/keel/port.h (C only)                                                  |
   src/backend/keel_r3 keel_r3: GPIO v2, i2c-dev, PWM worker, ECHO edge events             |
   src/backend/sim     simulator: board model + production MCU policy, real or virtual clock|
   src/backend/pwm_policy.c   pure lateness/pattern policy shared by r3 and sim            |
   src/os      os_posix.c: registry mutex, monotonic clock, getpid, pthread_atfork          |
```

Dependency rules (enforced by the CMake target structure):
- `src/core` includes only `<stdint.h>`, `<stddef.h>`, `<string.h>`, `<stdatomic.h>`, `keel/port.h`, its own internal
  header and three library-internal contracts: `backend/hosted.h` (how `keel_open` reaches a built-in backend),
  `os/os.h` (the registry lock and the owner pid) and the generated `keel_build_config.h` (which backends are built
  in). It still makes no OS calls, allocates nothing and creates no threads. Locks, clocks and thread ids come from
  backend hooks; the registry lock comes from `src/os`.
- Backends include `port.h` and their own headers, never core internals. Within `libkeel.so`, only
  `src/backend/keel_r3` includes Linux UAPI headers, and only `src/os` and the backends use pthreads. The camera
  library (`src/camera`, section 26) is a separate library and uses both.
- The simulator compiles the production MCU policy through a wrapper translation unit (section 18.2). Nothing else
  links the policy.
- Tools and consumer tests link only the public library; white-box unit tests link an internal static archive.

Library threads: only the keel_r3 PWM worker. It never calls application code and never touches the heartbeat. The
library registers no signal or atexit handlers and changes no process-wide scheduling.

The camera library (section 26) is a separate shared object with its own header, version script and package.
`libkeel.so.1` contains no camera code and gains no dependency from it; the two share only the status codes and
macros of `keel.h`.

## 4. ABI rules

The `@file` block of `keel.h` states these rules; they are frozen for every 1.x release.

- **Types.** Parameters and fields are fixed-width integers, fixed-size `char` arrays or pointers to them. No `bool`,
  enum type, `size_t`, `long`, struct-by-value, callback or pointer inside a struct. Every struct is laid out with
  explicit padding, so no compiler-inserted padding exists on x86-64, i386, aarch64 LP64 or ILP32 (pinned by
  `tests/abi/layout.c`).
- **Constants are enumerators.** The only macros are `KEEL_VERSION_MAJOR/MINOR/PATCH` and `KEEL_API` (plus include
  guards); `KEEL_API` can be overridden, so `cc -E -P -DKEEL_API= keel.h` yields a header every binding generator parses.
  Enumerators compare and assign to `uint32_t`/`int32_t` without casts in C and C++ (checked by `tests/abi/header_use.c`).
- **Append-only.** Status codes, enumerators, struct fields and functions are only appended within 1.x; released
  values never change (`tests/abi/constants.golden`, `tests/abi/symbols.txt`).
- **Struct versioning** (Linux `copy_struct_from_user`): `struct_size` first, set by the caller.
  - Below the 1.0 size, or above `KEEL_STRUCT_SIZE_MAX` (4096: an uninitialised field, not a newer caller):
    `KEEL_E_INVALID_ARGUMENT`, and nothing is read or written past `struct_size`.
  - Input larger than the library knows: accepted only if every extra byte is zero; otherwise `KEEL_E_UNSUPPORTED` with
    no effect. Exception: `keel_open` reports this as `KEEL_E_INVALID_ARGUMENT` (section 5.3). Reserved input fields
    must be zero.
  - Output: the library writes min(`struct_size`, known size) bytes and zeroes the rest; it never changes `struct_size`.
  - An output struct is untouched on `KEEL_E_INVALID_ARGUMENT`, `KEEL_E_INVALID_HANDLE` and `KEEL_E_NOT_OWNER`; for
    every other status it is fully written. The one exception is `keel_open`'s report: once its own `struct_size` is
    valid it is written for every status, `KEEL_E_INVALID_ARGUMENT` included, because it carries the remedy
    (section 5.3).
- **Strings.** Returned strings are static, NUL-terminated UTF-8, never NULL, never freed, and at most `KEEL_STRING_MAX`
  (256) bytes including the NUL, so Java can read them with a fixed bound. Strings passed in are copied during the call.
- **Exports.** `libkeel.so.1` exports exactly the 37 functions of `tests/abi/symbols.txt` under the version node
  `KEEL_1.0` (an explicit list, not a wildcard, so `keel_port_*` stays hidden). The port API is available only in
  `libkeel.a` and source builds. Internal symbols are `keel__*`, hidden by visibility.
- **Static archive hygiene.** `nm -g --defined-only libkeel.a` shows only `keel_*` and `keel__*` symbols; the MCU policy's
  global names are renamed (section 18.2).

Function catalogue (37 exports):

| Group | Functions |
|---|---|
| Metadata | `keel_abi_version`, `keel_version_string`, `keel_status_name`, `keel_status_string`, `keel_status_class`, `keel_fault_cause_name` |
| Lifetime | `keel_open`, `keel_close(dev, fault*)`, `keel_get_capabilities` |
| Clock | `keel_now_us`, `keel_sleep_until` |
| Safety and loop | `keel_tick`, `keel_cycle`, `keel_request_arm`, `keel_disarm`, `keel_emergency_stop`, `keel_clear_fault`, `keel_get_state`, `keel_get_snapshot`, `keel_get_fault`, `keel_read_safety_inputs` |
| Motion | `keel_set_motors`, `keel_brake` |
| Servo | `keel_servo_start`, `keel_servo_set_pulse`, `keel_servo_end` |
| Ranging | `keel_range_start`, `keel_range_poll`, `keel_range_measure` |
| LEDs | `keel_set_rgb`, `keel_set_rgb_all`, `keel_set_rgb_frame`, `keel_get_rgb`, `keel_set_button_leds` |
| Buttons | `keel_take_buttons` |
| Simulator | `keel_sim_set`, `keel_sim_get_state` (`KEEL_E_UNSUPPORTED` elsewhere) |

## 5. Status codes and classes

### 5.1 Codes

Zero is success; errors are negative. Every code has exactly one class, independent of the function returning it.

| Code | Value | Class | Meaning |
|---|---|---|---|
| `KEEL_OK` | 0 | OK | success |
| `KEEL_E_INVALID_ARGUMENT` | -1 | REJECTED | NULL pointer, bad `struct_size`, non-zero reserved field, unknown enumerator or flag, value out of range |
| `KEEL_E_IO` | -2 | FAULT | synchronous control-path I/O failed (ENABLE, heartbeat, servo request, button LEDs, READY/ARMED/button read, a motor command written by the calling thread) |
| `KEEL_E_STATE` | -3 | REJECTED | not allowed in this state; also the re-arm hold-off |
| `KEEL_E_HEALTH` | -4 | FAULT | health flags not ALL, `cycle_id` not increasing, or a tick from a non-owner thread |
| `KEEL_E_DEADLINE` | -5 | FAULT | tick gap or heartbeat fall late while ARMING, ARMED or SERVO |
| `KEEL_E_FEEDBACK` | -6 | FAULT | READY/ARMED disagree with the commanded state |
| `KEEL_E_AGAIN` | -7 | REJECTED | temporary: servo cooldown or idle time, ranging spacing, pending or ECHO HIGH; never a safety refusal |
| `KEEL_E_NO_MEMORY` | -8 | SETUP | allocation or handle slot failed at open |
| `KEEL_E_PWM` | -9 | FAULT | failure detected by the PWM worker (overrun, degraded, stall, worker-side write) |
| `KEEL_E_FAULTED` | -10 | FAULT | already latched, nothing done; sticky cause on clear; record status of an emergency stop |
| `KEEL_E_IN_USE` | -11 | SETUP | a line is held by another process, device or kernel function |
| `KEEL_E_PERMISSION` | -12 | SETUP | permission denied on a device node |
| `KEEL_E_NOT_FOUND` | -13 | SETUP | no BCM2711 GPIO chip, no GPIO v2 kernel interface, or backend not built in |
| `KEEL_E_UNSUPPORTED` | -14 | REJECTED | capability absent, simulator call on hardware, oversize input with a non-zero tail |
| `KEEL_E_CONFLICT` | -15 | REJECTED | go call from a non-owner thread, or conflict with an active instruction |
| `KEEL_E_PERIPHERAL` | -16 | PERIPHERAL | RGB LED I2C or ultrasonic failure; safety state unchanged |
| `KEEL_E_ACK_TIMEOUT` | -17 | FAULT | arming not complete within 2 s |
| `KEEL_E_INVALID_HANDLE` | -18 | HANDLE | NULL, closed, stale or forged handle; or closed during a wait |
| `KEEL_E_NOT_OWNER` | -19 | HANDLE | call from a process that did not open the device |
| `KEEL_E_INTERNAL` | -20 | FAULT | clock, interlock, backend contract or invariant failure (sticky) |
| `KEEL_E_SETUP_IO` | -21 | SETUP | other I/O failure during open; everything released |

`keel_status_class()` returns `KEEL_CLASS_FAULT` for any undefined value (the conservative reading).

### 5.2 What a class promises

| Class | Device after the call | Binding mapping (Python / Java / C#) |
|---|---|---|
| OK (0) | as documented | return |
| REJECTED (1) | unchanged; `snapshot.last_rejection` set. Exception: a motion request refused for its arguments or capability while ARMED coasts and drops the stored command | `RejectedError` (refined per code: `InvalidArgumentError`, also a `ValueError`; `StateError`, `AgainError`, `UnsupportedError`, `ConflictError`) / `RejectedException` (the same five refinements) / `ArgumentException`, `InvalidOperationException` |
| FAULT (2) | FAULTED: Category-0 stop done, heartbeat stopped, `keel_get_fault` gives the cause | `SafetyFaultError` (`.fault`) / `SafetyFaultException` / `SafetyFaultException` |
| PERIPHERAL (3) | unchanged | `PeripheralError` / `PeripheralException` / `PeripheralException` |
| SETUP (4) | no device | `SetupError` (`.report`) / `SetupException` / `SetupException` |
| HANDLE (5) | not usable by this caller; loop helpers stop | `HandleError` / `HandleException` / `ObjectDisposedException` |

### 5.3 Call-specific rules

- **`keel_open`** returns only `KEEL_OK`, `KEEL_E_INVALID_ARGUMENT` or SETUP-class codes. Every non-OK result means "no
  device; see the report", and bindings raise their setup error with the report whatever the code. An unsupported
  platform is `KEEL_E_NOT_FOUND`; I/O at open is `KEEL_E_SETUP_IO`; an options struct with a non-zero unknown tail is
  `KEEL_E_INVALID_ARGUMENT` (resource `KEEL_RESOURCE_OPTIONS`).
- **`keel_close(dev, fault*)`** status is diagnostic only: `KEEL_OK`, `KEEL_E_IO` (stop not confirmed),
  `KEEL_E_INVALID_ARGUMENT` (bad `fault` struct; the device is still closed), `KEEL_E_INVALID_HANDLE`, `KEEL_E_NOT_OWNER`.
  Bindings log it and never throw from close, Dispose or a cleaner. `fault` receives the final record including this
  close's `cleanup_status`. `keel_close(NULL, ...)` returns `KEEL_OK`.
- **`keel_cycle`**: bindings treat `KEEL_E_INVALID_ARGUMENT` and `KEEL_E_UNSUPPORTED` from `keel_cycle` as fatal for the
  loop (disarm, stop the loop, report), because they mean a binding defect.

### 5.4 Status strings

`keel_status_name` gives the enumerator name (`"KEEL_E_PWM"`, unknown: `"KEEL_E_UNKNOWN"`), `keel_status_string` one
sentence naming the remedy, `keel_fault_cause_name` the cause without prefix (`"PWM_OVERRUN"`, unknown: `"UNKNOWN"`).
There is no thread-local last error.

## 6. Handles, lifetime and processes

- **One open, one close** for every backend: `keel_open(options|NULL, &dev, report|NULL)` and `keel_close(dev, fault|NULL)`.
  Port backends use `keel_port_attach` (section 19) and the same `keel_close`.
- **Handles are tokens**, `(generation << 8) | (slot + 1)` cast to `keel_device *`, never dereferenced. The registry has
  64 slots per process; each entry holds the device pointer, generation, refcount, closing flag and an atomic
  `owner_pid`. keel_r3 allows one open device per robot (the exclusive line requests enforce it, also within a process).
- **Every entry point** first compares the entry's atomic `owner_pid` with a cached process id, before taking any lock
  and without a system call; the fork handler below refreshes the cached id in the child. It then validates the
  entry's tag (generation and closing bit), takes a reference and validates the tag again, all with atomics and no
  lock, so a stop call never queues behind other callers (a default mutex is not fair: a thread polling in a tight
  loop delayed a disarm by up to 7 ms before). The registry mutex only publishes a device, carries close's wait for
  the calls in flight and is held across `fork()`; it is never held while a device lock is taken.
- **Close** marks the entry closing (new calls get `KEEL_E_INVALID_HANDLE`), calls `emergency_disable`, performs the
  Category-0 stop under CONTROL, releases CONTROL, turns the LEDs off under LEDS, releases ranging under RANGE, calls the
  backend's `cancel_sleep` (sleepers in `keel_sleep_until` and `keel_range_measure` wake and return
  `KEEL_E_INVALID_HANDLE`), waits for the refcount to reach zero (every in-flight call is bounded), calls `destroy`,
  frees the device (hosted path) and bumps the generation. Close is bounded by about 100 ms.
- **Fork.** A `pthread_atfork` child handler, registered once with `pthread_once` at the first open, re-initialises the
  registry mutex, marks every entry foreign (owner pid of the parent) and closes the inherited GPIO, I2C and edge file
  descriptors in the child; the parent's copies are unaffected. Every call in the child, including close, returns
  `KEEL_E_NOT_OWNER` and touches nothing. Fork without exec is unsupported for robot code.
- **Process death.** On SIGKILL the PWM worker dies with the process. The MCU removes motor permission when the
  heartbeat window closes: a design bound of about 86 ms after the last fall (80 ms window, clock tolerance, 1 ms
  sampling); the kill test on the robot measured 51-71 ms (section 24.2).

## 7. Thread ownership: stop from anywhere, go only from the loop

- The **control owner** is the OS thread (backend `thread_id` hook: `gettid()` on Linux) of the first `keel_tick` or
  `keel_cycle` after `keel_open` or `keel_clear_fault` whose tick completes without a fault. Until then every thread
  counts as the owner, so `open; request_arm; loop { tick }` works from one thread or from a main thread followed by a
  loop thread. A fault latch does not reset the owner; only `keel_clear_fault` (and close) does.
- **Tick or cycle from another thread** fail-stops: `KEEL_E_HEALTH`, cause `TICK_THREAD_CHANGED`, detail = the calling
  thread id. (In FAULTED it returns `KEEL_E_FAULTED` like any tick.) This enforces mechanically that the heartbeat
  certifies one platform thread: Java virtual threads and pool continuations that migrate are caught.
- **Go calls from another thread** return `KEEL_E_CONFLICT` with no side effect: `keel_request_arm`, `keel_clear_fault`,
  non-zero `keel_set_motors`, `keel_brake`, `keel_servo_start`, `keel_servo_set_pulse`. The thread check comes after
  argument and capability validation and before every state check.
- **Stop calls are accepted from every thread in every state:** `keel_disarm`, `keel_emergency_stop`,
  `keel_set_motors(dev, 0, 0)`, `keel_servo_end`, `keel_close`. An out-of-range motion command from any thread coasts
  while ARMED (a stop-direction effect).
- **`keel_range_measure`** from the owner while ARMING, ARMED or SERVO returns `KEEL_E_CONFLICT` (it would block the loop).
- All other calls (queries, LEDs, buttons, ranging start/poll, sleep, simulator) are accepted from any thread.
- **Command timeout:** default 500 ms, range 20-1000 ms (`keel_open_options.command_timeout_ms`).
- UI threads post their intent to the loop; binding loop helpers offer a thread-safe `set_command(left, right)` that the
  next cycle applies.
- A port without a `thread_id` hook has no ownership checks. A port without lock hooks must set
  `KEEL_PORT_SINGLE_THREADED`; with a `thread_id` hook the core then refuses calls from any thread other than the
  attaching thread with `KEEL_E_CONFLICT`.

## 8. Locks and stop latency

| Domain | Protects | Rule |
|---|---|---|
| registry mutex (`src/os`) | publishing a handle, close's wait for calls in flight, fork | never held while a device lock is taken; handle validation is lock-free (atomic owner pid, tag and reference count) |
| CONTROL | safety state, heartbeat, acknowledgement, motion, servo, buttons, button LEDs, fault record, snapshot | never held across a sleep or a blocking wait; held only for bookkeeping and single I/O operations (well under 1 ms) |
| LEDS | RGB frame cache and I2C | never taken while CONTROL is held |
| RANGE | ultrasonic state | never taken while CONTROL is held; publishes `last_echo_rise_us` atomically for the snapshot |
| backend lock (keel_r3 PWM lock, simulator model lock) | backend shadows, line writes | a leaf: hooks take it, nothing is called while holding it |

No path holds two domain locks at once, so there is no lock order. Rules:

1. **Never sleep or block while holding CONTROL.** A tick raises the heartbeat under CONTROL, releases CONTROL, sleeps
   1 ms, takes CONTROL again and lowers the heartbeat. If a stop lowered the heartbeat in between, the stop's fall
   timestamp is kept. After re-taking CONTROL the tick re-reads the state (a stop or close may have happened).
2. **Stop first, lock second.** `keel_disarm`, `keel_emergency_stop` and `keel_close` increment the device's atomic
   `stop_epoch` and call the backend's `emergency_disable(epoch)` before taking CONTROL. That hook lowers ENABLE and
   coasts the motors under the backend lock only. The backend suppresses every later rising ENABLE, DRIVE, BRAKE or
   servo request that carries an older epoch (port.h, "Stop epoch"). Every CONTROL section loads the epoch on entry;
   if it changed since the core last handled a stop, that section performs the pending stop itself. A control section
   that was already running therefore cannot undo a stop issued from another thread.
3. **Snapshot and cycle take CONTROL only.** Range data reaches the snapshot through atomics; LED state is not in the
   snapshot.
4. **Close performs its Category-0 stop under CONTROL** before it takes LEDS and RANGE.
5. The keel_r3 PWM lock is held only for a command snapshot and a few line ioctls (the interlock's ARMED read and one
   `GPIO_V2_LINE_SET_VALUES` per request line). It is a priority-inheritance mutex because the SCHED_OTHER control
   thread takes it for every write.
6. **Fairness.** A default mutex is not fair. Measured on the simulator's board model (release build, 2000 disarms
   per run): a thread calling a CONTROL function in a tight loop delayed the bookkeeping half of `keel_disarm` by up
   to 7 ms while ENABLE still fell within microseconds, because it falls before any core lock; a thread calling
   control-I/O functions in a tight loop contends for the backend lock that `emergency_disable` takes, which delayed
   ENABLE by up to 3.5 ms in rare samples (p99.9 stayed under 0.2 ms). Handle validation takes no lock at all
   (section 6). A backend that must bound the second case under such load gives `emergency_disable` a fair (FIFO) or
   uncontended path to the ENABLE line.

Latency target (test, section 23): from the `keel_disarm` call to ENABLE LOW, p99.9 under 2 ms with the CPU loaded and
an I2C stub that hangs every transaction for its 20 ms timeout.

## 9. Safety state machine

### 9.1 States

```
            request_arm (owner)              acknowledgement complete (READY = ARMED = 1)
 DISARMED ----------------------> ARMING -------------------------------------------> ARMED
    ^   ^          disarm               |  disarm                                       |   | servo_start
    |   +-------------------------------+-----------------------------------------------+   v
    |                                                                                  SERVO --(lease end,
    |      clear_fault (owner; not sticky)                                                   servo_end,
    +----------------------------- FAULTED <--- any FAULT detection or emergency_stop         disarm)--> DISARMED
```

| State | ENABLE | Heartbeat | Motors | Servo |
|---|---|---|---|---|
| DISARMED | LOW | running (sensor power available) | coast | off |
| ARMING | acknowledgement levels | running | coast | off |
| ARMED | HIGH | running | coast, drive or brake | off |
| SERVO | HIGH (MCU grants servo, ARMED = 0) | running | coast | request HIGH, pulse after grant |
| FAULTED | LOW | stopped | coast | off |

The acknowledgement sub-phase (LOW1, HIGH1, LOW2, HIGH2) is a diagnostic (`ack_phase`), not a state.

### 9.2 Heartbeat

- The first pulse is emitted by a tick at least 30 ms after open (and immediately after a restart).
- A tick emits a pulse when the fall would come at least 30 ms after the previous fall: predicted interval =
  now + 1 ms - last fall.
- Each pulse is 1 ms HIGH, with CONTROL released during the HIGH time (section 8).
- **Late heartbeat.** In ARMING, ARMED or SERVO, a predicted interval over 70 ms fails before pulsing, and an actual fall
  more than 74 ms after the previous one fails after the pulse. Both latch `KEEL_E_DEADLINE`, cause `HEARTBEAT_LATE`,
  detail = interval in us.
- **Tick gap.** In ARMING, ARMED or SERVO, a gap over 70 ms since the previous tick fails with `KEEL_E_DEADLINE`, cause
  `TICK_LATE`, detail = gap in us.
- **DISARMED tolerance.** The state that matters is the state of the previous tick. If the previous tick ran while
  DISARMED (including the first tick after `keel_request_arm`), a long gap or a late fall restarts the heartbeat. The
  restart increments `heartbeat_restarts`, sets `KEEL_SNAP_HEARTBEAT_RESTARTED` and returns OK. The MCU will have latched
  F_HEARTBEAT and re-qualifies by itself after two valid falls and 100 ms of quiet; the cost is a sensor-power gap of
  about 0.2 s.
- An acknowledgement level write requires the last fall within 70 ms; otherwise `HEARTBEAT_LATE`.

### 9.3 Tick structure

One tick, in order:
1. Handle and owner-pid check.
2. CONTROL: apply any pending stop (epoch).
3. `read_inputs`. A `KEEL_IN_BACKEND_FAULT` bit while not FAULTED latches the backend's record first (section 12.3).
4. In FAULTED: debounce the buttons, return `KEEL_E_FAULTED`.
5. Thread check: a non-owner fails with `TICK_THREAD_CHANGED`.
6. Health and `cycle_id` checks.
7. Clock check: a backwards step fails with cause CLOCK.
8. Tick gap and predicted heartbeat interval (section 9.2).
9. Debounce the buttons; evaluate feedback and advance the state (9.4 to 9.6); renew the lease (section 10).
10. If a pulse is due: raise the heartbeat, release CONTROL, sleep 1 ms, re-take CONTROL, lower the heartbeat and check
    the actual fall time.
11. Record the owner on the first successful tick after open or clear.

### 9.4 Acknowledgement

`keel_request_arm` from DISARMED (owner, outside the hold-off) re-asserts ENABLE LOW and COAST, then enters ARMING with
`ack_started = phase_at = now` measured after the writes complete. On each tick in ARMING:

- **Timeout.** If more than 2 s have passed since `ack_started`: `KEEL_E_ACK_TIMEOUT`, cause `ACK_NO_READY` in LOW1,
  otherwise `ACK_NO_ARMED`.
- **LOW1.** ARMED HIGH fails with FEEDBACK `ACK_FEEDBACK`. Once READY is HIGH and 150 ms have passed, write ENABLE HIGH
  and enter HIGH1.
- **HIGH1, LOW2, HIGH2.** READY LOW fails with FEEDBACK `READY_LOST`.
- **HIGH1.** ARMED HIGH at 90 ms or later is a stale completion: the MCU was still in its second-HIGH wait from an
  interrupted acknowledgement. Write ENABLE LOW, return to LOW1 and count `stale_ack_restarts`. This restart is allowed
  once per `keel_request_arm`, and `ack_started` is not reset. ARMED HIGH before 90 ms, or a second stale completion,
  fails with `ACK_FEEDBACK` (detail = phase).
- **LOW2.** ARMED HIGH fails with `ACK_FEEDBACK`.
- **Transitions.** After 150 ms, HIGH1 goes to LOW2 (ENABLE LOW) and LOW2 goes to HIGH2 (ENABLE HIGH). After 150 ms in
  HIGH2 with ARMED HIGH, the state becomes ARMED.
- All levels are timed from the completion of their write. Motor inputs and the servo request stay LOW throughout; the
  backend interlocks enforce it (section 17.5).

### 9.5 Feedback rules by state

| State | Rule | Cause |
|---|---|---|
| DISARMED | ARMED HIGH more than 10 ms after ENABLE went LOW | `ARMED_UNEXPECTED` (sticky): the AND gate no longer removes enable |
| DISARMED | READY LOW | not a fault; feeds sensor-power inference and restarts the servo cooldown |
| ARMING | as section 9.4 | `ACK_*`, `READY_LOST` |
| ARMED | READY LOW (checked first), then ARMED LOW, on every tick and every fresh read of a non-zero `set_motors`, `brake` or `servo_start` | `READY_LOST`, `ARMED_LOST` |
| SERVO | READY LOW | `READY_LOST` |
| SERVO | ARMED still HIGH 10 ms after the request | `SERVO_NOT_GRANTED` |
| SERVO | ARMED HIGH after the grant was confirmed | `DUAL_PERMIT` |

### 9.6 Sensor-power inference (`snapshot.sensor_power`)

- OFF: the heartbeat is stopped (FAULTED, closed, or no fall for 70 ms, the tick window: the MCU's own 80 ms window
  ends earlier on a fast MCU clock), or READY has been LOW for at least 150 ms while the heartbeat ran (the MCU is
  latched).
- UNKNOWN: the heartbeat runs but is not yet qualified (fewer than two falls since it started), or READY has been LOW
  for less than 150 ms.
- PERMITTED: the heartbeat is qualified and READY is HIGH. An MCU strike lockout is invisible to the Pi.
- CONFIRMED: PERMITTED, and an echo rose within the last 500 ms.

## 10. Motion

- **Duty.** Signed permille -1000..1000. Positive is forward: left forward drives BCM11 with BCM8 LOW, right forward
  drives BCM10 with BCM9 LOW. Zero coasts. Brake is a separate verb, so brake and shoot-through cannot be expressed by a
  duty.
- **Acceptance** of a non-zero `keel_set_motors` (or a cycle DRIVE) requires, in order:
  - valid duties;
  - the owner thread;
  - not FAULTED;
  - ARMED (else `KEEL_E_STATE` from DISARMED or ARMING, `KEEL_E_CONFLICT` from SERVO);
  - the tick window still open (now - last tick at most 70 ms, else `KEEL_E_DEADLINE`, cause `TICK_LATE`);
  - a fresh read with no backend latch, READY = ARMED = 1.

  On acceptance the command is stored with `expiry = now + command_timeout_ms` and driven.
- **Deadman (TTL).** Every accepted non-zero command or brake renews the expiry; `(0, 0)` clears it. Each armed tick
  passes `valid_until = min(expiry, now + 70 ms)` to the backend, which coasts at `valid_until` without renewal. When
  `expiry - now` is under 1 ms the core writes an explicit COAST and sets `KEEL_SNAP_COMMAND_EXPIRED`. This is not a
  fault and the device stays ARMED. Loops re-send their command every cycle.
- **Deferred reversal.** If a wheel would change direction without having been idle (coast or brake) for 20 ms, both
  motors coast now (synchronous COAST). The command is stored as pending, `KEEL_SNAP_REVERSAL_PENDING` is set and OK is
  returned. The first tick or command at least 20 ms after the coast applies it, using that call's fresh READY/ARMED
  sample and the pending command's own expiry. A newer command replaces a pending one; `(0, 0)`, brake, disarm and
  every stop clear it. The timeout of a pending command runs from its acceptance.
- **Brake** (`keel_brake`, cycle BRAKE). ARMED and owner only; all four inputs HIGH, with the same timeout rules as a
  drive; clears a pending reversal; counts as idle time for a later reversal but as PWM activity for the servo. No stop
  path ever brakes. Without `KEEL_CAP_BRAKE` it is `KEEL_E_UNSUPPORTED`, and while ARMED the motors coast.
- **Invalid motion while ARMED.** An out-of-range duty, an unknown cycle command or flag, a bad cycle `out`, or a brake
  without the capability does the following:
  - stores COAST, clears the expiry, the pending reversal and the brake;
  - sets `command_left/right` to 0 and writes COAST synchronously;
  - returns `KEEL_E_INVALID_ARGUMENT` (`KEEL_E_UNSUPPORTED` for the brake).

  The next tick renews nothing. This is the only REJECTED result with a side effect, and it is a stop-direction one.
- **`(0, 0)`** is accepted from any thread in every state. While ARMED it coasts synchronously and drops the stored
  command, expiry, pending reversal and brake; elsewhere it does nothing.
- **Lease-expiry race.** The backend treats a DRIVE or BRAKE whose `valid_until` is at or before now as a synchronous
  coast that returns OK and counts `lease_expiries`. The worker stores COAST when a lease expires, so a later renewal
  starts from idle (new phase epoch).

## 11. Stop vocabulary (IEC 60204-1 categories)

| Verb | Category | Effect | Latching | Leave with |
|---|---|---|---|---|
| `keel_disarm` | 0 | ENABLE LOW at once (emergency_disable), then coast, servo pulse off, request LOW; heartbeat continues (except in FAULTED) | no | `keel_request_arm` |
| `keel_emergency_stop` | 0 | as disarm, then heartbeat LOW and stopped; FAULTED with cause `USER_STOP` | yes | `keel_clear_fault` (owner), then arm |
| any FAULT detection | 0 | as emergency stop, with the detected cause | yes | `keel_clear_fault` (owner) unless sticky |
| `keel_close` | 0 | as emergency stop, then LEDs off and release | n/a | reopen |
| `keel_set_motors(dev, 0, 0)` | 2 (coast) | both bridges coast, stays ARMED | no | next command |
| `keel_brake` | 2 (brake) | all inputs HIGH for the command timeout, stays ARMED | no | next command |

**Safe-stop sequence** (shared by disarm, emergency stop, fault latch and close; every step is attempted even if an
earlier one fails, and every step is idempotent):
1. `emergency_disable(epoch)`: ENABLE LOW and motors COAST under the backend lock;
2. COAST through `write_motors`;
3. servo `OFF` (pulse stopped, then request LOW) where a servo hook exists;
4. heartbeat LOW, for latching stops and in FAULTED.

`keel_disarm` and `keel_close` re-run the whole sequence in every state, including FAULTED. They return `KEEL_E_IO` with
`os_error` if any step failed, and they update `cleanup_status` and `cleanup_os_error` in the fault record on every
attempt. A disarm whose stop fails while not FAULTED latches FAULTED with that `KEEL_E_IO`. A disarm that finds a
backend latch performs the stop, records the backend's cause and returns its status.

Bindings map their stop button and Ctrl-C to `keel_emergency_stop`; normal program flow uses `keel_disarm`.
No libkeel function is async-signal-safe: a signal handler only sets a `volatile sig_atomic_t` flag (or
writes to a pipe), and a normal thread (the loop, or one that waits in `sigwait()` or reads a `signalfd`) calls
`keel_emergency_stop`. Called from the handler itself it can deadlock on a lock the interrupted thread holds.

## 12. Fault model and cause catalogue

### 12.1 Latching

A FAULT-class detection in any call does the following, in order:
1. captures the fault record before any cleanup: status, cause, state, ack phase, last inputs, the backend's `os_error`
   and detail when the backend supplied the cause, and the detection time;
2. runs the safe-stop sequence with the heartbeat stopped and stores its result in `cleanup_status` and
   `cleanup_os_error`;
3. latches FAULTED and increments `fault_count`;
4. returns the primary status, never the cleanup status.

Only the first fault since the last clear writes the record (first fault wins). While FAULTED, calls return their
documented FAULTED results and the record is kept, with one addition: a sticky cause found while already FAULTED (for
example a stop hook that breaks the port contract, `HAL_CONTRACT`) sets `KEEL_FAULT_STICKY` on the kept record, so
`keel_clear_fault` refuses it as well.

A stop call (`keel_disarm`, `keel_emergency_stop`, `keel_close`) lowers ENABLE through its stop epoch before it takes
CONTROL, so its own CONTROL section finds the device already DISARMED. A record it writes (`USER_STOP`, or a backend
latch or stop failure it finds) names the state and acknowledgement phase that stop ended, for example ARMED, not
DISARMED.

### 12.2 Status pass-through and classes through the hardware layer

- On a safety path the core passes a backend status through only if it is FAULT-class. Any other non-OK value from a
  safety-path hook latches `KEEL_E_INTERNAL`, cause `HAL_CONTRACT`, detail = the raw value sign-extended to 64 bits.
- Peripheral hooks (`i2c_write`, `range_trigger`, `range_edges`) map any failure to `KEEL_E_PERIPHERAL`, never latching.
- Backends map errno to `KEEL_E_IO` with `os_error` in their fault record, never to AGAIN or a busy code.
- Synchronous motor writes by the calling thread fail with `KEEL_E_IO` (cause `MOTOR_WRITE`); failures detected by the
  PWM worker are `KEEL_E_PWM`.

### 12.3 The backend latch has priority

- Backends latch a record for every failure they report from a safety-path hook and every failure their own threads
  detect. While latched, `read_inputs` returns OK with the real levels plus the pseudo-bit `KEEL_IN_BACKEND_FAULT`.
- Every entry point that performs control I/O begins its CONTROL section with `read_inputs` and checks that bit
  first. Stop calls check it right after `emergency_disable`. If the bit is set and the device is not FAULTED, the core
  reads `get_fault` and latches with the backend's status and cause before evaluating anything else. A set bit with a
  non-FAULT record, or no record, is `HAL_CONTRACT`.
- Cached calls (`keel_get_*`, `keel_now_us`) and refusals decided from cached state do no I/O and do not consult the latch.
- A backend thread's self-stop lowers ENABLE and SERVO_REQUEST and coasts; it never touches the heartbeat.
- Only `keel_clear_fault` (and open) calls `reset_fault`.
- Regression (named test): a PWM worker OVERRUN injected between two ticks with no pulse due gives
  `keel_take_buttons` = `KEEL_E_PWM` and `keel_get_fault().cause` = `PWM_OVERRUN`, not `ARMED_LOST` or `E_IO`.

### 12.4 Recovery

- `keel_request_arm` in FAULTED returns `KEEL_E_FAULTED`; only `keel_clear_fault` leaves FAULTED.
- A 1 s **re-arm hold-off**, measured from the latch time, is enforced on `keel_request_arm` in every state: it returns
  `KEEL_E_STATE` with `KEEL_SNAP_REARM_HOLDOFF` set. Never `KEEL_E_AGAIN`, which generic retry libraries act on.
- **Sticky causes**: `ARMED_UNEXPECTED`, `INTERLOCK`, `INTERNAL`, `CLOCK`, `HAL_CONTRACT`. `keel_clear_fault` returns
  `KEEL_E_FAULTED` for them until close and reopen; the record carries `KEEL_FAULT_STICKY` and the snapshot
  `KEEL_SNAP_FAULT_STICKY`.
- **`keel_clear_fault`** (owner thread; any thread while no owner is recorded):
  1. re-runs the safe stop;
  2. calls `reset_fault`, then reads the inputs again: a backend latch that persists keeps the device FAULTED and
     returns the backend status;
  3. flags the record `KEEL_FAULT_CLEARED`;
  4. forgets the control owner and enters DISARMED. The next tick restarts the heartbeat.
- `keel_get_fault` keeps returning the last record, with CLEARED, until the next latch replaces it.
- Bindings never re-arm or clear automatically after a fault.

### 12.5 Fault-cause catalogue

| # | Cause | Status | Detected in (by) | Detail | Sticky |
|---|---|---|---|---|---|
| 0 | `NONE` | - | - | - | - |
| 1 | `HEALTH_FLAGS` | `E_HEALTH` | DISARMED, ARMING, ARMED, SERVO (tick, cycle) | missing health bits | no |
| 2 | `CYCLE_NOT_INCREASING` | `E_HEALTH` | same | rejected cycle_id | no |
| 3 | `TICK_THREAD_CHANGED` | `E_HEALTH` | same, from a non-owner thread | calling thread id | no |
| 4 | `TICK_LATE` | `E_DEADLINE` | ARMING, ARMED, SERVO (tick gap > 70 ms); ARMED (`set_motors`, `brake`, `servo_start` with the tick window closed) | gap in us | no |
| 5 | `HEARTBEAT_LATE` | `E_DEADLINE` | ARMING, ARMED, SERVO (predicted interval > 70 ms or fall > 74 ms; acknowledgement write with the last fall > 70 ms ago) | interval in us | no |
| 6 | `READY_LOST` | `E_FEEDBACK` | ARMING after LOW1, ARMED, SERVO | - | no |
| 7 | `ARMED_LOST` | `E_FEEDBACK` | ARMED (tick, fresh reads, backend DRIVE/BRAKE check) | - | no |
| 8 | `ARMED_UNEXPECTED` | `E_FEEDBACK` | DISARMED, ARMED HIGH > 10 ms after ENABLE LOW | - | yes |
| 9 | `ACK_NO_READY` | `E_ACK_TIMEOUT` | ARMING (LOW1 at 2 s) | - | no |
| 10 | `ACK_NO_ARMED` | `E_ACK_TIMEOUT` | ARMING (after LOW1 at 2 s) | - | no |
| 11 | `ACK_FEEDBACK` | `E_FEEDBACK` | ARMING (ARMED in LOW1, LOW2, HIGH1 < 90 ms, or a second stale completion) | ack phase | no |
| 12 | `SERVO_NOT_GRANTED` | `E_FEEDBACK` | SERVO (ARMED HIGH 10 ms after the request) | - | no |
| 13 | `DUAL_PERMIT` | `E_FEEDBACK` | SERVO (ARMED HIGH after the grant) | - | no |
| 14 | `PWM_OVERRUN` | `E_PWM` | ARMED (PWM worker: one wake > 30 ms late; recorded by the next entry point that reads the inputs) | lateness in us | no |
| 15 | `PWM_DEGRADED` | `E_PWM` | ARMED (PWM worker: sum of lateness over 1 ms in a sliding 100 ms window > 20 ms) | excess in us | no |
| 16 | `PWM_STALL` | `E_PWM` | ARMED (`write_motors` finds an unexpired DRIVE/BRAKE and the worker's scheduled wake > 25 ms past) | age in us | no |
| 17 | `PWM_WRITE` | `E_PWM` | worker motor-line write failed | - (os_error) | no |
| 18 | `GPIO_WRITE` | `E_IO` | any state (control output write) | logical output mask | no |
| 19 | `GPIO_READ` | `E_IO` | any state (input read) | - (os_error) | no |
| 20 | `MOTOR_WRITE` | `E_IO` | ARMED (synchronous motor write) | - (os_error) | no |
| 21 | `CLOCK` | `E_INTERNAL` | any (clock backwards, sleep hook failure) | step back in us | yes |
| 22 | `INTERLOCK` | `E_INTERNAL` | backend interlock refusal (section 17.5) | - | yes |
| 23 | `HAL_CONTRACT` | `E_INTERNAL` | any (non-FAULT status on a safety path, latch without a record) | raw status | yes |
| 24 | `INTERNAL` | `E_INTERNAL` | any other invariant | internal code | yes |
| 25 | `USER_STOP` | `E_FAULTED` (record status) | `keel_emergency_stop` | - | no |

## 13. Evaluation order and the arbitration table

### 13.1 Evaluation order

Every call is evaluated in this order. The first rule that applies decides the outcome.

1. **Process and handle:** `KEEL_E_NOT_OWNER` (atomic, before any lock), `KEEL_E_INVALID_HANDLE`. Nothing touched.
2. **Stop calls lower ENABLE** (`emergency_disable`) before anything below.
3. **Arguments:** pointers, `struct_size`, ranges, enumerators, reserved bits. `KEEL_E_INVALID_ARGUMENT`, or
   `KEEL_E_UNSUPPORTED` for an oversize input with a non-zero tail. There are two exceptions. A refused motion request
   while ARMED coasts (section 10). `keel_cycle` with a usable `in` evaluates its command arguments only after the tick.
4. **Capability:** `KEEL_E_UNSUPPORTED`.
5. **Thread:** go calls from a non-owner get `KEEL_E_CONFLICT`. Ticks and cycles follow section 9.3 instead: the
   inputs and the backend latch first, then FAULTED, then the owner check (`TICK_THREAD_CHANGED`).
6. **FAULTED:** go calls get `KEEL_E_FAULTED`; `keel_clear_fault` with a sticky cause gets `KEEL_E_FAULTED`.
7. **Hold-off** (`keel_request_arm`): `KEEL_E_STATE` with `REARM_HOLDOFF`.
8. **State:** `KEEL_E_STATE`.
9. **Conflict:** `KEEL_E_CONFLICT` (servo burst active, motors driven or braked, reversal pending).
10. **Timing:** `KEEL_E_AGAIN` (cooldown, idle time, ranging spacing).
11. **Control I/O:** `read_inputs`, then the backend latch, the clock, the tick window and feedback, in that order. Any
    failure here is a FAULT.
12. **Action**, with every hook status passed through the rules of section 12.2.

Steps 1 and 3 to 10 use only cached state, so every REJECTED result is decided without I/O. Every REJECTED result
records its status in `snapshot.last_rejection`.

### 13.2 Request-versus-state table

The table below is generated from `tests/integration/arbitration_table.h` by `tests/docs/arbitration_render.c`, and
`tests/docs/check_arbitration_doc.sh` fails if the two differ. The arbitration integration test replays
every reachable cell against the simulator, asserting:
- the status and the recorded cause;
- the state after the call;
- the side effects (outputs, MCU stage);
- heartbeat continuity: the heartbeat keeps running after every REJECTED result outside FAULTED.

Legend:
- "owner" means the control owner, or any thread while none is recorded. "other" means another thread of the process
  while an owner is recorded. "any" means both columns agree.
- `E_X` means `KEEL_E_X`. "-> STATE" shows a state change, and a cause in brackets is the cause the call records.
- "HB" says whether the heartbeat keeps running afterwards, given on-time ticks.
- Cells assume nominal I/O; injected I/O failures follow section 12 and are covered by the error-propagation matrix.
- Invalid handles always give `E_INVALID_HANDLE` with nothing touched; `keel_open` and the metadata calls do not depend
  on a device.

<!-- arbitration-table:begin -->
| Call | Condition | Thread | DISARMED | ARMING | ARMED | SERVO | FAULTED |
|---|---|---|---|---|---|---|---|
| `keel_tick` | on time, health ALL, cycle_id increasing [1] | owner | OK; sample, pulse; HB yes | OK; sample, pulse, ack; HB yes | OK; sample, pulse, lease; HB yes | OK; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_tick` | on time, health ALL, cycle_id increasing | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | more than 70 ms after the previous tick, which ran in the same state [2] | owner | OK; sample, pulse, hb restart; HB yes | E_DEADLINE (TICK_LATE) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_DEADLINE (TICK_LATE) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_DEADLINE (TICK_LATE) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | more than 70 ms after the previous tick, which ran in the same state | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | heartbeat fall lands more than 74 ms after the previous fall (previous tick in the same state) [3] | owner | OK; sample, pulse, hb restart; HB yes | E_DEADLINE (HEARTBEAT_LATE) -> FAULTED; sample, pulse, Cat-0 stop, hb stop, latch; HB no | E_DEADLINE (HEARTBEAT_LATE) -> FAULTED; sample, pulse, Cat-0 stop, hb stop, latch; HB no | E_DEADLINE (HEARTBEAT_LATE) -> FAULTED; sample, pulse, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | heartbeat fall lands more than 74 ms after the previous fall (previous tick in the same state) | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | health is not KEEL_HEALTH_ALL | owner | E_HEALTH (HEALTH_FLAGS) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (HEALTH_FLAGS) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (HEALTH_FLAGS) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (HEALTH_FLAGS) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | health is not KEEL_HEALTH_ALL | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | cycle_id not greater than the last accepted one | owner | E_HEALTH (CYCLE_NOT_INCREASING) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (CYCLE_NOT_INCREASING) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (CYCLE_NOT_INCREASING) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (CYCLE_NOT_INCREASING) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | cycle_id not greater than the last accepted one | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | in is NULL or in->struct_size is below the 1.0 size [4] | any | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB no |
| `keel_cycle` | in is larger than this library knows and its tail is not zero [5] | any | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB no |
| `keel_cycle` | valid in, command NONE (tick on time) [6] | owner | OK; sample, pulse; HB yes | OK; sample, pulse, ack; HB yes | OK; sample, pulse, lease; HB yes | OK; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command NONE (tick on time) | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command DRIVE with a valid non-zero duty [7] | owner | E_STATE; sample, pulse; HB yes | E_STATE; sample, pulse, ack; HB yes | OK; sample, pulse, lease, drive; HB yes | E_CONFLICT; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command DRIVE with a valid non-zero duty | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command DRIVE with (0, 0) | owner | OK; sample, pulse; HB yes | OK; sample, pulse, ack; HB yes | OK; sample, pulse, coast; HB yes | OK; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command DRIVE with (0, 0) | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command BRAKE | owner | E_STATE; sample, pulse; HB yes | E_STATE; sample, pulse, ack; HB yes | OK; sample, pulse, lease, brake; HB yes | E_CONFLICT; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command BRAKE | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in; unknown command or flag, duty out of range, or bad out [8] | owner | E_INVALID_ARGUMENT; sample, pulse; HB yes | E_INVALID_ARGUMENT; sample, pulse, ack; HB yes | E_INVALID_ARGUMENT; sample, pulse, coast; HB yes | E_INVALID_ARGUMENT; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in; unknown command or flag, duty out of range, or bad out | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_request_arm` | no fault latched within the last 1 s | owner | OK -> ARMING; ack; HB yes | OK; none; HB yes | OK; none; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_request_arm` | no fault latched within the last 1 s | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_request_arm` | a fault latched less than 1 s ago [9] | owner | E_STATE; hold-off; HB yes | unreachable | unreachable | unreachable | E_FAULTED; none; HB no |
| `keel_request_arm` | a fault latched less than 1 s ago | other | E_CONFLICT; none; HB yes | unreachable | unreachable | unreachable | E_CONFLICT; none; HB no |
| `keel_disarm` | any [10] | any | OK; Cat-0 stop; HB yes | OK -> DISARMED; Cat-0 stop; HB yes | OK -> DISARMED; Cat-0 stop; HB yes | OK -> DISARMED; Cat-0 stop, servo end; HB yes | OK; Cat-0 stop, hb stop; HB no |
| `keel_emergency_stop` | any [11] | any | OK (USER_STOP) -> FAULTED; Cat-0 stop, hb stop, latch; HB no | OK (USER_STOP) -> FAULTED; Cat-0 stop, hb stop, latch; HB no | OK (USER_STOP) -> FAULTED; Cat-0 stop, hb stop, latch; HB no | OK (USER_STOP) -> FAULTED; Cat-0 stop, hb stop, latch, servo end; HB no | OK; Cat-0 stop, hb stop; HB no |
| `keel_clear_fault` | recorded cause not sticky (or not FAULTED) [12] | owner | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK -> DISARMED; Cat-0 stop, clear; HB yes |
| `keel_clear_fault` | recorded cause not sticky (or not FAULTED) | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_clear_fault` | sticky cause (ARMED_UNEXPECTED, INTERLOCK, INTERNAL, CLOCK, HAL_CONTRACT) [13] | owner | unreachable | unreachable | unreachable | unreachable | E_FAULTED; none; HB no |
| `keel_clear_fault` | sticky cause (ARMED_UNEXPECTED, INTERLOCK, INTERNAL, CLOCK, HAL_CONTRACT) | other | unreachable | unreachable | unreachable | unreachable | E_CONFLICT; none; HB no |
| `keel_set_motors` | (0, 0) | any | OK; none; HB yes | OK; none; HB yes | OK; coast; HB yes | OK; none; HB yes | OK; none; HB no |
| `keel_set_motors` | valid non-zero duty, no reversal [14] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | OK; drive; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_set_motors` | valid non-zero duty, no reversal | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_set_motors` | valid; a wheel changes direction without 20 ms idle [15] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | OK; reversal; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_set_motors` | valid; a wheel changes direction without 20 ms idle | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_set_motors` | a duty outside -1000..1000 [16] | any | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; coast; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB no |
| `keel_brake` | KEEL_CAP_BRAKE [17] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | OK; brake; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_brake` | KEEL_CAP_BRAKE | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_servo_start` | KEEL_CAP_SERVO; valid arguments; motors coasting 20 ms; cooldown elapsed [18] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | OK -> SERVO; servo request; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_servo_start` | KEEL_CAP_SERVO; valid arguments; motors coasting 20 ms; cooldown elapsed | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_servo_start` | KEEL_CAP_SERVO; a motor driven or braked, or a reversal pending [19] | owner | unreachable | unreachable | E_CONFLICT; none; HB yes | unreachable | unreachable |
| `keel_servo_start` | KEEL_CAP_SERVO; a motor driven or braked, or a reversal pending | other | unreachable | unreachable | E_CONFLICT; none; HB yes | unreachable | unreachable |
| `keel_servo_start` | KEEL_CAP_SERVO; cooldown running, or motors coasting for less than 20 ms | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | E_AGAIN; none; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_servo_start` | KEEL_CAP_SERVO; cooldown running, or motors coasting for less than 20 ms | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_servo_start, keel_servo_set_pulse` | no KEEL_CAP_SERVO (keel_r3 on R3: J5 not fitted) [20] | any | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | unreachable | E_UNSUPPORTED; none; HB no |
| `keel_servo_set_pulse` | KEEL_CAP_SERVO; pulse in range [21] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | E_STATE; none; HB yes | OK; pulse width; HB yes | E_FAULTED; none; HB no |
| `keel_servo_set_pulse` | KEEL_CAP_SERVO; pulse in range | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_servo_end` | any | any | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK -> DISARMED; servo end; HB yes | OK; none; HB no |
| `keel_range_start` | KEEL_CAP_RANGE; sensor power PERMITTED or CONFIRMED; idle; spacing elapsed; ECHO LOW [22] | any | OK; trigger, read; HB yes | OK; trigger, read; HB yes | OK; trigger, read; HB yes | OK; trigger, read; HB yes | unreachable |
| `keel_range_start` | KEEL_CAP_RANGE; sensor power OFF or UNKNOWN [23] | any | OK; unavailable; HB yes | OK; unavailable; HB yes | OK (loop stalled); unavailable; HB no | OK (loop stalled); unavailable; HB no | OK; unavailable; HB no |
| `keel_range_start` | KEEL_CAP_RANGE; sensor powered; measurement pending, under 60 ms since the trigger, or ECHO HIGH | any | E_AGAIN; none; HB yes | E_AGAIN; none; HB yes | E_AGAIN; none; HB yes | E_AGAIN; none; HB yes | unreachable |
| `keel_range_poll` | KEEL_CAP_RANGE | any | OK; result; HB yes | OK; result; HB yes | OK; result; HB yes | OK; result; HB yes | OK; result; HB no |
| `keel_range_measure` | KEEL_CAP_RANGE; spacing elapsed; sensor powered unless FAULTED [24] | owner | OK; trigger, wait <= 35 ms, read; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | OK; unavailable; HB no |
| `keel_range_measure` | KEEL_CAP_RANGE; spacing elapsed; sensor powered unless FAULTED | other | OK; trigger, wait <= 35 ms, read; HB yes | OK; trigger, wait <= 35 ms, read; HB yes | OK; trigger, wait <= 35 ms, read; HB yes | OK; trigger, wait <= 35 ms, read; HB yes | OK; unavailable; HB no |
| `keel_set_rgb, keel_set_rgb_all, keel_set_rgb_frame` | KEEL_CAP_RGB; valid arguments [25] | any | OK; I2C frame; HB yes | OK; I2C frame; HB yes | OK; I2C frame; HB yes | OK; I2C frame; HB yes | OK; I2C frame; HB no |
| `keel_set_button_leds` | mask within KEEL_BUTTON_ALL [26] | any | OK; button LEDs; HB yes | OK; button LEDs; HB yes | OK; button LEDs; HB yes | OK; button LEDs; HB yes | OK; button LEDs; HB no |
| `keel_take_buttons` | valid out [27] | any | OK; read, take edges; HB yes | OK; read, take edges; HB yes | OK; read, take edges; HB yes | OK; read, take edges; HB yes | OK; read, take edges; HB no |
| `keel_read_safety_inputs` | valid out | any | OK; read; HB yes | OK; read; HB yes | OK; read; HB yes | OK; read; HB yes | OK; read; HB no |
| `keel_get_state, keel_get_snapshot, keel_get_fault, keel_get_capabilities, keel_get_rgb, keel_now_us` | valid out [28] | any | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK; none; HB no |
| `keel_sleep_until` | deadline at most 10 s ahead [29] | any | OK; sleep; HB yes | OK; sleep; HB yes | OK; sleep; HB yes | OK; sleep; HB yes | OK; sleep; HB no |
| `keel_sim_set, keel_sim_get_state` | simulator device; valid item [30] | any | OK; sim; HB yes | OK; sim; HB yes | OK; sim; HB yes | OK; sim; HB yes | OK; sim; HB no |
| `keel_close` | any [31] | any | OK -> closed; Cat-0 stop, hb stop, release; HB no | OK -> closed; Cat-0 stop, hb stop, release; HB no | OK -> closed; Cat-0 stop, hb stop, release; HB no | OK -> closed; Cat-0 stop, hb stop, servo end, release; HB no | OK -> closed; Cat-0 stop, hb stop, release; HB no |
| `any call` | from a process that did not open the device (forked child) [32] | any | E_NOT_OWNER; none; HB yes | E_NOT_OWNER; none; HB yes | E_NOT_OWNER; none; HB yes | E_NOT_OWNER; none; HB yes | E_NOT_OWNER; none; HB no |

Notes:

1. ARMING becomes ARMED when the acknowledgement completes; SERVO becomes DISARMED when the lease ends. The first successful tick records the control owner.
2. A tick whose previous tick ran while DISARMED (the first after keel_request_arm) restarts the heartbeat instead of failing.
3. A fall predicted more than 70 ms after the previous one gives the same result before pulsing.
4. No tick runs; the heartbeat ages until the next valid tick.
5. No tick runs; bindings treat this as fatal for the loop.
6. Identical to the on-time keel_tick row; out receives the snapshot.
7. Reversal deferral applies as for keel_set_motors.
8. Checked after the tick; while ARMED the motors coast. Bindings treat this as fatal.
9. Arming cannot be in progress within 1 s of a latch, so only DISARMED and FAULTED occur.
10. ENABLE falls before any lock is taken; the whole safe stop is re-run in every state.
11. Record status E_FAULTED, cause USER_STOP; in FAULTED the existing record is kept.
12. The heartbeat resumes at the next tick, which records the new control owner.
13. A sticky record exists only while FAULTED; close and reopen to recover.
14. The command lives for the command timeout (default 500 ms), then coasts without a fault.
15. The first tick or command at least 20 ms after the coast applies the pending command.
16. Arguments are checked before the thread, so any thread's invalid command coasts while ARMED.
17. Without the capability: KEEL_E_UNSUPPORTED from any thread, coasting while ARMED.
18. The pulse starts on the tick that confirms the grant (ARMED 0, READY 1 within 10 ms).
19. Motors only drive, brake or hold a pending reversal while ARMED.
20. The capability is checked before the thread and the state; keel_servo_end stays a no-op.
21. Never extends the lease.
22. FAULTED has no heartbeat, so the sensor is never powered there.
23. On-time ticks keep READY HIGH and the heartbeat qualified in ARMED and SERVO. There the sensor is unpowered only after the loop stalled past 70 ms (or the MCU dropped READY since the last tick, which the fresh READY read finds): the state shown is the cached one until the next tick latches, and the heartbeat has already stopped.
24. The control owner may not block while a heartbeat deadline is armed.
25. An I2C failure returns KEEL_E_PERIPHERAL and never latches.
26. A GPIO failure is KEEL_E_IO and latches (the lines share the control request).
27. A backend latch found by the read is recorded first (for example E_PWM after a worker overrun).
28. Cached reads: no I/O.
29. Returns KEEL_E_INVALID_HANDLE if keel_close starts during the sleep.
30. On other devices: KEEL_E_UNSUPPORTED, nothing done.
31. The Category-0 stop runs under the control lock before the LED and range locks are taken.
32. Checked atomically before any lock; the parent's device is untouched.
<!-- arbitration-table:end -->

## 14. Servo (J5)

The API is present in 1.0. keel_r3 reports no `KEEL_CAP_SERVO` because J5 is not fitted on R3, so
`keel_servo_start` and `keel_servo_set_pulse` return `KEEL_E_UNSUPPORTED` there, and `keel_servo_end` is a no-op. The R3
backend claims BCM19 (SERVO_REQUEST) and holds it LOW at all times, and never touches BCM12. The simulator implements
the full contract, so bindings and applications can be tested against it.

| Item | Rule |
|---|---|
| Limits | pulse 500-2500 us; lease 20-400 ms (400 ms plus margin stays under the MCU's 500 ms on a 6%-fast clock) |
| Preconditions | ARMED; owner thread; both motors coasting (not driven, not braked, no pending reversal) for at least 20 ms, else `KEEL_E_AGAIN` (coasting < 20 ms) or `KEEL_E_CONFLICT` (driven, braked, reversal pending, burst active) |
| Cooldown | 2200 ms from open, from the last READY-LOW sample, and from the completion of the core's servo `OFF` call that ended the last burst. That completion is never earlier than the real request-LOW edge, so the host cooldown always ends after the MCU's 2000 ms (x1.06). The `OFF` calls of stops without a burst do not restart it, because the MCU restarts its own cooldown only when a burst ends |
| Start | the stored motor command and timeout are discarded; the core reads the inputs fresh, then calls `servo(REQUEST, valid_until = now + lease)`; the state becomes SERVO |
| Grant | ticks require READY = 1. Within 10 ms of the request ARMED must read 0, else `SERVO_NOT_GRANTED`. Once ARMED = 0 is seen, the core calls `servo(PULSE, pulse)` and sets `KEEL_SNAP_SERVO_GRANTED`. The pulse never starts before the grant, so no signal is driven into an unpowered servo |
| During | `keel_servo_set_pulse` (owner) changes the width, never the lease. ARMED HIGH after the grant fails with `DUAL_PERMIT` |
| End | lease end (tick at or after `valid_until`, or the backend itself at `valid_until`), `keel_servo_end`, disarm, fault, close: `servo(OFF)` (pulse off, then request LOW), then ENABLE LOW; state DISARMED; cooldown from the OFF completion; driving again needs a new arm |
| Simulator PWM | disabled at open and at close; the backend's own lease timer ends a burst without ticks |
| Addressable LEDs on J5 | not supported: J5 power is lease-gated by the MCU |

## 15. Ranging (J1, HC-SR04; TRIG BCM13, ECHO BCM25)

- `keel_range_start` (non-blocking), `keel_range_poll` (non-blocking), and `keel_range_measure`. The blocking one never
  waits for the trigger spacing (it returns `KEEL_E_AGAIN` instead), waits at most 35 ms in total, holds no CONTROL, and
  wakes when close starts.
- Order inside `keel_range_start`:
  1. capability;
  2. sensor power. If `sensor_power` is not PERMITTED or CONFIRMED (heartbeat stopped or not qualified, READY LOW),
     return OK with an immediate UNAVAILABLE result (new sequence number; a pending measurement is abandoned) and do
     not drive TRIG. The cached READY can be a whole tick old, and an MCU that reset or latched since then has
     already switched the sensor off, so a cached PERMITTED or CONFIRMED is confirmed by a fresh READY read under
     CONTROL first. That read is control I/O: the backend latch has priority (section 12.3), and a read failure
     latches as for `keel_read_safety_inputs` (FAULT-class statuses);
  3. a pending measurement, less than 60 ms since the last trigger, or ECHO HIGH: `KEEL_E_AGAIN`;
  4. TRIG pulse of at least 10 us.
- **Statuses, never sentinel distances.** `IDLE` = 0, so a zeroed result is never VALID.

  | Status | Meaning |
  |---|---|
  | `IDLE` | no measurement started |
  | `PENDING` | measurement in progress |
  | `VALID` | 20-4000 mm |
  | `NO_ECHO` | the echo rose but the pulse exceeded 23.3 ms, or was still HIGH at 30 ms; means "nothing detected", never "clear" |
  | `UNAVAILABLE` | the sensor is unpowered, or no echo rose within 5 ms (locked out or unplugged) |
  | `INVALID` | ECHO HIGH before the trigger, a pulse under 117 us, lost edge events, or READY fell during the measurement |
- Distance = `echo_us * 0.1715` mm (343 m/s; no temperature compensation).
- keel_r3 requests TRIG as an output starting LOW, and ECHO as an input with rising and falling edge events stamped on
  the kernel's CLOCK_MONOTONIC.
- Ranging works in every state, because sensor power needs a heartbeat, not arming. Errors of the sensor lines are
  `KEEL_E_PERIPHERAL` and never latch; only the fresh READY read of step 2 (control I/O) and a failed sleep inside
  `keel_range_measure` (cause `CLOCK`) can return a FAULT-class status. `KEEL_OPEN_NO_RANGE` leaves BCM13/25
  unclaimed.

## 16. LEDs and buttons

- **RGB.** IS31FL3218-compatible protocol:
  - initialisation: shutdown (0x00 = 0), reset (0x17), clear the 18 PWM bytes (0x01..0x12), enable all channels
    (0x13..0x15 = 0x3F), latch (0x16), wake (0x00 = 1);
  - a frame is one PWM block write plus a latch;
  - close writes shutdown;
  - LED i uses bytes 3i (R), 3i+1 (G), 3i+2 (B).

  Calls: `keel_set_rgb(index, 0x00RRGGBB)` (bits 24-31 ignored), `keel_set_rgb_all`, `keel_set_rgb_frame(rgb[], count)`
  (one frame, one latch, no tearing) and `keel_get_rgb` (cache). Values are linear.
- **Failures** are `KEEL_E_PERIPHERAL`, non-latching, and leave the cache unchanged. LEDs work in every state, including
  FAULTED. A missing I2C bus or LED driver at open clears `KEEL_CAP_RGB` and sets `KEEL_OPEN_WARN_NO_RGB`; the open
  succeeds, and RGB calls then return `KEEL_E_UNSUPPORTED`.
- **I2C transport.** `I2C_RDWR` with the address in every message, `I2C_TIMEOUT` = 2 (20 ms), no `flock`, and the bus
  speed left alone (400 kHz is the opt-in `keel-setup --i2c-fast`). LED calls use the LEDS domain and never delay ticks
  or stops.
- **Button LEDs.** `keel_set_button_leds(mask, values)` changes only the masked bits. The lines share the control
  request with GPIO26 and the heartbeat, so a failure is `KEEL_E_IO` and latches. The state is cached in the snapshot.
- **Buttons.** 20 ms debounce. `keel_take_buttons` does a fresh read, debounces, returns held, pressed and
  released, and consumes the edges. `keel_get_snapshot` peeks; `keel_cycle` consumes with `KEEL_CYCLE_TAKE_BUTTONS`. A
  button held at open becomes a press after 20 ms. Buttons work in FAULTED.

## 17. The keel_r3 backend

### 17.1 Discovery and preflight

- **Chip.** The first `/dev/gpiochipN` whose label is `pinctrl-bcm2711` with at least 28 lines (BCM2711 only: Pi 4,
  Pi 400, CM4), unless `gpiochip_path` names a chip. A wrong label is `KEEL_E_NOT_FOUND`.
- **Preflight.** `GPIO_V2_GET_LINEINFO` on every line to be requested. A line flagged USED is `KEEL_E_IN_USE`; the report
  gives `line` and `consumer` (for example `spi0`) and a hint (`dtparam=spi=off`).
- **errno mapping at open:**

  | errno | Status |
  |---|---|
  | EACCES, EPERM | `KEEL_E_PERMISSION` |
  | ENOENT, ENODEV, ENXIO | `KEEL_E_NOT_FOUND` |
  | EBUSY | `KEEL_E_IN_USE` |
  | ENOTTY or EINVAL from `GPIO_V2_GET_LINE_IOCTL` (kernel without GPIO v2) | `KEEL_E_NOT_FOUND` |
  | ENOMEM | `KEEL_E_NO_MEMORY` |
  | anything else | `KEEL_E_SETUP_IO` |

  The report always carries `os_error`, `path` and a one-line `hint`.
- **I2C.** `i2c_path` or `/dev/i2c-1`. Any failure to open the bus or initialise the driver is a warning (section 16).

### 17.2 Line requests

Every request is exclusive, with consumer `keel` and `O_CLOEXEC`; outputs are requested at value 0.

| Request | Lines (BCM) | Config |
|---|---|---|
| control | 26, 18, 19, 23, 22, 17, 27 | outputs LOW |
| motor | 8, 11, 10, 9 | outputs LOW |
| input | 20, 21, 5, 6, 16, 24 | inputs; 20/21 bias disabled; 5/6/16/24 pull-up and active-low (logical "pressed") |
| range (unless `NO_RANGE`) | 13 out LOW; 25 in with both edge events | separate request |

BCM0, 1, 7, 14, 15, 4 and 12 are never requested; a backend test asserts it.

### 17.3 Hooks

- `write_outputs` sets ENABLE, heartbeat and button LEDs with one `SET_VALUES` on the control request.
- `read_inputs` uses one `GET_VALUES`.
- `emergency_disable` writes ENABLE LOW on the control request and LOW on all motor lines, under the PWM lock. It
  records the epoch and stores COAST.
- `thread_id` is `gettid()`; `now_us` is CLOCK_MONOTONIC.
- `sleep_until_us` waits on a per-device condition variable with CLOCK_MONOTONIC, so `cancel_sleep` can wake it.

### 17.4 PWM worker

BCM8-11 have no hardware PWM, so the 100 Hz userspace worker stays.

- **Scheduling.** Created with `PTHREAD_EXPLICIT_SCHED` at SCHED_FIFO priority 40 (fixed; there is no priority option).
  On EPERM the thread is recreated as SCHED_OTHER. The fallback is reported in `report.timing_class`,
  `report.realtime_os_error`, `KEEL_OPEN_WARN_BEST_EFFORT_TIMING` and `snapshot.timing_class`, and
  `KEEL_CAP_REALTIME_PWM` is clear. `KEEL_OPEN_NO_REALTIME` skips the attempt. The library never calls
  `sched_setscheduler` or `mlockall` on the process.
- **Thread setup.** Async signals are blocked around `pthread_create`. The thread is named `keel-pwm` and has a 64 KiB
  stack, pre-faulted, with best-effort `mlock` of that stack and the backend allocation. The mutex is
  `PTHREAD_PRIO_INHERIT`.
- **Timing.** Absolute CLOCK_MONOTONIC waits (`pthread_cond_timedwait`, clock set with `condattr_setclock`), with the
  phase anchored to the motion-start epoch. The worker is signalled only when the pattern changes or motion starts from
  idle, not on every lease renewal. No edge follows a synchronous coast.
- **Late wakes are never fatal individually.** Each wake computes `lateness = now - expected`, recomputes the output
  from the current phase, and updates the statistics: wakes, wakes over 1 ms, wakes over 5 ms, maximum and last
  lateness. The worker latches only on:
  - `PWM_OVERRUN`: one wake more than 30 ms late;
  - `PWM_DEGRADED`: the sum of max(0, lateness - 1 ms) over a sliding 100 ms window exceeds 20 ms;
  - `PWM_WRITE`: a motor-line ioctl failed (status `KEEL_E_PWM`, errno kept).
- **Stall.** `PWM_STALL` is detected by the control thread inside `write_motors`: a stored, unexpired DRIVE or BRAKE
  while the worker's scheduled wake is more than 25 ms in the past. The scheduled wake is used rather than the last
  pass because a brake has no periodic wakes. The control thread then drives the lines LOW itself and latches.
- **Self-stop.** On a latch the worker writes ENABLE LOW and SERVO_REQUEST LOW (control request) and the motor lines LOW,
  stores COAST and records the fault. It never touches the heartbeat.
- **Lease expiry** is a silent coast counted in `lease_expiries`. The backend enforces leases and the core diagnoses
  missing ticks.
- **Policy.** The lateness and pattern decisions are a pure function in `src/backend/pwm_policy.c`, unit-tested with
  injected times: lateness of 1.9, 2.5 and 9.9 ms does not latch and gives the right output; 31 ms latches OVERRUN; a
  budget overrun latches DEGRADED.
- **Acceptance (qualification, on the robot).** `keel-live --pwm-jitter 600`, idle and under `stress-ng --cpu 4`,
  targets zero latches at SCHED_FIFO and p99.9 lateness under 1 ms. Measured on the robot (2026-11-07, 600 s under
  load): p99.9 lateness 216 us and no overruns.

### 17.5 Interlocks (defence in depth; the core never triggers them)

Under the PWM lock, with shadows of ENABLE, SERVO_REQUEST and the motor command:
- a rising ENABLE is refused unless the motor command is COAST and SERVO_REQUEST is LOW;
- DRIVE or BRAKE is refused unless ENABLE is HIGH and SERVO_REQUEST is LOW.

A refusal coasts, latches `INTERLOCK` and returns `KEEL_E_INTERNAL` (sticky).

DRIVE or BRAKE also requires a fresh ARMED = 1 read (one `GET_VALUES`) under the lock. ARMED = 0 coasts and latches
`KEEL_E_FEEDBACK` / `ARMED_LOST`: it can be a genuine MCU drop racing the core's read, so it is not called a defect.
This catches a DRIVE during the acknowledgement's HIGH levels, which the ENABLE shadow alone would allow.

Order inside `write_motors`:
1. a COAST command is always applied and returns only the status of its own write;
2. latched: return the latched status;
3. old epoch: suppress (COAST, OK);
4. `valid_until` at or before now: coast, OK, `lease_expiries`;
5. interlocks;
6. fresh ARMED read;
7. stall check;
8. store and signal.

## 18. Simulator

### 18.1 Model

- MOTOR_EN_SAFE = MCU motor permit AND GPIO26 AND the fault line HIGH, which drives ARMED. READY comes from the MCU.
- DRV8411 levels follow the motor command through the shared `pwm_policy.c`: drive, coast, brake.
- An IS31FL3218 register file with latch.
- An HC-SR04 powered by the MCU sensor permit, with a 0.5 ms echo rise and width = 2 x distance / 343 m/s.
- A servo pulse gated by the servo permit.
- Servo: the full burst contract of section 14.
- Capabilities: MOTORS, BRAKE, RGB, BUTTONS, BUTTON_LEDS, SERVO, RANGE and SIMULATED (minus `NO_SERVO`/`NO_RANGE`).
  Timing class NONE.
- Backend interlocks and the stop epoch behave as in keel_r3; `emergency_disable` and the backend's own self-stop also
  stop the servo pulse and lower the request. In addition, a servo REQUEST is refused unless ENABLE is
  HIGH and the motors have coasted for at least 3 ms (the MCU samples every 1 ms).

### 18.2 MCU policy

The MCU is the production policy, copied into `src/backend/sim/mcu/` from the firmware tree
(`firmware/safety-mcu/src/`).

- **Header.** Each file gets a short header comment: SPDX MIT, copyright BisocM, provenance, and the SHA-256 of the
  original file. The bytes below the header are the original file, unmodified.
- **Hash guard.** A CTest step hashes the content below the header and requires:
  - `safety_core.c`: `da9cc0d8b9e2a4735e0751860fd2501c2b898fecef27278977b7cbd9efba48b2`
  - `safety_core.h`: `3a7c36f0c6e449968a9308381dd0bc14f57e670a1501613b834a69dd4e5b3985`

  A policy change is therefore a deliberate, reviewed event. `.gitattributes` marks both files `-text`, so line endings
  never change the bytes.
- **Renaming.** A wrapper translation unit `mcu_policy.c` defines `safety_init`, `safety_step`, `sensor_guard_valid`,
  `sensor_guard_boot`, `sensor_guard_trip`, `sensor_guard_tick` and `sensor_guard_lockout` to `keel__mcu_*` names, then
  `#include`s the unmodified `safety_core.c`. Its private header does the same before including `safety_core.h`.
- **Stepping.** The policy is stepped every simulated millisecond with the input mapping of the reference-model tests.

The simulator's ground-truth `mcu_stage` and `mcu_faults` fields are opaque diagnostics outside ABI stability; they
are never asserted by binding conformance scenarios.

### 18.3 Clocks

- **REALTIME** (default) follows CLOCK_MONOTONIC. Every hook first advances the model to now in 1 ms steps.
- **VIRTUAL** (`KEEL_OPEN_VIRTUAL_CLOCK`) moves time only inside `keel_sleep_until`. The model advances under the
  simulator's own lock, never CONTROL. Virtual-clock devices are driven from one thread, and the same loop code runs in
  both modes.

### 18.4 Selection and stimulus

- Select the simulator with `KEEL_BACKEND_SIMULATOR`, or with `KEEL_BACKEND=simulator` plus AUTO. AUTO never falls back
  to it silently, and `KEEL_CAP_SIMULATED` and `KEEL_SNAP_SIMULATED` identify it. Several simulator devices may be open
  in one process.
- `keel_sim_set` covers:
  - buttons and distance;
  - a hardware-fault pulse and MCU reset;
  - injected GPIO read and write failures, I2C failures and PWM write failures;
  - sensor lockout, PWM lateness and a PWM stall;
  - ARMED stuck HIGH.

  `keel_sim_get_state` returns pin levels, permits, bridge drive, servo pulse, the clock and the falls seen.

## 19. Porting interface (`port.h`, C only)

- **Not exported from the .so.** The port API is in `libkeel.a` and source builds only, versioned by
  `KEEL_PORT_VERSION` (1) and `struct_size`, independently of the application ABI.
- **`keel_port_attach`**`(storage, size, backend, options, &dev, report)` attaches the core to caller storage
  (`keel_port_storage_size/align`). It validates the table, resets the backend latch, drives safe levels, initialises the
  LEDs when RGB is declared, and returns a normal handle. `keel_close` calls `destroy` and never frees the storage. The
  status rules of `keel_open` apply.
- **Required hooks:** `now_us`, `sleep_until_us`, `read_inputs` (with `KEEL_IN_BACKEND_FAULT`), `write_outputs`
  (with stop epoch), `write_motors` (`keel_port_motor_command`: COAST/DRIVE/BRAKE, duties, stop epoch, `valid_until_us`),
  `emergency_disable(epoch)`, `get_fault`, `reset_fault`.
- **Optional hooks:**
  - `destroy`;
  - `lock`/`unlock` (CONTROL, LEDS, RANGE; NULL requires `KEEL_PORT_SINGLE_THREADED`);
  - `thread_id` (NULL: no owner checks);
  - `cancel_sleep`;
  - `get_stats` (`keel_port_stats`);
  - `i2c_write` (RGB);
  - `servo` (`keel_port_servo_command`: OFF/REQUEST/PULSE, with epoch and lease);
  - `range_trigger` (reports ECHO HIGH instead of triggering);
  - `range_edges`;
  - `sim_set`/`sim_get_state`.

  A capability declared without its hooks is refused at attach.
- **Status rules, backend latch, stop epoch and interlocks** are stated in the `port.h` overview and in sections 12.2,
  12.3, 8 and 17.5.
- `size_t` is allowed in `port.h` because it is C only.

## 20. Snapshot and diagnostics

`keel_snapshot` (144 bytes) is filled by `keel_get_snapshot` and `keel_cycle` under CONTROL, with no I/O.

| Group | Fields |
|---|---|
| State | `state`, `flags` (`KEEL_SNAP_*`), `inputs` and `inputs_sampled_us`, `time_us`, `last_cycle_id`, `owner_thread_id`, `tick_budget_us` (time left in the 70 ms window; negative when late), `ack_phase` |
| Motion | `command_left/right` (stored command, 0 after coast, stop or expiry), `output_left/right` (applied duty; 0 when coasting, braking, expired or dwelling), `command_remaining_ms` |
| Buttons and LEDs | held, pressed, released (peeked), `button_leds` |
| Sensor and servo | `sensor_power`, `servo_pulse_us`, `servo_lease_remaining_ms`, `servo_cooldown_remaining_ms` |
| Faults and counters | `fault_cause`, `fault_count`, `last_rejection`, `heartbeat_restarts`, `stale_ack_restarts` |
| Timing | `timing_class`, `pwm_late_wakes`, `pwm_max_lateness_us`, `pwm_lease_expiries` |

`keel_fault` (64 bytes) is the record of section 12: status, cause, state, ack phase, inputs, `os_error`,
`cleanup_status`, detail, time, `fault_count`, flags (CLEARED, STICKY) and `cleanup_os_error`.

`keel_open_report` (456 bytes) gives:
- status, resource, `os_error`, line, backend, capabilities;
- warnings (`NO_RGB`, `BEST_EFFORT_TIMING`), timing class and `realtime_os_error`;
- path, consumer and hint.

On success, resource, path and hint describe the first degradation, if any.

## 21. Bindings

Every binding follows the rules below; `docs/bindings.md` gives them in detail. The production Python and Java
bindings, which follow them, are in `software/bindings/`.

- **Loading.**
  1. `KEEL_LIBRARY` (absolute path) if set.
  2. Otherwise the SONAME `libkeel.so.1`, the only choice on the robot. Robot packages never bundle a library, so there
     is exactly one copy of the safety code.
  3. On a desktop, a simulator build named by `KEEL_LIBRARY`; a binding package may bundle one in a later release.

  At load, require `keel_abi_version() >> 16 == 1` and a sufficient minor.
- **One header.** Bindings use only `keel.h` (preprocessed with `-DKEEL_API=`); constants are generated from it.
  A binding of the camera library uses `camera.h` the same way (section 26.2).
- **Control loop.** A dedicated platform thread (never a virtual thread, pool or async task) runs:

  ```
  next = now_us
  loop:
      next += 10 ms
      step()
      rc = keel_cycle({++id, health})
      dispatch rc by class
      keel_sleep_until(next)
  ```

  Health is `KEEL_HEALTH_ALL` only if `step()` returned normally within its budget. No keepalive or background ticker
  is offered, and the device clock is used, never language sleeps.
- **Duties handed to a loop helper** are checked against `KEEL_DUTY_MAX` by the helper: a violation fails that step
  (health without `KEEL_HEALTH_NO_EXCEPTION`, so the tick fail-stops with `HEALTH_FLAGS`), and a duty posted from
  another thread is refused there and replaced by a coast. `KEEL_E_INVALID_ARGUMENT` from `keel_cycle` therefore keeps
  meaning a defect in the input the binding built. Pass-through calls (`keel_set_motors`, a binding's own
  `keel_cycle` wrapper) stay unvalidated, so libkeel's refusal and its coast while ARMED apply.
- **Loop outcomes.** FAULT stops the application's motion logic. REJECTED is logged and counted, except
  `KEEL_E_INVALID_ARGUMENT` and `KEEL_E_UNSUPPORTED` from `keel_cycle`, which are fatal. HANDLE stops the loop.
- **Stop buttons and Ctrl-C** call `keel_emergency_stop`. Bindings never auto-clear or auto-arm. Close runs from
  try-with-resources, `using` or `with`; finalizers and cleaners are a last resort, and close never throws.
- **Signal safety.** No libkeel function is async-signal-safe: a signal handler only sets a `volatile sig_atomic_t` flag (or
  writes to a pipe), and a normal thread (the loop, or one that waits in `sigwait()` or reads a `signalfd`) calls
  `keel_emergency_stop`. Called from the handler itself it can deadlock on a lock the interrupted thread holds.
  Java and .NET deliver signals on ordinary threads already; Python runs its handlers on the main thread between
  bytecodes, which is safe.
- **C#.** The four static-string functions are declared as returning a pointer (`nint`/`byte*`) and converted with
  `Marshal.PtrToStringUTF8`. A `string` return would free static memory. A test calls each 10^5 times. Structs are
  blittable, the handle is a `SafeHandle`, and `GCSettings.LatencyMode` is SustainedLowLatency.
- **Java** (FFM, Java 25). Applications grant native access with `--enable-native-access=ALL-UNNAMED` (class path)
  or `--enable-native-access=<module>`: the attribute in a library jar's manifest is ignored on the class path.
  Strings are read with `reinterpret(256)`, the `KEEL_STRING_MAX` bound stated in the header. Structs live in confined
  arenas, per call and, for the loop, in one arena its own thread opens and closes. Use ZGC or
  `-XX:MaxGCPauseMillis=20`.
- **Python** (ctypes). The loop thread sets `sys.setswitchinterval(0.001)` while running, which keeps the GIL
  hand-off after each foreign call short behind one busy Python thread. Behind several CPU-bound Python threads a
  hand-off can take many switch intervals and exceed the 40 ms stall budget, so CPU-bound work runs in another process
  or on free-threaded CPython. Call `gc.freeze()` after start-up and allocate nothing heavy in `step()`.
- **Conformance.** Language-neutral scenarios (`tests/bindings/scenarios.json`) are replayed by every binding on
  the virtual-clock simulator, through its public API. The test suite replays the same file through the reference
  ctypes binding (CTest `ctypes.scenarios`), so every expectation is proven against the library itself. Scenarios
  open the simulator only, and never assert `mcu_stage` or `mcu_faults`.

## 22. Build, versioning and packaging

- **Version single source.** `KEEL_VERSION_MAJOR/MINOR/PATCH` in `keel.h`; CMake parses them (`file(STRINGS)`).
  SemVer: MAJOR = incompatible ABI or semantics (new SONAME, new runtime package name), MINOR = additive, PATCH = fixes.
  SOVERSION = MAJOR. `keel_abi_version()` = `0x00010000`. `keel_version_string()` =
  `"1.0.0 (<triple>; backends r3,simulator)"`. GNU ld version script `KEEL_1.0 { global: <the 37 names>; local: *; };`.
- **CMake.**
  - Requires CMake 3.21 or later; options are gated on `PROJECT_IS_TOP_LEVEL`.
  - Targets: shared `keel` (`Keel::keel`, SOVERSION 1) and static `keel_static` (`OUTPUT_NAME keel`, INTERFACE
    `KEEL_STATIC`, `Keel::keel_static`). The port API is compiled into the static archive only.
  - Options: `KEEL_BACKEND_KEEL_R3` (ON on Linux), `KEEL_BACKEND_SIM` (ON), `KEEL_WERROR` (OFF; ON in presets and CI),
    `KEEL_SANITIZE` (a `-fsanitize=` list) and `KEEL_TSAN` (set by presets only), `KEEL_TOOLS`, `KEEL_PACKAGING`,
    `KEEL_BUILD_TESTS` (top-level default) and `KEEL_OS` (`posix`, or `none` for single-threaded ports). The
    backend, tool and packaging modules are `cmake/backend_keel_r3.cmake`, `cmake/tools.cmake` and
    `cmake/packaging.cmake`, included when present. The camera library has its own module, `cmake/camera.cmake`,
    and options (section 26.11).
  - Warnings live in a non-exported INTERFACE target (`keel_build_options`, `cmake/compiler_options.cmake`):
    `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wundef`, plus
    `-Werror` in presets and CI.
  - Release hardening: `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, `-Wl,-z,relro,-z,now`, and
    `-mbranch-protection=standard` on aarch64.
  - Presets: `dev`, `asan-ubsan`, `tsan`, `release`, `cross-arm64`, `sim-only`. Sanitizer flags live only in presets
    and never become usage requirements.
  - Toolchain file `cmake/toolchains/aarch64-linux-gnu.cmake`.
  - Install rules provide the CMake package `Keel` (SameMajorVersion), `keel.pc` (the shared library) and
    `keel-static.pc` (`${libdir}/libkeel.a -pthread` and `-DKEEL_STATIC`: `-lkeel` would pick the `.so` from the same
    directory, and only the archive has the port API). Both find their prefix through `${pcfiledir}`, so they work
    from any install location.
- **Debian packages** are built with debhelper (`debian/`, `dh $@ --buildsystem=cmake`, `dpkg-buildpackage -a arm64`),
  not CPack:

  | Package | Contents |
  |---|---|
  | `libkeel1` (arm64) | `libkeel.so.1*`, shlibs, `Depends: keel-common (= ${source:Version})` |
  | `libkeel-dev` | `keel.h`, `port.h`, the `.so` link, `libkeel.a`, the CMake package (`KeelConfig.cmake`, `KeelTargets.cmake`), `keel.pc`, `keel-static.pc` |
  | `keel-common` (arch all) | `/usr/lib/modules-load.d/keel.conf` (`i2c-dev`), `/usr/lib/udev/rules.d/99-keel.rules`, `/usr/lib/sysusers.d/keel.conf` (`g keel -`), `/etc/security/limits.d/90-keel-rtprio.conf` (`@keel - rtprio 40`) |
  | `keel-tools` | `keel-live`, `keel-demo`, `keel-doctor`, `keel-setup` |
  | `libkeel-camera1` (arm64) | `libkeel-camera.so.1*`, symbols, `Depends: libcamera-ipa` and the libcamera runtime package by name (section 26.11) |
  | `libkeel-camera-dev` | `camera.h`, the `.so` link, `KeelCameraTargets.cmake`, `keel-camera.pc` |
  | `keel-camera` | `keel-camera` |

  The board packages depend on nothing of the camera, so they install where the camera library's libcamera is
  not available.

  The `keel-common` postinst runs `systemd-sysusers` (inserted by `dh_installsysusers`; the package depends on
  `systemd | systemd-standalone-sysusers | systemd-sysusers`), `modprobe i2c-dev || true` and
  `udevadm control --reload; udevadm trigger --subsystem-match=gpio --subsystem-match=i2c-dev`. It never changes the
  I2C speed, never edits boot config and never adds users.
- **udev rule** (`99-keel.rules`, no `GROUP:=`). Where the distribution has `gpio` and `i2c` groups (Raspberry Pi OS),
  they keep their meaning; elsewhere the nodes get group `keel`. Both gpio rules also create `/dev/keel/gpiochip`,
  which keel_r3 and `keel-doctor` try after scanning `/dev/gpiochip*`:

  ```
  SUBSYSTEM=="gpio", KERNEL=="gpiochip*", KERNELS=="fe200000.gpio", SYMLINK+="keel/gpiochip", PROGRAM="/usr/bin/getent group gpio", GROUP="gpio", MODE="0660", GOTO="keel_gpio_end"
  SUBSYSTEM=="gpio", KERNEL=="gpiochip*", KERNELS=="fe200000.gpio", SYMLINK+="keel/gpiochip", GROUP="keel", MODE="0660"
  LABEL="keel_gpio_end"
  SUBSYSTEM=="i2c-dev", KERNEL=="i2c-1", PROGRAM="/usr/bin/getent group i2c", GROUP="i2c", MODE="0660", GOTO="keel_i2c_end"
  SUBSYSTEM=="i2c-dev", KERNEL=="i2c-1", GROUP="keel", MODE="0660"
  LABEL="keel_i2c_end"
  ```

- **`keel-setup`.** It adds the user to `keel` (for rtprio) and to `gpio`, `i2c` and `video` (the camera's device
  nodes) where those groups exist, and edits `/boot/firmware/config.txt` idempotently inside a marked block, with a
  backup:
  - `dtparam=spi=off`;
  - `force_eeprom_read=0` (the firmware must not read a HAT EEPROM and apply its overlay);
  - `dtparam=i2c_arm=on`;
  - `dtparam=i2c_arm_baudrate=400000` only with `--i2c-fast`.

- **`keel-doctor`** is read-only. It checks:
  - the chip label and that the 19 lines are free (naming consumers);
  - node owner and mode, and the user's groups;
  - that `i2c-dev` is loaded and `/dev/i2c-1` exists;
  - a kernel of at least 5.10 and SPI0 off;
  - RLIMIT_RTPRIO and a SCHED_FIFO probe thread;
  - the cpufreq governor and the throttling flags;
  - that the library really reaches keel_r3: an open of `/dev/null` as the GPIO chip must fail inside the backend
    (resource `GPIO_CHIP`), not before it (resource `BACKEND`).

  It prints JSON and text, with a remedy per failed check. `--user NAME` (used by `install.sh`, which runs as root)
  judges node access, group membership and the real-time limit for that account from the user and group databases,
  as they apply after its next login, and skips the SCHED_FIFO probe, which can only describe the calling process.
- **install.sh** is published under the `releases/latest/download/` URL, with `--user NAME`, `--dev` (adds
  `libkeel-dev` and `libkeel-camera-dev`), `--no-camera`, `--camera` and `--version X.Y.Z`. It verifies SHA256SUMS,
  installs the board packages `libkeel1`, `keel-common` and `keel-tools` and the camera packages `libkeel-camera1`
  and `keel-camera` (plus the dev packages), runs `keel-setup` and prints `keel-doctor --user NAME`. It leaves the
  camera packages out with `--no-camera`, and also, with a message, when this system neither has nor offers a
  dependency of `libkeel-camera1` (read with `dpkg-deb`, looked up with `dpkg-query` and `apt-cache`); `--camera`
  skips that check. A release without the camera packages is refused unless `--no-camera` is given. Its body runs
  from a `main` function called on the last line, so a truncated download runs nothing, and no step reads standard
  input (under `curl | sh` that is the script itself); dpkg keeps an admin-edited conffile (`--force-confdef
  --force-confold`) instead of asking. A manual two-step path is documented.
- **CI** is `.github/workflows/libkeel.yml`, path-filtered to `software/libkeel/**`, with pinned actions and
  `contents: read`. Its jobs are those of section 23.2.

## 23. Tests and the 1.0 gate

### 23.1 Layers (CTest labels; every case registered individually, timeout 60 s)

| Layer | Content |
|---|---|
| abi | `tests/abi/check_headers.sh`: C99/C11/C++11/C++17 consumers, `layout.c` on x86-64, i386, aarch64 LP64 and ILP32, `check_constants.py` (preprocessed constants vs compiler vs golden; functions vs `symbols.txt`), `tests/docs/check_header_docs.py`; `check_exports.sh` (exported symbols vs `symbols.txt` with `nm -D`, static-archive symbol hygiene); the struct-size handshake for every output struct (`sim_api.struct_handshake`: oversize zero tail accepted, non-zero tail UNSUPPORTED with no effect, output zero-fill, outputs untouched on INVALID_ARGUMENT and the HANDLE class, `struct_size` above `KEEL_STRUCT_SIZE_MAX` refused for every input and output, the open report written on INVALID_ARGUMENT); `sim_api.r3_dispatch` (an R3 open reaches the backend); the Python ctypes smoke against the simulator and the replay of `tests/bindings/scenarios.json` (`ctypes.*`) |
| unit | debounce, heartbeat scheduler, deadman, reversal, servo cooldown, range classification, LED protocol, status/class/cause tables, registry tokens, `pwm_policy` (1.9/2.5/9.9 ms no latch, 31 ms OVERRUN, DEGRADED budget) |
| reference model | the core against the hash-guarded MCU policy every simulated ms: the 1-70 ms schedule sweep and the 71-100 ms rejection; servo sweeps asserting no F_COOLDOWN, F_PWM or F_SERVO_TIMEOUT and never both permits; brake while ARMED never latches |
| arbitration | replays every reachable cell of `arbitration_table.h` from both thread columns (status, cause, state after, outputs, MCU stage, heartbeat continuity after every REJECTED); `check_arbitration_doc.sh` keeps the documents identical to the table |
| error propagation | every hook x every status x every entry point: FAULT-class statuses pass through, anything else becomes `HAL_CONTRACT` (named case: injected `KEEL_E_AGAIN`); peripheral hooks give `KEEL_E_PERIPHERAL`; fail-safe outputs reached; record holds the primary cause and `cleanup_status`. Named regression: worker OVERRUN between ticks gives `PWM_OVERRUN` and `keel_take_buttons` = `KEEL_E_PWM` |
| random walk | seeded sequences of 10k calls (`KEEL_TEST_SEED` replays). Invariants every ms: no motor edge while the MCU is not armed; LP and LN HIGH together only in BRAKE while ARMED; servo request LOW whenever motors are not idle; falls only from ticks and 20-80 ms apart; FAULTED means all outputs LOW |
| threads (TSan) | hammering disarm, cycle, set_rgb, range_measure and close; disarm is never undone (stop epoch); close-vs-call gives INVALID_HANDLE; disarm latency p99.9 under 2 ms with a hung I2C stub under load; close within 100 ms with eight RGB calls on that hung bus; fork during hammering gives NOT_OWNER in the child within 100 ms; owner rules |
| backend | keel_r3 with syscall substitution: line requests, errno mapping, USED-line preflight naming the consumer, interlocks and the fresh ARMED read, I2C_RDWR and timeout, stall with a mocked clock, SCHED_FIFO EPERM fallback, never-requested offsets 0/1/4/7/12/14/15 |
| simulator | the integration suites through `keel_open(SIMULATOR, VIRTUAL_CLOCK)`, public API only; hash guard of the MCU policy |
| consumers | C via pkg-config, a port.h program via `keel-static.pc` (no `libkeel.so` dependency), C++ via `find_package(Keel)`, Python ctypes, all against the simulator |
| tools | `keel-demo` and `keel-live` against the simulator, their usage and precondition exits; `keel-doctor` against fake system trees and, for its GPIO checks, the fake kernel (`tests/tools/`) |
| docs | the arbitration table, the status and cause tables and the prose-only rules of the documents against `arbitration_table.h` and `keel.h` (`tests/docs/`) |
| packaging | the package metadata against `keel.h`, `symbols.txt` and the `keel-common` files; `keel-setup` and `install.sh` (`tests/packaging/`) |
| hygiene | SPDX and copyright lines, LF line endings and other parties' names in every file (`tests/hygiene/check_tree.sh`) |
| camera | `libkeel-camera` against its test source (section 26.9): the API rules, capture, encoders, outputs, stills, lifecycle (threads, descriptors, signals) and white-box units; its ABI files in the abi layer, its consumers in the consumers layer and `keel-camera` in `tests/camera/check_camera_tool.sh` |
| hardware (manual, the maintainer, on the robot) | `keel-live --io --leds --buttons --safety --motors --range`, `--pwm-jitter 600` under load, `--kill-test`; wheels raised; JSONL archived |

### 23.2 The 1.0 gate (CI)

- Linux gcc build with `-Werror`, and the full suite under ASan/UBSan and under TSan.
- arm64 cross build (no qemu is available, so arm64 tests run on the robot later).
- ABI job: exported symbol list, struct layouts (`check_headers.sh --require-cross --require-parser`, with pycparser
  installed so the C-parser step cannot be skipped), Python ctypes smoke against the simulator.
- Consumer tests: C, C++ and Python ctypes against the simulator.
- Documentation coverage with `check_header_docs.py`, since Doxygen is not installed; the arbitration document check.
- The camera library in the gcc, sanitizer, ABI, consumer and package jobs: built against Ubuntu 24.04's
  libcamera 0.2 and, in an Ubuntu 26.10 container, against the robot's libcamera 0.7 with libavformat; tested with
  the test source (section 26.9); packaged as `libkeel-camera1`, `libkeel-camera-dev` and `keel-camera` for amd64
  and arm64 (section 26.11). The cross-arm64 job builds libkeel only.
- Hardware qualification runs on the robot, outside CI; sections 24.2 and 26.12 record the results.

## 24. Scope cuts and qualification items

### 24.1 Not built for 1.0

- armhf, Windows and macOS artifacts.
- CodeQL, fuzzing, abidiff, attestations and an apt repository.
- A sysfs-PWM servo backend: the servo API stays and returns `KEEL_E_UNSUPPORTED` on R3.
- An expansion I2C pass-through API: I2C device drivers open `/dev/i2c-1` themselves.
- Addressable LEDs on J5.
- Callbacks or upcalls of any kind.

### 24.2 Hardware results and items not yet characterised

Measured on the robot on 2026-11-07 (an R3 board; Ubuntu 26.10; libkeel 1.0 installed from its Debian packages):

1. The MCU removal bound after process death: ARMED falls 51-71 ms after SIGKILL, inside the design bound of about
   86 ms after the last fall. ENABLE stays HIGH after the process releases the line, so the MCU heartbeat timeout
   removes motor power, as designed (`keel-live --kill-test`).
2. PWM lateness under SCHED_FIFO: a 600 s soak under load gives p99.9 lateness 216 us and no overruns, far inside the
   30 ms / 20-per-100 ms / 25 ms thresholds (`keel-live --pwm-jitter 600`).
3. Real-time privilege under the shipped limits: `keel-doctor`, run as the user, reports 13 checks passed, 1 skipped
   (the firmware's throttling flags are unavailable) and 0 failed, including its rtprio and SCHED_FIFO 40 probes.
   The shipped systemd units are not yet characterised.

Not yet characterised:

4. Echo-rise latency through the board buffer and kernel timestamp jitter (the 5 ms UNAVAILABLE window, the 117 us
   minimum); no sensor was fitted during the tests.
5. The 10 ms grace windows for ARMED_UNEXPECTED and the servo grant (edge probe).
6. The 40 ms stall budget for JVM and CPython loops (binding soak).
7. The udev rule on Raspberry Pi OS (on the robot's Ubuntu 26.10, `keel-doctor` confirms GPIO access and group
   membership), and whether the USED flag is reported for pinctrl-muxed SPI0 pins.
8. The servo path on a board with J5 fitted (simulator-proven only).

## 25. Decisions taken in this specification

Where two rules meet, or a rule leaves a choice, this specification decides as follows.

1. **Open versus the struct rule.** An options struct with a non-zero unknown tail makes `keel_open` return
   `KEEL_E_INVALID_ARGUMENT` (resource OPTIONS), not `KEEL_E_UNSUPPORTED`. The open-specific rule (OK,
   INVALID_ARGUMENT or SETUP only) is more specific than the general struct rule.
2. **Cycle versus the struct rule.** A `keel_cycle` input with a non-zero unknown tail returns `KEEL_E_UNSUPPORTED` with
   no tick. A "valid struct_size" in the always-tick rule is one the library can honour completely, and ignoring an
   unknown, possibly safety-relevant field would be worse than a missed tick, which the heartbeat timeout turns safe.
   Bindings treat it as fatal.
3. **Backend latch check scope.** "Every entry point checks it first" applies to every entry point that performs control
   I/O, at the start of its CONTROL section; stop calls check it right after lowering ENABLE. Cached reads and refusals
   decided from cached state do no I/O.
4. **Unsupported platform at open** is `KEEL_E_NOT_FOUND` (no BCM2711 chip, no GPIO v2, backend not built in, unknown
   `KEEL_BACKEND` value), keeping open's classes to OK, INVALID_ARGUMENT or SETUP.
5. **Missing I2C bus** is treated like a missing LED driver: a warning that clears `KEEL_CAP_RGB`, never an open failure.
6. **Interlock.** The fresh ARMED read under the backend lock (section 17.5) is used rather than an armed-confirmed
   shadow. It needs no extra hook and does not trust the core. ARMED = 0 there is FEEDBACK/`ARMED_LOST`, not a defect.
7. **Range status values.** `IDLE` = 0, so a zero-initialised result can never read as VALID.
8. **Refused brake without the capability while ARMED coasts**, like every motion request refused for its arguments
   (section 10, invalid motion while ARMED).
9. **`keel_emergency_stop` returns OK** when the stop succeeded. The device is FAULTED with record status
   `KEEL_E_FAULTED` and cause `USER_STOP`. In FAULTED it re-runs the stop and keeps the existing record. If the stop
   finds a backend latch (section 12.3), the backend's cause is recorded instead and its status returned: the earlier
   failure wins.
10. **Servo cooldown origin** is the completion of the core's own `servo(OFF)` call that ends a burst. That is never
    earlier than the real request-LOW edge, so no backend-reported time is needed. The `OFF` calls of stops without a
    burst do not restart the cooldown (section 14).
11. **Stall rule** uses the worker's scheduled wake, not its last pass, because a brake has no periodic wakes.
12. **R3 SERVO_REQUEST** is owned by the backend, which holds it LOW permanently; keel_r3 has no servo hook.
13. **Heartbeat restart** is keyed on the state of the previous tick. This covers the first tick after
    `keel_request_arm`.
14. **Clear fault resets the owner** (section 7) and must come from the owner (section 12.4). A dead loop thread
    therefore means close and reopen; loop helpers keep their thread alive while the device is open.
15. **Sticky set** is exactly the list in section 12.4. `DUAL_PERMIT` and `TICK_THREAD_CHANGED` are recoverable.
16. **Single-threaded ports** must say so (`KEEL_PORT_SINGLE_THREADED`); the core then enforces it when it can.
17. **Thread check placement.** It comes after argument and capability checks, so an invalid motion command from any
    thread still coasts while ARMED (a stop-direction effect). It comes before FAULTED and state checks, so a foreign
    go call is always `KEEL_E_CONFLICT`.
18. **Port attach** takes options and a report like `keel_open`, so ports get the command timeout and the LED warning.
19. **Diagnostic fields.** The snapshot carries `owner_thread_id`, `stale_ack_restarts`, `pwm_lease_expiries` and
    `fault_count`. The fault record carries `flags` and `cleanup_os_error`. The open report carries capabilities,
    warnings and the real-time outcome (section 20).
20. **`keel_open_options` has no PWM priority field.** The PWM worker runs at the fixed SCHED_FIFO priority 40
    (section 17.4), so the struct is 272 bytes with no reserved fields; appending fields later is allowed.
21. **Pass-through needs a defined code.** A safety-path hook status passes through only if it is a defined FAULT-class
    code. An undefined negative value (which `keel_status_class()` reads conservatively as FAULT for applications) is a
    port contract violation and latches `HAL_CONTRACT`. `cleanup_status` is likewise always OK or FAULT-class.
22. **A failed ENABLE-lowering is the first fault.** If `emergency_disable` fails inside `keel_emergency_stop` before the
    stop latches, that failure is recorded (the backend's cause, or `HAL_CONTRACT`), not `USER_STOP`. `keel_close`
    records a failed read or stop step in the final record the same way; its status stays diagnostic.
23. **Outputs on UNSUPPORTED.** `keel_cycle` with a non-zero unknown input tail fills a usable `out` with the snapshot
    (no tick), and `keel_sim_get_state` on a non-simulator device writes a zeroed state, as the output rule of section 4
    requires for every status outside INVALID_ARGUMENT and the HANDLE class.
24. **Range wait cap.** A measurement still pending when `keel_range_measure` reaches its 35 ms cap (or when close
    starts) is classified at once: NO_ECHO if the echo rose, otherwise UNAVAILABLE. With the trigger issued at the start
    of the call this happens only when the trigger itself was delayed.
25. **Lease versus the MCU window.** The backend lease is `min(expiry, last tick + 70 ms)` as section 10 states, while
    the MCU removes motor permission 80 ms after the last fall. When a loop stalls, the PWM lines can therefore keep
    switching for a few milliseconds after the MCU has dropped the permit; the AND gate has removed bridge enable by
    then, so the motors are already off. The random walk leaves stalls with active motors to the reference tests for
    this reason.
26. **Bounds and stop records.** `KEEL_STRUCT_SIZE_MAX` = 4096 is an enumerator rather than a number stated only
    in the text, so bindings generate it. Sensor power counts the heartbeat as stopped after 70 ms, not the MCU's
    80 ms, and is confirmed with a fresh READY read before TRIG. The fault record of a stop call names the state
    the stop ended. A sticky cause found while already FAULTED marks the kept record sticky. The arbitration table
    flags the cells whose precondition is a stalled loop (`KEEL_ARB_LOOP_STALLED`): the cached state is shown, the
    heartbeat column is "no" and the replay expects the next tick to latch `TICK_LATE`. The doctor's library check
    proves that `keel_open` reaches keel_r3 instead of reading the version string.

## 26. Camera library (libkeel-camera)

The robot's camera connects to the Raspberry Pi 4's camera connector, not to the Keel board, so it has a library of
its own. `docs/camera.md` is the user guide; this section gives the rules.

### 26.1 Scope and identity

| Item | Value |
|---|---|
| Library | `libkeel-camera` 1.0.0, ABI 1.0, SONAME `libkeel-camera.so.1`, file `libkeel-camera.so.1.0.0`; no static archive |
| Header | `include/keel/camera.h`, which includes `keel.h` for the status codes and `KEEL_API` |
| Prefixes | functions and types `keel_camera_`; enumerators and macros `KEEL_CAMERA_`; internal symbols `cam_` (hidden) |
| CMake | module `cmake/camera.cmake`, target `keel_camera` (`Keel::keel_camera`, file name `libkeel-camera`), package component `camera` (`find_package(Keel COMPONENTS camera)`) |
| pkg-config | `keel-camera` |
| Debian packages | `libkeel-camera1`; the header, `.so` link, CMake target and pkg-config file in `libkeel-camera-dev`; the tool in `keel-camera` |
| Tool | `keel-camera` (section 26.10) |
| Cameras | Camera Module 2 (IMX219, fixed focus) and Camera Module 3 (IMX708, autofocus; also Wide and NoIR); any other camera libcamera drives works for capture |
| Run-time dependencies | libcamera with its Raspberry Pi IPA module; the kernel's V4L2 devices: the camera receiver and ISP (through libcamera), the bcm2835-codec encoders (`/dev/video*`) and `/dev/dma_heap`; the camera enabled in the boot configuration (`camera_auto_detect=1`, or the sensor's overlay `imx219` or `imx708`); optionally libjpeg and FFmpeg's libavformat (section 26.11). No helper program and no package manager. |
| Not used | camera applications or any other helper process; `libkeel.so` |

`libkeel.so.1` has no camera code and no camera dependency. The camera library needs no Keel board and no device
opened with `keel_open`.

### 26.2 ABI rules

The rules of section 4 apply unchanged, with these specifics:

- Exactly the 25 functions of `tests/abi/camera_symbols.txt` are exported, under the version node
  `KEEL_CAMERA_1.0` (`cmake/keel-camera.map`). `keel_camera_abi_version()` returns `0x00010000`;
  `keel_camera_version_string()` returns `"1.0.0 (<triple>; libcamera <version>[; jpeg][; libav][; test-hooks])"`.
- Constants are enumerators, frozen in `tests/abi/camera_constants.golden`; struct layouts are pinned on the four
  targets of section 4 (`tests/abi/camera_layout.c`). Struct sizes in bytes: `keel_camera_error` 528,
  `keel_camera_info` 248, `keel_camera_mode` 40, `keel_camera_options` 72, `keel_camera_stream_info` 64,
  `keel_camera_frame_info` 64, `keel_camera_encoder_options` 32, `keel_camera_encoded_info` 32,
  `keel_camera_output_options` 24, `keel_camera_output_stats` 304, `keel_camera_stats` 72,
  `keel_camera_still_options` 24, `keel_camera_still_info` 32.
- Every struct is versioned by `struct_size` with the rule of section 4: an input above the known size is accepted
  only with a zero tail (else `KEEL_E_UNSUPPORTED`; from `keel_camera_open` `KEEL_E_INVALID_ARGUMENT`, resource
  OPTIONS, as section 25 decides for `keel_open`), and outputs are written up to the known size and zero-filled
  beyond it. A size below 1.0 or above `KEEL_STRUCT_SIZE_MAX` is `KEEL_E_INVALID_ARGUMENT`.
- Status codes keep the meaning and class of section 5. No camera call returns a FAULT-class status: the camera is
  not part of the safety function.
- Handles (`keel_camera`, `keel_camera_list`) are generation-checked tokens of a 64-slot registry, as in section 6:
  `(generation << 8) | (slot + 1)`, validated without a lock, with the owner pid compared first, so a forked child
  gets `KEEL_E_NOT_OWNER` (a `pthread_atfork` handler keeps the cached pid current).
- **Ownership.** The thread that opened a handle owns it. Every other call on it from another thread is
  `KEEL_E_CONFLICT` with no effect, except `keel_camera_close` and `keel_camera_list_close`, which any thread may
  call. A close from another thread wakes the owner's blocked reads, which return `KEEL_E_INVALID_HANDLE`, and
  waits for the calls in flight, like `keel_close`.
- **Bindings** load `libkeel-camera.so.1` by its SONAME, or the absolute path in `KEEL_CAMERA_LIBRARY`; require
  `keel_camera_abi_version() >> 16 == 1`; generate their constants from `camera.h`; read returned strings within
  `KEEL_STRING_MAX`; and never throw from close. The camera is optional for a binding: a missing library is reported
  when the camera API is first used, never when the binding is imported. The production bindings follow these rules
  as `keel.camera` (Python) and `io.github.bisocmorg.keel.camera` (Java); their tests run on the test source of
  section 26.9.

### 26.3 Capture

`src/camera/capture_libcamera.cpp` is the only file that uses libcamera. It exports the internal C interface
`cam_libcamera_source` (`camera_internal.h`); no C++ type or exception crosses it.

- One `CameraManager` per process (libcamera allows no more), shared by the listings and handles that are open and
  stopped with the last of them.
- **Listing** acquires each camera that no process holds and configures every sensor mode, to read its frame-rate
  limit and crop; a held camera is listed with `KEEL_CAMERA_INFO_IN_USE` and zero rates. Models are named from the
  sensor: `ov5647` Camera Module 1, `imx219` Camera Module 2, `imx708` Camera Module 3 and its `_wide` and `_noir`
  variants, `imx477` HQ Camera, `imx296` Global Shutter Camera; other sensors keep their name.
- **Mode choice.** Of the sensor modes at least as large as the stream that reach its frame rate, the library takes
  the one with the largest crop (the widest field of view), then the smallest, then the deepest, and passes it to
  libcamera as the `SensorConfiguration`. Without a qualifying mode libcamera chooses. libcamera's own choice favours
  the closest size, which on the IMX219 crops a 640x480 stream to the centre quarter of the sensor.
- **Stream.** Role `VideoRecording` (`StillCapture` for stills), the requested size, the pixel format whose bytes
  in memory match `keel_camera_format` (libcamera names packed formats after DRM fourccs: RGB888 is libcamera's
  `BGR888` and BGR888 its `RGB888`; I420 is `YUV420`), colour space `ColorSpace::Sycc`, the orientation from the
  flips and the rotation, and `frame_queue + 4` buffers. libcamera may adjust the size; `keel_camera_get_stream`
  reports the result.
- **Controls** are looked up by name in the camera's control list, so one source supports libcamera 0.2
  (`AeEnable`) and 0.5 or later (`ExposureTimeMode`, `AnalogueGainMode`); only controls the camera lists are set.
  `FrameDurationLimits` sets the frame rate. Focus controls exist only on cameras that list `AfMode`; a focus option
  on another camera is `KEEL_E_UNSUPPORTED` at open.
- **Logging.** libcamera's log goes to an internal buffer, never to the application's stderr, unless
  `LIBCAMERA_LOG_FILE` or `LIBCAMERA_LOG_LEVELS` is set; its last error line is appended to failure messages.
- **Frames** are libcamera's own buffers, mapped once. A frame carries a reference count (the program's queue, the
  encoder and a reader each hold one); when the last reference goes, its request is queued to the camera again.

### 26.4 Data flow, threads and encoders

```
 libcamera request completed (libcamera's thread)
   -> frame queue (frame_queue frames; full: the oldest is dropped)            -> keel_camera_read_frame
   -> encoder (at most 2 frames waiting; full: the older is dropped)
        keel-cam-encode thread: V4L2 memory-to-memory, dma-buf import or copy
        -> chunk (H.264: SPS rewritten for full range, flags scanned)
             -> encoded queue (encoded_queue chunks)                           -> keel_camera_read_encoded
             -> each output's queue (queue chunks) -> keel-cam-output thread   -> file, socket, libavformat
```

- Nothing on this path waits for the program or a destination. Every queue is bounded and counts its drops: raw
  frames drop the oldest frame; H.264 queues drop everything queued and resume at the next keyframe, so a reader
  always gets a decodable stream; MJPEG queues drop the oldest chunk. Chunks are shared by reference between the
  queues; up to 16 idle chunk buffers are kept for reuse.
- **Locks.** The session lock protects the queues, the state, the counters and the output list; an output's lock
  nests inside it, never the other way. A listening output asks for a keyframe through an atomic flag, which the
  next frame passes to the encoder, so no output thread takes the session lock.
- **Encoders** are the bcm2835-codec memory-to-memory devices, found by driver and card name
  (`bcm2835-codec-encode` for H.264, `bcm2835-codec-encode_image` for JPEG and MJPEG), never by node number.
  Frames are imported as dma-bufs when the encoder's buffer layout equals the ISP's, and copied otherwise. The image
  encoder pads its buffers to a multiple of 16 lines and encodes the frame's own lines (its crop): a frame whose
  height is not a multiple of 16 is copied with its last line repeated, and the crop is set and checked. The
  encoders take frames up to `KEEL_CAMERA_ENCODER_SIZE_MAX` (1920x1920); `keel_camera_set_encoder` refuses a larger
  stream with `KEEL_E_UNSUPPORTED`.
- **H.264**: High profile by default, level 4.0 (4.2 above 245,760 macroblocks per second), the bit rate and
  keyframe interval of the options, and repeated parameter sets, so every keyframe carries them. Chunks read by the
  program carry them at the start and, with `KEEL_CAMERA_INLINE_HEADERS`, before every keyframe; outputs always get
  them where a receiver can join. A keyframe is forced whenever an output or a listening client joins.
- **Colour signalling.** The hardware encoder writes no video signal type for YUV input, and decoders then assume
  limited range, which crushes shadows and highlights. Every SPS without one is rewritten to carry it: full range,
  BT.709 primaries, sRGB transfer, BT.601 matrix (`ColorSpace::Sycc`). An SPS that already signals one, or that
  cannot be parsed, is copied unchanged.
- **Stop** stops the camera, lets the encoder finish the frames it holds (at most 300 ms, so a recording keeps its
  last frames), discards the queued frames and chunks and keeps the outputs open; they resume at the next keyframe
  after `keel_camera_start`. Close stops, then stops every output (section 26.7) and releases the camera.
- **Threads.** Capture runs on libcamera's thread (the test source has its own, `keel-cam-test`); there is one
  `keel-cam-encode` thread per encoder and one `keel-cam-output` thread per output. They block every signal, call no
  application code and all end in `keel_camera_stop` or `keel_camera_close`. The library registers no signal or
  atexit handlers.
- **Failure while running.** A camera or encoder failure sets state FAILED; reads return `KEEL_E_PERIPHERAL` and
  `keel_camera_last_error` gives the cause. `keel_camera_stop` returns to STOPPED.

### 26.5 Frames, colour and conversion

- Formats: I420 (three planes; the chroma planes have half the width and height), RGB888 (bytes R, G, B) and BGR888
  (bytes B, G, R). The image signal processor produces them, so reading a frame costs one copy and no conversion.
- Rows may be padded: the stride and offset of each plane come from `keel_camera_get_stream` and with every frame.
  Frames are copied into the caller's buffer inside the dma-buf CPU-access synchronisation.
- Colour space: full-range BT.601 (sYCC), the JPEG colour space, for frames, H.264 (signalled, section 26.4), MJPEG
  and stills.
- `keel_camera_convert_i420` converts I420 to RGB888 or BGR888 in 16-bit fixed point, each chroma sample covering
  2x2 pixels: `R = Y + ((91881 * (V - 128) + 32768) >> 16)`,
  `G = Y + ((-22554 * (U - 128) - 46802 * (V - 128) + 32768) >> 16)`, `B = Y + ((116130 * (U - 128) + 32768) >> 16)`,
  each clamped to 0..255.
- `sequence` is the sensor's frame counter (a gap is a frame the camera dropped); `timestamp_ns` is the sensor's
  start of exposure on `CLOCK_MONOTONIC`.

### 26.6 Errors and diagnosis

Every failure records a `keel_camera_error`: status, resource (`keel_camera_resource`), errno, path and a one-line
message that names the remedy. `keel_camera_open` and `keel_camera_list_open` write it for every status once its
`struct_size` is valid; `keel_camera_last_error` returns the latest one of a handle (status OK if none).

libcamera skips cameras it cannot open without saying why, so an empty camera list is diagnosed from the device
nodes (`src/camera/diagnose.c`):

1. a `/dev/media*` node that cannot be opened: `KEEL_E_PERMISSION` (resource DEVICE_NODE), message "permission
   denied on /dev/mediaN: add the user to the video group (sudo keel-setup --user NAME), then log in again";
2. a camera receiver (`unicam`, `rp1-cfe`) with a sensor entity means a camera is connected: a missing Raspberry Pi
   IPA module (`ipa_rpi_vc4.so`, `ipa_rpi_pisp.so`) is `KEEL_E_NOT_FOUND` (resource IPA), an inaccessible
   `/dev/video*` node or CMA heap `KEEL_E_PERMISSION`, and anything else `KEEL_E_NOT_FOUND` (resource LIBCAMERA),
   naming libcamera's log;
3. otherwise no camera is connected, or the boot configuration does not enable it: an empty list from
   `keel_camera_list_open`, and `KEEL_E_NOT_FOUND` from `keel_camera_open`, naming the cable and the camera entry
   of `config.txt` (`camera_auto_detect=1`, or `dtoverlay=imx219` or `dtoverlay=imx708`).

A camera held by another process or handle is `KEEL_E_IN_USE` at open.

### 26.7 Outputs

`keel_camera_output_start` makes the library itself write the encoder's chunks to a destination, beside the
program's own reads, for up to `KEEL_CAMERA_OUTPUTS_MAX` (8) outputs per handle.

| Destination | Codec | Behaviour |
|---|---|---|
| `tcp://HOST:PORT` | either | connects within 3 s (else `KEEL_E_NOT_FOUND`), then sends the bare stream |
| `tcp://[ADDRESS]:PORT?listen` | either | binds at once (`KEEL_E_IN_USE` if taken) and serves up to `max_clients` receivers (default 4, at most 16), each with its own queue, from its next keyframe; a receiver beyond them is closed at once |
| `udp://HOST:PORT` | either | datagrams of at most `datagram_bytes` (512..65507, default 1472) |
| `PATH.h264`, `PATH.264` | H.264 | Annex B file |
| `PATH.mjpeg`, `PATH.mjpg` | MJPEG | concatenated JPEG images |
| `PATH.mp4`, `PATH.mov`, `PATH.ts` | H.264 | libavformat only |
| `PATH.mkv` | either | libavformat only |
| `rtsp://`, `rtsps://`, `rtmp://`, `rtmps://`, `srt://` | H.264 | libavformat only |

- HOST is a name, an IPv4 address or a bracketed IPv6 address. A destination that does not match the codec, an
  unknown scheme or extension, and an option meant for another kind of output are `KEEL_E_INVALID_ARGUMENT`; a
  libavformat destination in a build without it is `KEEL_E_UNSUPPORTED`.
- Every file and receiver starts at a keyframe preceded by the parameter sets; joining requests one at once.
- Each output has its own thread and queue (`queue` chunks, default 64). A destination slower than the camera loses
  chunks by the codec's drop rule (section 26.4), counted in `chunks_dropped`; it never delays the camera, the
  program or another output.
- A send that makes no progress for 2 s, a refused or reset connection, or a failed write puts the output in
  `KEEL_CAMERA_OUTPUT_FAILED` with its status, errno and message; capture and the other outputs continue. A
  listening output drops only the failed receiver.
- Every socket and file is opened close-on-exec and every send uses `MSG_NOSIGNAL`, so a receiver that goes away
  never raises `SIGPIPE`.
- `keel_camera_output_stop` and close send what is queued for at most 2 s, then close the connection; files are
  finished (a container's index written) and closed. `keel_camera_output_stop` returns `KEEL_E_PERIPHERAL` for an
  output that had failed or could not finish its file; `keel_camera_close` returns it when an output could not
  finish cleanly.
- libavformat containers use a 1/90000 time base, take their header from the first keyframe's parameter sets and
  carry the frames' capture timestamps.

### 26.8 Stills

`keel_camera_capture_still` and `keel_camera_capture_still_file` work while capture is stopped:

1. configure a still stream (role `StillCapture`, I420, 2 buffers) at the requested size, by default the full
   sensor resolution;
2. start the camera and let 8 frames pass for exposure and white balance to settle; on an autofocus camera, wait
   for focus for at most 1.5 s;
3. take the next frame and encode it: on the hardware image encoder up to `KEEL_CAMERA_ENCODER_SIZE_MAX` in both
   directions, and above it with libjpeg (raw YCbCr 4:2:0, no colour conversion), flagged
   `KEEL_CAMERA_STILL_SOFTWARE`. Without libjpeg such stills are `KEEL_E_UNSUPPORTED`;
4. stop; the next `keel_camera_start` configures the video stream again.

The options' timeout (default 5 s) limits the whole capture; `KEEL_E_AGAIN` means no frame arrived. A buffer too
small for the image is `KEEL_E_INVALID_ARGUMENT` with nothing written, and the error message gives the size needed;
(width rounded up to 16) x (height rounded up to 16) x 3 + 2048 bytes always suffice. The file variant creates or
replaces the file.

### 26.9 Test source

A build with `KEEL_CAMERA_TEST_HOOKS=ON` (the CMake presets set it; `debian/rules` sets it OFF, and `install.sh`
installs only the packages) compiles a synthetic camera (`src/camera/capture_synthetic.c`) and encoder
(`src/camera/encoder_synthetic.c`). With `KEEL_CAMERA_TEST_SOURCE=1` in the environment they replace libcamera and the
hardware encoders. They are test-only: they let this library's tests and the bindings' tests run every path above
the capture source without a camera.

- Camera 0 is an IMX219 (Camera Module 2, fixed focus) and camera 1 an IMX708 (Camera Module 3, autofocus), with
  the sensor modes libcamera 0.7 reports for them on a Raspberry Pi 4.
- Frames carry a pattern every test can recompute; padding bytes are 0xA5, and strides are rounded up to 64 bytes
  as on the ISP. With n the low byte of the sequence number and (x, y) the position before the flips: I420
  `Y = (7x + 3y + n) & 0xFF`, and `U = (5x + n) & 0xFF`, `V = (5y + 2n) & 0xFF` at chroma sample (x, y); RGB888
  `R = (x + n) & 0xFF`, `G = (y + n) & 0xFF`, `B = (x + y) & 0xFF` (BGR888 stores the same values as B, G, R).
- The synthetic encoder behaves like the hardware towards the pipeline (its own thread, at most 2 waiting frames,
  the 1920x1920 limit). Its H.264 has a real SPS and PPS for the frame size, an IDR slice at every keyframe and
  non-IDR slices otherwise, with payloads that encode the frame's sequence number; a forced keyframe starts a new
  interval. Its MJPEG chunks are structurally valid JPEG images (SOI, a comment naming the frame, DQT, SOF0 with the
  frame size, SOS, stuffed entropy-coded bytes, EOI). `KEEL_CAMERA_TEST_ENCODER=v4l2` keeps the hardware encoders
  with the synthetic camera.
- `KEEL_CAMERA_TEST_FAULT` injects one failure: `no-camera`, `permission` (`/dev/media0` not accessible),
  `ipa-missing`, `busy` (another process holds the camera), `stall` (the camera stops after 3 frames),
  `capture-fail` (it fails after 3 frames), `encoder-missing`, `encoder-fail` (it fails after 3 chunks).
- The tests (`tests/camera/`, CTest label `camera`) cover the API rules (struct sizes, handles, ownership, fork,
  every option bound), capture (patterns, layouts, flips, formats, drop policies, autofocus), every output kind with
  real sockets and files (slow and vanishing receivers included), stills, the lifecycle (repeated open and close with
  outputs leaves no thread or descriptor behind; descriptors are close-on-exec; no signal disposition changes) and
  white-box units (destinations, the SPS rewrite on a hardware SPS, the H.264 scanner, the queues, the encoder copy,
  the diagnosis on fake device trees). They also run under ASan/UBSan and TSan.

### 26.10 keel-camera

`keel-camera COMMAND [OPTIONS]`, through `camera.h` only:

| Command | Does |
|---|---|
| `list [--json]` | the cameras, their models, autofocus and sensor modes |
| `still -o FILE.jpg` | one still, by default at full resolution |
| `record -o FILE` | video to a file (`.h264`, `.mjpeg`; with libavformat also `.mp4`, `.mkv`, `.mov`, `.ts`) |
| `stream DESTINATION` | video to any destination of section 26.7, or `-` for standard output |
| `frames` | raw frames on standard output, rows unpadded, until `-n N` frames, `-t SEC` or the reader closes the pipe |

Options set the fields of the options structs (`-c`, `-W`, `-H`, `-f`, `--hflip`, `--vflip`, `--rotation`,
`--autofocus`, `--af-range`, `--lens-position`, `--exposure-us`, `--gain`, `--ev`, `--awb`, `--awb-gains`,
`--codec`, `--bitrate`, `--intra`, `--profile`, `--inline`, `-q`, `--max-clients`, `--format`); `-t SEC` limits a
run. SIGINT and SIGTERM end a run cleanly (files finished) with status 0, and so does a reader that closes the pipe
of `stream -` or `frames`. Exit status: 0 done, 1 a failure while running, 2 usage (also `KEEL_E_INVALID_ARGUMENT`),
3 `KEEL_E_NOT_FOUND`, 4 `KEEL_E_IN_USE`, 5 `KEEL_E_PERMISSION`, 6 `KEEL_E_UNSUPPORTED`. Messages are the library's
error messages.

### 26.11 Build, dependencies and packaging

- **CMake options** (`cmake/camera.cmake`): `KEEL_CAMERA` = AUTO (the default: built when pkg-config finds
  `libcamera` and a C++ compiler exists), ON (required) or OFF; `KEEL_CAMERA_JPEG` and `KEEL_CAMERA_LIBAV` = AUTO,
  ON or OFF, for libjpeg and for `libavformat`, `libavcodec` and `libavutil`; `KEEL_CAMERA_TEST_HOOKS` (OFF; ON in
  the presets). The version comes from the `KEEL_CAMERA_VERSION_*` macros of `camera.h`. The C sources use the
  warnings of section 22; the one C++ file is C++17 with the same warnings where they apply to C++.
- **libcamera versions.** The source builds against libcamera 0.2 (Ubuntu 24.04) up to 0.7 (Ubuntu 26.10, the
  robot). libcamera has no stable ABI: its SONAME changes with every minor version. A binary therefore runs only with
  the libcamera it was built against, and `libkeel-camera1` depends on that runtime package by name (through
  `${shlibs:Depends}`, for example `libcamera0.7`) and on `libcamera-ipa`, which libcamera loads at run time. The
  release's arm64 packages are built for the robot's Ubuntu release; on another distribution the packages or the
  source are built there.
- **libjpeg** encodes the stills above 1920x1920 that the hardware image encoder does not take; the packages build
  with it (`KEEL_CAMERA_JPEG=ON`).
- **libavformat** is optional and off in the packages (`KEEL_CAMERA_LIBAV=OFF`): the bare H.264 and MJPEG outputs
  need no FFmpeg. In a build with it, FFmpeg's own log goes to standard error.
- **Debian.** `libkeel-camera1` carries `libkeel-camera.so.1*` and is checked against
  `debian/libkeel-camera1.symbols`; `libkeel-camera-dev` carries `camera.h`, `libkeel-camera.so`,
  `KeelCameraTargets.cmake` (an export set of its own, which `KeelConfig.cmake` includes when present) and
  `keel-camera.pc`, and `keel-camera` carries the tool; both depend on `libkeel-camera1` of the same version, and no
  board package depends on them. `debian/rules`
  configures `-DKEEL_CAMERA=ON -DKEEL_CAMERA_JPEG=ON -DKEEL_CAMERA_LIBAV=OFF -DKEEL_CAMERA_TEST_HOOKS=OFF`.
- **Access.** The camera's nodes (`/dev/media*`, `/dev/video*`, `/dev/dma_heap/*`) belong to the `video` group on
  Ubuntu and Raspberry Pi OS. `keel-setup` adds the user to it, and the example unit `keel-app.service` sets
  `SupplementaryGroups=video`.

### 26.12 Hardware results and items not yet characterised

Measured on the robot on 2026-11-08 with a Camera Module 2 (Ubuntu 26.10, libcamera 0.7.0):

1. Listing: one camera with 8 sensor modes, their frame rates and crops, in 0.14 s.
2. Frames: I420, RGB888 and BGR888 at 640x480 and 1280x720 with the reported strides and offsets, consecutive
   sequence numbers at 30 fps, the same colours in the three formats and the full field of view at 640x480.
3. Encoders: H.264 at 640x480 and 1920x1080 decodes with FFmpeg and is signalled full range; MJPEG at 640x480 and
   1920x1080 decodes; a recording ended with SIGINT keeps its last frames.
4. Stills: 3280x2464 with libjpeg in about 1.1 s; 1920x1080, 1280x960 and 1000x750 on the hardware encoder.
5. Outputs: files, a TCP connection, a listening port with two receivers at once, UDP and standard output, each
   decoded by FFmpeg; OpenCV's `VideoCapture` on another computer read H.264 and MJPEG from a listening port. With
   libavformat: MP4, Matroska, MPEG-TS and QuickTime files and an RTSP stream.
6. Errors: a second handle and another process get `KEEL_E_IN_USE`; a user without the `video` group gets
   `KEEL_E_PERMISSION` with the remedy; focus options on Camera Module 2 get `KEEL_E_UNSUPPORTED`.
7. Twenty cycles of open, start with outputs and close leave the process's descriptors and threads as they were.

Not yet characterised:

8. Camera Module 3 and its autofocus on hardware (proven on the test source only).
9. Glass-to-glass latency, CPU load and streaming for hours.
10. Raspberry Pi OS's own libcamera build.
