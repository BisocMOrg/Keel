<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# libkeel

libkeel is the C library that drives the **Keel robot baseboard** (R3) on a **Raspberry Pi 4**: two DC
motors behind a hardware enable gate, the STM32C011 safety MCU, six RGB LEDs, four buttons with indicator LEDs
and an HC-SR04 ultrasonic sensor. It implements the host side of the safety contract (the heartbeat that keeps
motor power alive, the arming handshake, READY/ARMED feedback), so an application only decides what the robot
should do and calls one function per control cycle.

- **One C11 library, one header.** `keel.h` is the whole API for C and C++ programs and for the Python and Java
  bindings. It has no macros for constants, no padding in its structs and versioned structs, so bindings
  stay compatible across 1.x releases.
- **Stops and faults.** Any thread may stop the robot, but only the control loop's thread may make it move.
  A missed loop deadline, wrong feedback or an I/O failure performs a Category-0 stop and reports one cause.
  If the process dies, the MCU removes motor power when the heartbeat stops.
- **A simulator built in.** The same state machine runs against the production safety-MCU firmware policy on
  any Linux PC, on the real-time clock or on a deterministic virtual clock.
- **Installed in one command** on the Pi from Debian packages, with the device permissions, real-time priority
  and boot configuration it needs.
- **A camera library beside it.** `libkeel-camera` drives a Camera Module 2 or 3 on the Pi's camera connector
  through libcamera and the Pi's hardware encoders: raw frames, H.264 and MJPEG to files and network viewers, and
  stills. It is a separate library; `libkeel.so` has no camera code. See [docs/camera.md](docs/camera.md).

Version 1.0.0, ABI 1.0, shared library `libkeel.so.1`. MIT licence.

## Quick start

A complete program: open the board, arm it, drive forward at 20% for about two seconds, stop.

```c
#include <keel/keel.h>
#include <stdio.h>

int main(void)
{
    keel_open_report report = {.struct_size = sizeof report};
    keel_device *dev = NULL;
    int32_t rc = keel_open(NULL, &dev, &report); /* the board, or KEEL_BACKEND=simulator */
    if (rc != KEEL_OK) {
        fprintf(stderr, "keel_open: %s: %s\n", keel_status_name(rc), report.hint);
        return 1;
    }

    keel_cycle_input in = {.struct_size = sizeof in, .health = KEEL_HEALTH_ALL};
    keel_snapshot out = {.struct_size = sizeof out};
    uint64_t next = 0;
    keel_now_us(dev, &next);
    rc = keel_request_arm(dev); /* an explicit decision; arming takes about 0.7 s of cycles */

    /* The control loop: one keel_cycle every 10 ms on the device clock. */
    for (int i = 0; rc == KEEL_OK && i < 300; ++i) {
        in.cycle_id++;
        if (out.state == KEEL_STATE_ARMED) {
            in.command = KEEL_CYCLE_DRIVE;
            in.left = 200; /* permille: 20% forward */
            in.right = 200;
        }
        rc = keel_cycle(dev, &in, &out);
        next += 10000;
        if (rc == KEEL_OK) {
            rc = keel_sleep_until(dev, next);
        }
    }
    if (rc != KEEL_OK) {
        keel_fault fault = {.struct_size = sizeof fault};
        keel_get_fault(dev, &fault);
        fprintf(stderr, "stopped: %s (cause %s)\n", keel_status_name(rc),
                keel_fault_cause_name(fault.cause));
    }
    keel_close(dev, NULL); /* Category-0 stop: enable off, motors coast, heartbeat stops */
    return rc == KEEL_OK ? 0 : 1;
}
```

Build and run it on the robot (wheels off the ground the first time), or on any Linux PC against the simulator:

```sh
cc -std=c11 drive.c $(pkg-config --cflags --libs keel) -o drive
./drive                          # on the robot
KEEL_BACKEND=simulator ./drive   # anywhere
```

The loop is the whole contract: call `keel_cycle` (or `keel_tick`) every 10 ms from the thread that does the
real work, pass `KEEL_HEALTH_ALL` only when that work succeeded, and resend the motor command every cycle (a
command lives 500 ms). [docs/safety.md](docs/safety.md) explains what each status means and what the library
does about it. The test suite compiles and runs this exact example.

## Install on the Raspberry Pi

On a Raspberry Pi 4 with the robot's operating system, 64-bit Ubuntu 26.10:

```sh
curl -fsSL https://github.com/BisocMOrg/Keel/releases/latest/download/install.sh | sudo sh -s -- --user "$USER"
sudo reboot
```

The script verifies every download against the release's `SHA256SUMS`, installs `libkeel1`, `libkeel-camera1`,
`keel-common` and `keel-tools`, and runs `keel-setup`, which adds the user to the device groups and turns SPI0 off
and I2C1 on in a marked, backed-up block of `config.txt`. The camera library links the distribution's libcamera,
which has no stable ABI, so on another distribution build the packages there ([docs/deploy.md](docs/deploy.md),
section 1). Add `--dev` for the headers, CMake package and pkg-config file,
`--version 1.0.0` to pin a release, `--dry-run` to see what it would do. [docs/deploy.md](docs/deploy.md) has
the manual two-step install, what each package contains and how to run an application as a service.

## Build from source

Requirements: a C11 compiler, CMake 3.21 or later and Linux (the board backend needs kernel headers 5.10 or
later for the GPIO character device). Python 3 and a C++ compiler are needed for the test suite only. The camera
library also needs a C++17 compiler and `libcamera-dev` (and `libjpeg-dev` for large stills); without libcamera it
is skipped.

```sh
cmake --preset release           # or: dev, asan-ubsan, tsan, sim-only
cmake --build --preset release
ctest --preset release
sudo cmake --install build/release
```

Without presets: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`. Useful options:

| Option | Default | Effect |
|---|---|---|
| `KEEL_BACKEND_KEEL_R3` | ON on Linux | the backend for the R3 board |
| `KEEL_BACKEND_SIM` | ON | the simulator backend |
| `KEEL_PACKAGING` | OFF | also install the udev rule, group, i2c-dev loading, rtprio limit and `keel-setup` |
| `KEEL_BUILD_TESTS` | ON when top level | the test suite |
| `KEEL_WERROR` | OFF (ON in presets) | warnings are errors |
| `KEEL_CAMERA` | AUTO | the camera library and `keel-camera`: built when libcamera is found (`ON` requires it, `OFF` skips it) |
| `KEEL_CAMERA_JPEG`, `KEEL_CAMERA_LIBAV` | AUTO | libjpeg for stills above 1920x1920; FFmpeg's libavformat for container and streaming-protocol outputs |

Cross-compile for the robot from an x86-64 Linux machine with `cmake --preset cross-arm64` (needs
`gcc-aarch64-linux-gnu`); the Debian packages are built with `dpkg-buildpackage -a arm64 -Pnocheck -b`. One
aarch64 build serves every Pi 4 running 64-bit Linux; [docs/deploy.md](docs/deploy.md) explains why.

Use the installed library from CMake or pkg-config:

```cmake
find_package(Keel 1.0 REQUIRED)
target_link_libraries(app PRIVATE Keel::keel)          # or Keel::keel_static
```

```sh
cc app.c $(pkg-config --cflags --libs keel) -o app            # the shared library
cc app.c $(pkg-config --cflags --libs keel-static) -o app     # libkeel.a, also for port.h
```

## The simulator

Every build includes a simulator of the baseboard: the motor bridges and enable gate, the LED driver, the
buttons, the ultrasonic sensor, the servo port and the production safety-MCU policy, stepped every simulated
millisecond. Select it with `KEEL_BACKEND=simulator` (when the application opens with defaults) or with
`KEEL_BACKEND_SIMULATOR` in `keel_open_options`. With `KEEL_OPEN_VIRTUAL_CLOCK` time advances only inside
`keel_sleep_until`, so tests run deterministically and faster than real time. `keel_sim_set` injects button
presses, distances, I/O failures, MCU resets and late PWM wakes; `keel_sim_get_state` returns the pin levels and
the MCU's view. The simulator never falls back silently: `KEEL_CAP_SIMULATED` and `KEEL_SNAP_SIMULATED` identify
it.

## Other languages

The official Python and Java libraries are in [`../bindings`](../bindings/README.md) and are released with libkeel
at the same version:

| Language | Package | Documentation |
|---|---|---|
| Python 3.11 or later | `keel-robot` (`import keel`); on the robot the Debian package `python3-keel` (`install.sh --python`) | [Python binding](../bindings/python/README.md) |
| Java 25 or later | `io.github.bisocmorg:keel` (module `io.github.bisocmorg.keel`) | [Java binding](../bindings/java/README.md) |

Both are pure code over `keel.h` with nothing to compile: on the robot they load the system library
`libkeel.so.1`, elsewhere the library named by `KEEL_LIBRARY` (an absolute path). [docs/bindings.md](docs/bindings.md)
gives the rules every binding follows and the pitfalls of each runtime, including C#, for which no binding is
built. `tests/consumers/keel_ctypes.py` is the minimal reference binding that libkeel's own tests use.

## Documentation

| Document | For |
|---|---|
| [docs/safety.md](docs/safety.md) | application developers: states, the loop contract, stops, statuses, the fault catalogue and the request-versus-state table |
| [docs/bindings.md](docs/bindings.md) | binding authors: loading, structs, strings, errors, the loop thread, per-language pitfalls |
| [../bindings](../bindings/README.md) | Python and Java developers: the official bindings |
| [docs/deploy.md](docs/deploy.md) | putting it on a robot: builds per target, packages, permissions, services |
| [docs/camera.md](docs/camera.md) | the camera: `keel-camera` recipes (viewers, recording, stills, frames for a model), the C API and the Python and Java camera APIs |
| [docs/architecture.md](docs/architecture.md) | maintainers and porters: layers, locks, backends, tests |
| [docs/spec.md](docs/spec.md) | the complete specification |
| `include/keel/keel.h`, `include/keel/camera.h` | the API reference (Doxygen: `mkdir -p build && doxygen Doxyfile`) |
| [CHANGELOG.md](CHANGELOG.md) | what changed in each release |

## Tests

`ctest --preset dev` runs the whole suite (188 cases, 263 with the camera library): the core against the production
MCU policy, every cell of the arbitration table from both thread columns, every hook failure at every entry
point, seeded random walks with per-millisecond invariants, thread and latency tests, the ABI gates (exported
symbols, struct layouts, constants, documentation), the C, C++, README and Python consumers, the packaging
scripts, and the camera library against a synthetic camera. Hardware tests run on the robot only.

## Licence

MIT. Copyright (c) 2026 BisocM.
