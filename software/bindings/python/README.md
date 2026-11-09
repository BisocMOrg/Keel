<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# keel-robot: the Python binding of libkeel

`keel` is the Python interface to **libkeel**, the host library of the Keel robot baseboard on a Raspberry Pi 4: two
DC motors behind a hardware enable gate, a safety MCU with a heartbeat and an arming handshake, six RGB LEDs, four
buttons with indicator LEDs and an ultrasonic sensor. The binding is pure Python (ctypes), has no dependencies and
loads the system's `libkeel.so.1`, so there is exactly one copy of the safety code on a robot. libkeel's built-in
simulator runs the same safety state machine on any Linux PC. `keel.camera` drives the robot's Raspberry Pi camera
through its own library, `libkeel-camera.so.1` ([Camera](#camera)).

Version 1.0.0, for libkeel 1.0 (ABI 1.0). Python 3.11 or later, Linux. MIT licence.

## Installation

`pip install keel-robot` (not `keel`, which is an unrelated project on PyPI).

The binding needs a libkeel with the same major version and at least its minor version, and the newest
`keel-robot` may need a newer libkeel than the robot has. Pin the minor of the installed library:

```sh
dpkg-query -W libkeel1                        # for example: libkeel1   1.0.0
python3 -m venv --system-site-packages ~/robot
~/robot/bin/pip install "keel-robot==1.0.*"
```

On the robot the recommended way is the Debian package, which depends on the matching `libkeel1` and works for the
system `python3`, systemd services and venvs made with `--system-site-packages`:

```sh
curl -fsSL https://github.com/BisocMOrg/Keel/releases/latest/download/install.sh | sudo sh -s -- --user "$USER" --python
python3 -c 'import keel; print(keel.version_string())'
```

On a desktop, build a simulator-only libkeel and point the binding at it:

```sh
cmake --preset sim-only && cmake --build --preset sim-only --target keel     # in software/libkeel
export KEEL_LIBRARY="$PWD/build/sim-only/libkeel.so.1" KEEL_BACKEND=simulator
```

The binding loads `KEEL_LIBRARY` (an absolute path; an empty value counts as unset) or else `libkeel.so.1` through
the dynamic loader. `import keel` never touches the library, so type checking and documentation builds work
anywhere; the library is loaded on first use or by `keel.load()`.

## Quick start

For sequential programs, subclass `KeelTask`. The task runner opens the device and automatically waits for
arming when a movement is requested. Each movement finishes and removes motor drive before returning.

```python
import time

import keel
from keel.task import KeelTask


class FirstTask(KeelTask):
    """Drive, pause, then turn using bounded actions."""

    def run(self) -> None:
        """Execute the student's sequence while the runner manages the hardware."""
        self.keel.drive_for(20, 20, 1000)  # percent left/right, milliseconds
        time.sleep(0.5)
        self.keel.turn_left_for(600)


keel.run(FirstTask())
```

`self.keel.take_image()` returns a BGR888 camera frame; `frame.array()` gives a numpy view when numpy is
installed. Students implement behaviours such as `approach_box` in their own methods. There is no built-in
box detection or OpenCV algorithm. `self.keel.stop()` cancels the task. See the
[sequential task guide](../docs/tasks.md) for the Java equivalent, timeouts, units and failure handling.
These task APIs are new source functionality: build/install the updated binding if using an older release.

The [sequential_moves example](examples/sequential_moves.py) accepts `--simulator` or the explicit
`--allow-motion` hardware gate. Task programs use the real-time simulator, not `OpenFlag.VIRTUAL_CLOCK`.

## Cycle-level control

Open the board, arm it, drive forward at 20% for about three seconds, stop:

```python
import keel


def step(cycle: keel.Cycle) -> None:
    if cycle.snapshot.state == keel.State.ARMED:
        cycle.drive(200, 200)  # permille, sent again every cycle


with keel.open() as device:  # the board, or KEEL_BACKEND=simulator
    loop = keel.ControlLoop(device, step)
    device.request_arm()  # before the first tick every thread is the owner
    loop.run(cycles=300)  # 3 s on the device clock, on this thread
    device.disarm()
```

On a robot the wheels turn: put it on a stand the first time. `keel.open()` with no arguments opens the board, or
the backend named by `KEEL_BACKEND`; it never falls back to the simulator silently. Tests and tools that must never
reach a board open the simulator explicitly:

```python
device = keel.open(backend=keel.Backend.SIMULATOR, flags=keel.OpenFlag.VIRTUAL_CLOCK)
```

The `examples` directory has five cycle-level programs, each runnable with `--simulator`: `buttons` (buttons mirrored
on their LEDs; `--drive LEFT RIGHT MS`), `drive`, `leds`, `ranging` and `safety`. They arm only on the simulator or
with `--allow-motion` (`buttons`: `--drive`). The `sequential_moves` example demonstrates the task API with the same
explicit hardware gate. Three more use the camera and need no board: `camera_frames` (frames
into vision code), `camera_stream` (video to the network or a file) and `camera_still` (a JPEG photo).

## Package layout

`import keel` gives every public name. The subpackages group the types by what they describe, and each can be
imported on its own:

| Module | Contents |
|---|---|
| `keel` | `open()`, `Device`, `load()` and the library metadata, `stop_on_signals()`, the open options and report (`Backend`, `OpenFlag`, `OpenReport`, `OpenResource`, `OpenWarning`, `Capability`, `TimingClass`), and every name below |
| `keel.errors` | the exception hierarchy, `Status` and `StatusClass` |
| `keel.loop` | `ControlLoop`, `Cycle`, `LoopStats`, `CycleCommand`, `CycleFlag` and `Health` |
| `keel.task` | `KeelTask`, `TaskRobot`, `TaskOptions` and task errors: sequential, bounded actions |
| `keel.state` | `State`, `AckPhase`, `SafetyInput`, `Snapshot`, `SnapshotFlag`, `Fault`, `FaultCause` and `FaultFlag` |
| `keel.peripherals` | `Button`, `ButtonState`, `Led`, `RangeMeasurement`, `RangeStatus` and `SensorPower` |
| `keel.sim` | `SimItem`, `SimFlag` and `SimState` |
| `keel.abi`, `keel.limits` | the ABI identification and the limits of `keel.h`, as constants |
| `keel.camera` | the camera ([Camera](#camera)); its names are not re-exported by `keel` |

`keel._internal` holds the library loaders and the generated structs and calls; it is not API.

## The loop and the safety contract

The rules below describe low-level `ControlLoop` use. The task runtime places a bounded action supervisor
on that loop: it permits only the duration explicitly requested by an action, checks deadlines on the
control thread and commands zero drive between actions. It never clears faults automatically. See
[task execution](../docs/tasks.md#bounds-faults-and-cancellation).

libkeel is the only safety logic; the binding passes every call through unchanged. The contract in brief (the full
story is in [safety.md](https://github.com/BisocMOrg/Keel/blob/main/software/libkeel/docs/safety.md)):

- **One loop, one thread.** A `keel_cycle` every 10 ms from the thread that does the real work keeps the safety
  MCU's permission alive. `keel.ControlLoop` runs it: on its own daemon thread with `start()` or a `with` block, or
  on the calling thread with `run()`. Health is `Health.ALL` only when the step returned normally within its
  budget (40 ms by default), reported healthy I/O (`cycle.io_ok`) and handed over valid duties; anything else
  fail-stops the device on purpose.
- **Stop from anywhere, go only from the loop.** The control owner is the thread of the first successful tick.
  `disarm()`, `emergency_stop()`, `coast()`, `servo_end()` and `close()` work from any thread in every state; arming,
  driving and clearing faults only from the owner. Other threads post intents with `loop.set_command(left, right)`,
  `loop.brake()` and `loop.coast()` (one-shot: post again within the 500 ms command timeout to keep moving), and run
  go calls on the loop thread with `loop.request_arm()`, `loop.clear_fault()` and `loop.submit(fn)`, which return
  futures.
- **Commands expire.** A motion command lives for the command timeout and then coasts. `Cycle.drive()` is sent again
  every cycle; a duty outside -1000..1000 handed to the loop fails that cycle's health check, a latched stop that
  `loop.clear_fault()` recovers from.
- **Faults latch.** A FAULT-class status means the device performed a Category-0 stop and stays FAULTED until
  `clear_fault()` from the owner. The loop keeps running in FAULTED, so the application can show an error colour
  (the LEDs work in every state), read buttons and clear the fault. Nothing is ever cleared or armed automatically.

## Threads, the GIL and signals

- ctypes releases the GIL around every foreign call, so a stop from a user-interface thread runs while the loop
  thread sleeps in libkeel. A cycle makes four foreign calls (two clock reads, `keel_cycle` and `keel_sleep_until`),
  and after each one the loop thread needs the GIL back. While a loop runs, the switch interval is 1 ms.
- **Keep CPU-bound Python work out of the loop's process.** CPython does not hand the GIL to a particular waiting
  thread, so a hand-off can take many switch intervals when several threads compete. Measured against the
  real-time simulator on CPython 3.12 with the robot armed, 3 s per run: one busy pure-Python thread stretched the
  loop's largest gap between cycles from 10 ms to 13-17 ms; two stretched it to 40-56 ms, past the 40 ms stall
  budget, and in some runs latched a fault (`TICK_LATE`, or `HEALTH_FLAGS` when the step budget ran out); three
  latched `TICK_LATE` or `HEARTBEAT_LATE` in every run. Run heavy computation in another
  process, started with the `spawn` or `forkserver` method (a forked child cannot use the parent's device:
  `HandleError`), or use free-threaded CPython 3.14t, where the largest gap stayed at about 10 ms with any number of
  busy threads.
- **Stop latency in Python.** A stop first needs the GIL, because a few lines of Python run before the foreign call.
  Behind other busy Python threads that wait is not bounded by the switch interval (measured: up to 15 ms with two
  busy threads); behind a C extension that holds the GIL it is unbounded. The hard bound is then the hardware's: the
  motor lease (70 ms after the last tick) and the MCU heartbeat timeout (about 86 ms after the last heartbeat),
  because a held GIL also stops the loop from ticking. Free-threaded builds (3.14t) have no such wait.
- **Ctrl-C** raises `KeyboardInterrupt`, the stop button: when it escapes a device's or a loop's `with` block or
  `run()`, the binding makes an emergency stop first, so the final fault record shows `USER_STOP`.
- **SIGTERM, SIGHUP and SIGQUIT** end the process without cleanup by default; SIGHUP arrives when the terminal or
  the ssh session goes away. `keel.stop_on_signals(*targets)` installs handlers for SIGINT, SIGTERM, SIGHUP and
  SIGQUIT, the signals libkeel's own tools stop on (pass `signals=` to choose others). Each handler emergency-stops
  every target and then raises `KeyboardInterrupt` (SIGINT) or `SystemExit(128 + signal)`, so the program unwinds
  through its `with` blocks and exits with the shell's status for that signal (130 after Ctrl-C, 143 after SIGTERM):

  ```python
  with keel.open() as device, keel.stop_on_signals(device):
      ...
  ```

- Python runs signal handlers on the main thread between bytecodes, so a handler may call `emergency_stop()` or
  `loop.stop()`, which take no binding lock. If a handler should only flag the request, store `True` in a plain
  attribute that the step reads; do not use `threading.Event`, whose `set()` takes a lock the interrupted thread may
  hold. Never call libkeel from a native signal handler (a ctypes callback installed with the C library's `signal`
  or `sigaction`).
- `ControlLoop.run()` refuses to run on an asyncio event-loop thread; async applications use `start()` and wrap the
  loop's futures with `asyncio.wrap_future`.

## Errors

Every status other than OK raises the exception of its class, so an application catches the class:

| Exception | Raised for | Device after the call |
|---|---|---|
| `RejectedError` (subclasses `InvalidArgumentError`, `StateError`, `AgainError`, `UnsupportedError`, `ConflictError`) | REJECTED: refused | unchanged, except that a refused motion request while ARMED coasts |
| `SafetyFaultError` (`.fault` holds the record) | FAULT, undefined codes | latched FAULTED; Category-0 stop done |
| `PeripheralError` | an LED or ultrasonic failure | safety state unchanged |
| `SetupError` (`.report` holds the open report and its remedy) | every failed `keel.open()` | no device |
| `HandleError` | a closed, stale or inherited handle | unusable by this caller |
| `LibraryError` | loading, ABI or symbol failure | no status |

`InvalidArgumentError` is also a `ValueError`. Every status error carries `status`, `code`, `name`, `description`,
`status_class` and `function`. `close()` never raises; its status is in `close_status` and the final fault record in
`final_fault`.

**A `TypeError` or `OverflowError` from a device method means that no call was made.** ctypes would silently
truncate an integer that does not fit its C type, so the binding refuses it first. The only exception is a motor
duty, which is saturated to the int32 range so that libkeel sees it as out of range, refuses it and, while ARMED,
coasts, exactly as for any other out-of-range duty.

## The simulator and testing your own code

The simulator models the motor bridges and enable gate, the LED driver, the buttons, the ultrasonic sensor, the servo
port and the production safety-MCU policy. With `OpenFlag.VIRTUAL_CLOCK`, time moves only inside `sleep_until()`,
so a test can drive arm, drive, fault and recovery sequences deterministically in milliseconds with
`loop.run(cycles=...)` or `loop.run(until=...)`. `device.sim_set()` injects button presses, distances, I/O failures,
MCU resets and late PWM wakes; `device.sim_state()` returns the pin levels and the MCU's view.

## Camera

`keel.camera` drives a Camera Module 2 or Camera Module 3 on the Raspberry Pi's camera connector through
`libkeel-camera.so.1`, the camera library of libkeel ([camera.md](https://github.com/BisocMOrg/Keel/blob/main/software/libkeel/docs/camera.md)).
It needs neither `libkeel.so` nor a `Device`, and a program that uses only the camera never loads the board library.
The camera library talks to libcamera and the Pi's hardware encoders directly; it starts no helper programs.

- **Install.** `install.sh` installs `libkeel-camera1`, which `python3-keel` recommends; the binding needs it for
  camera use only. The user must be in the `video` group: `sudo keel-setup --user NAME`, then log in again. numpy is
  optional: `sudo apt install python3-numpy`, or `pip install "keel-robot[numpy]"`.
- **Loading.** The camera library is loaded on the first camera call, by its SONAME or from the absolute path in
  `KEEL_CAMERA_LIBRARY`, and independently of `libkeel.so.1`. A missing library raises `CameraLibraryError` (a
  `keel.LibraryError`) naming the package.

```python
from keel import camera

with camera.open(width=640, height=480, pixel_format=camera.PixelFormat.BGR888) as cam:
    cam.set_encoder(camera.Codec.H264)
    cam.start_output("drive.h264")  # the library records from its own threads
    cam.start()
    for count, frame in enumerate(cam.frames(), start=1):
        image = frame.array()  # numpy (480, 640, 3), B, G, R: a view of the frame, no copy
        if count == 30:  # about one second
            break
    cam.stop()
    still = cam.capture_still(width=1920, height=1080)
    with open("photo.jpg", "wb") as photo:
        photo.write(still.data)
```

- **Frames.** `read_frame()` and the `frames()` iterator give `Frame` objects: the layout, the sensor's sequence
  number and the start of exposure on `CLOCK_MONOTONIC` (the robot's control-loop clock), and the pixels. Open the
  camera with `PixelFormat.I420` (the default), `RGB888` or `BGR888`; the Pi's image signal processor converts at no
  CPU cost. `frame.array()` is a numpy view of the frame's own buffer, (height, width, 3) for the packed formats and
  OpenCV's (height * 3 // 2, width) layout for I420; `frame.plane(0)` is the greyscale Y plane; `frame.data` (a
  memoryview) and `frame.to_bytes()` (rows without padding) need no numpy. The queue keeps the newest frame by
  default (`frame_queue=1`), so a slow model never slows the camera; `stats()` counts the frames it dropped.
- **Encoded video.** `set_encoder(Codec.H264)` or `Codec.MJPEG` before `start()`. `read_chunk()` and `chunks()` give
  one H.264 access unit or one JPEG image at a time; concatenated, they are a stream FFmpeg reads. `start_output()`
  makes the library write the stream itself: a `.h264` or `.mjpeg` file, `tcp://HOST:PORT` (a receiver that
  listens), `tcp://:PORT?listen` (up to four viewers that come and go, each from its next keyframe) or
  `udp://HOST:PORT`. View a listening robot with
  `ffplay -f h264 -fflags nobuffer -flags low_delay -framedrop tcp://ROBOT:5000`, or read it from OpenCV on another
  computer with `cv2.VideoCapture("tcp://ROBOT:5000", cv2.CAP_FFMPEG)`. `.mp4`, `.mkv`, `rtsp://` and the other
  container and protocol outputs need a libkeel-camera built with FFmpeg's libavformat; the release packages are
  not, and raise `CameraUnsupportedError`.
- **Stills.** `capture_still()` and `capture_still_file()` take one JPEG while capture is stopped, by default at the
  sensor's full resolution (3280x2464 on Camera Module 2).
- **Camera Module 3.** `af_mode`, `af_range` and `lens_position` control the autofocus; `trigger_autofocus()` runs
  one scan. On Camera Module 2, which has a fixed-focus lens, they raise `CameraUnsupportedError`.
- **Threads.** The thread that opens a camera owns it: every method but `close()` must be called from that thread,
  or it raises `CameraThreadError` with no effect. `close()` works from any thread and wakes a read blocked in the
  owner, whose `frames()` loop then ends. A camera that is dropped without being closed is closed by a finalizer,
  with a `ResourceWarning`, and every camera still open is closed at interpreter exit, so files are finished.
- **Errors.** Every failure raises a `CameraError` subclass chosen by the status (`keel.camera.errors`), with the
  error record libkeel-camera wrote in `.report`; the message names the remedy, for example "permission denied on
  /dev/media0: add the user to the video group (sudo keel-setup --user NAME), then log in again".
  `CameraArgumentError` is also a `ValueError`, `CameraTimeoutError` a `TimeoutError` and `CameraPermissionError` a
  `PermissionError`.

## The import name

The import package is `keel`, after libkeel's `keel_` prefix. If the unrelated `keel` distribution is installed in
the same environment, the two overwrite each other's `site-packages/keel`; keep them in separate environments.

## Troubleshooting

- **`LibraryError: cannot load libkeel.so.1`.** On the robot, install `libkeel1` (`install.sh` above); elsewhere,
  set `KEEL_LIBRARY` to the absolute path of a libkeel build.
- **`LibraryError: ... has ABI 1.0, but keel-robot 1.1.0 needs ABI 1.1`.** The binding is newer than the library:
  upgrade `libkeel1` or install the `keel-robot` release that matches it.
- **`SetupError`.** The message ends with the remedy from the open report, for example `BCM8 is used by spi0; remedy:
  set dtparam=spi=off`; `sudo keel-setup --user NAME` fixes device permissions and the boot configuration.
- **`error: externally-managed-environment`.** Ubuntu and Raspberry Pi OS refuse `pip install` into the system Python
  (PEP 668): use the Debian package or a venv made with `--system-site-packages`.
- **`CameraLibraryError: cannot load libkeel-camera.so.1`.** Install `libkeel-camera1`; elsewhere, set
  `KEEL_CAMERA_LIBRARY` to the absolute path of a libkeel-camera build. `keel-camera list` checks the camera and its
  permissions from the shell.

## Building and testing

The tests run against a simulator-only libkeel build, so they cannot reach a robot's board: every test opens the
simulator explicitly, and a suite-wide guard fails any call that could move a robot.

```sh
cmake --preset sim-only -DKEEL_BUILD_TESTS=OFF -DKEEL_TOOLS=OFF      # in software/libkeel; needs libcamera-dev
cmake --build --preset sim-only --target keel keel_camera
export KEEL_LIBRARY=/path/to/software/libkeel/build/sim-only/libkeel.so.1
export KEEL_CAMERA_LIBRARY=/path/to/software/libkeel/build/sim-only/libkeel-camera.so.1
cd software/bindings/python
python -m pip install --group test                                  # pip 25.1 or later
python -W error -m pytest
```

The camera tests (`tests/camera`, `tests/internal/camera`) run on the synthetic camera of that test build, which
`KEEL_CAMERA_TEST_SOURCE=1` selects (the CMake presets compile it; the packages never do): a Camera Module 2 and a
Camera Module 3 whose frames carry a pattern the tests recompute, and faults they inject. They never load
`libkeel.so`.

On the robot, with the libkeel Debian packages installed and `KEEL_LIBRARY` unset, the hardware tests check the
binding against the board without moving it; an operator confirms the LED colours:
`KEEL_TEST_HARDWARE=1 python3 -m pytest -m hardware -p no:cacheprovider`. The camera's hardware tests in the same
run use the connected camera and skip when there is none.

The suite follows the package: `tests/internal` checks the loader and the generated layer against `keel.h`, and
`tests/internal/camera` the camera's against `camera.h`; `tests/loop` the control loop, `tests/camera` the camera
(its errors in `tests/camera/errors`), and the files in `tests` the device, errors, lifetime, threads, conformance,
examples and portability; `tests/support` holds the helpers they share.

`abi.py`, `limits.py` and every `_generated.py` (one per package, and `_internal/_generated.py`) are generated from
`keel.h`, and their counterparts under `camera/` and `_internal/camera/` from `camera.h`, by
`software/bindings/tools/keelgen.py`; do not edit them. After a header change, run
`python3 software/bindings/tools/keelgen.py --lang python` and commit the result; CI fails while they differ.

## API reference

Every public item has a docstring, and the generated enumerations carry the comments of `keel.h`. To build the HTML
reference:

```sh
pip install pdoc
pdoc -o build/docs/python keel
```

## Licence

MIT. Copyright (c) 2026 BisocM. See [LICENSE](https://github.com/BisocMOrg/Keel/blob/main/software/bindings/python/LICENSE).
