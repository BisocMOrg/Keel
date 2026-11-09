<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Changelog

All notable changes to libkeel. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/): the major version is the ABI major and the
SONAME (`libkeel.so.1`), minor versions only add, patch versions only fix.

## [1.0.0] - 2026-11-07

The first release of libkeel, the host library for the Keel robot baseboard (R3) on a Raspberry Pi 4.
ABI 1.0, SONAME `libkeel.so.1`.

### Added

- `keel.h`: the application and binding API. 37 exported functions, every constant an enumerator, every struct
  versioned by `struct_size`, every status in exactly one class (OK, REJECTED, FAULT, PERIPHERAL, SETUP,
  HANDLE).
- Safety: the MCU heartbeat and arming acknowledgement, READY/ARMED feedback rules, a fault record with a cause
  catalogue, a 1 s re-arm hold-off and sticky causes for board and library defects.
- Threading: any thread may stop the robot (`keel_disarm`, `keel_emergency_stop`, `keel_set_motors(dev, 0, 0)`,
  `keel_servo_end`, `keel_close`); only the control loop's thread may make it move. ENABLE falls before any lock
  is taken.
- Motion: signed permille duty at 100 Hz, a deadman timeout (500 ms by default), deferred direction reversal and
  braking.
- Servo bursts under the MCU lease (simulator; J5 is not fitted on R3) and HC-SR04 ranging with results as
  statuses, never sentinel distances.
- RGB LEDs, button LEDs and debounced buttons, usable in every state.
- A simulator backend with the production safety-MCU policy, on the real-time or a virtual clock.
- `port.h`: the C porting interface for other platforms (static library and source builds only).
- `libkeel-camera` (`camera.h`, `libkeel-camera.so.1`, ABI 1.0), a separate library for the Raspberry Pi camera:
  Camera Module 2 and 3 through libcamera, raw frames in I420, RGB888 or BGR888, H.264 and MJPEG on the Pi's
  hardware encoders, outputs the library writes itself (files, TCP, a listening TCP port for several viewers, UDP;
  containers and streaming protocols with FFmpeg's libavformat), JPEG stills up to the full sensor resolution and
  Camera Module 3 autofocus. Bounded queues that drop and count, never block. A synthetic camera for tests
  (`KEEL_CAMERA_TEST_HOOKS`). The `keel-camera` tool lists, captures stills, records, streams and writes raw frames.
- Debian packages `libkeel1`, `libkeel-dev`, `keel-common` and `keel-tools` for the board, and `libkeel-camera1`,
  `libkeel-camera-dev` and `keel-camera` for the camera; `install.sh` for a one-command install from a GitHub
  release (the camera packages when the system has their libcamera, or never with `--no-camera`; `--python` adds
  `python3-keel`, the Python binding released beside them); `keel-setup` for group membership (including `video`
  for the camera) and the boot configuration.
- CMake package `Keel` (`Keel::keel`, `Keel::keel_static`, component `camera` with `Keel::keel_camera`) and
  pkg-config `keel` (shared), `keel-static` (`libkeel.a` by path, with the port API) and `keel-camera`.
- Releases also carry the Python and Java bindings (`software/bindings`), built from the same tag and listed in
  the same `SHA256SUMS`.
- A reference Python ctypes binding in the test suite, which reads returned strings with a scan bounded by
  `KEEL_STRING_MAX`, and the binding rules in `docs/bindings.md`.
- Language-neutral conformance scenarios (`tests/bindings/scenarios.json`): every production binding replays
  them through its public API, and the test suite replays them against the library through the reference ctypes
  binding (CTest `ctypes.scenarios`).

### Known limitations

Checked on the robot on 2026-11-07 (SPEC section 24.2): ARMED falls 51-71 ms after the process is killed (design
bound about 86 ms), motor PWM lateness under load has a 99.9th percentile of 216 us with no overruns over 600 s,
and `keel-doctor` reports 13 checks passed, 1 skipped (the firmware's throttling flags are unavailable) and 0 failed
after an install from the Debian packages. Not yet characterised: ranging edge timing (no sensor was fitted), the
10 ms grace windows, binding runtimes under load and the udev rule on Raspberry Pi OS. The servo path is proven on
the simulator only.

The camera library was checked on the robot on 2026-11-08 with a Camera Module 2 (SPEC section 26.12). Camera
Module 3 autofocus is proven on the test source only. The release's camera packages (`libkeel-camera1`,
`libkeel-camera-dev`, `keel-camera`) depend on the robot's libcamera (`libcamera0.7`, Ubuntu 26.10); the board
packages do not depend on libcamera. Other distributions build the camera library themselves.
