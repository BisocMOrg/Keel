<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Sequential robot tasks

The Java and Python task APIs let a program read as a sequence of actions. Students implement their own
behaviours, such as `approachBox` or `approach_box`, using bounded motor commands and camera images. They do
not create a hardware control loop or poll the arming state.

This API is additive. `Device`, `ControlLoop` and the native C contract remain available for programs that
need direct cycle-level control. The task API is new source functionality; build the updated bindings to use
it rather than assuming an earlier 1.0.0 release artifact already contains it.

## First task in Java

Save this as `FirstTask.java`. Running it requests real movement unless `KEEL_BACKEND=simulator` is set.
For a first hardware run, keep the wheels clear of the bench.

```java
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.task.KeelTask;

public final class FirstTask extends KeelTask {
    /**
     * Runs sequential actions, each ending with zero motor drive.
     * @throws Exception if an action fails or the task is interrupted
     */
    @Override
    protected void run() throws Exception {
        keel.driveFor(20, 20, 1000);
        Thread.sleep(500);
        keel.turnLeftFor(600);
    }

    /**
     * Executes this task and closes its hardware resources.
     * @param args unused command-line arguments
     * @throws Exception if opening the robot or running the task fails
     */
    public static void main(String[] args) throws Exception {
        Keel.run(new FirstTask());
    }
}
```

```sh
java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar FirstTask.java
```

The first movement waits for the safety controller to arm. `driveFor` requests 20% duty on each motor for
one second and returns after motor drive has been removed. The next line pauses for half a second with
zero motor drive. `turnLeftFor` then turns with the left motor backwards and the right motor forwards, at 20%
duty, for 600 ms. Finishing `run` disarms and closes the task's resources.

## The same task in Python

```python
import time

import keel
from keel.task import KeelTask


class FirstTask(KeelTask):
    """Drive, pause, then make a timed left turn."""

    def run(self) -> None:
        """Run three sequential actions; the runner handles the hardware."""
        self.keel.drive_for(20, 20, 1000)
        time.sleep(0.5)
        self.keel.turn_left_for(600)


keel.run(FirstTask())
```

The execution model is the same in both languages. Task motor durations are **milliseconds**, including in
Python. Ordinary `time.sleep` still takes seconds; ordinary `Thread.sleep` takes milliseconds.

## Operations and units

| Java | Python | Behaviour |
|---|---|---|
| `Keel.run(task)` | `keel.run(task)` | Open the robot, run the student's task, then stop and close resources |
| `keel.driveFor(left, right, ms)` | `self.keel.drive_for(left, right, ms)` | Run signed left/right duty percentages for a bounded duration; stop before returning |
| `keel.turnLeftFor(ms)` | `self.keel.turn_left_for(ms)` | Left motor -20%, right motor +20%, then stop |
| `keel.turnRightFor(ms)` | `self.keel.turn_right_for(ms)` | Left motor +20%, right motor -20%, then stop |
| `keel.takeImage()` | `self.keel.take_image()` | Lazily open the camera and return a BGR888 frame with motor drive off |
| `keel.stop()` | `self.keel.stop()` | Cancel this task and disarm; subsequent operations are refused |

Task motor duties are percentages from **-100 to +100**, unlike the low-level `Cycle.drive` and
`setCommand`/`set_command` methods, which take permille from -1000 to +1000. Positive duty requests forward
rotation; negative duty reverses it. Duty is an electrical command, not a measured speed.

A completed action confirms zero electrical command and output. Wheels may coast before reaching physical
standstill; the task API has no wheel encoders and does not measure travelled distance.

`turnLeftFor(600)` is a timed turn. It does not promise a particular angle. An angle-based or distance-based
behaviour requires calibrated observations or additional feedback and belongs in the student's program.

`takeImage` returns a frame from the running camera stream, not a full-resolution JPEG still. The camera is
opened on first use, in the task's thread, and closed with the task. Python can use `frame.array()` with the
optional numpy dependency; Java can use `frame.toBufferedImage()` or its pixel buffer. See the
[camera guide](../../libkeel/docs/camera.md) for frame layouts and calibration.

## Students own the behaviour

There is no `approachBox`, colour detection, or OpenCV dependency in the Keel API. The student writes those
methods in their task class. For example, the body of a Java task can be:

```java
@Override
protected void run() throws Exception {
    approachBox(Colour.YELLOW);
    Thread.sleep(500);
    keel.turnLeftFor(600);
    approachBox(Colour.RED);
}
```

Here is one possible student implementation. `Colour` and `OpenCV.findBox` represent the lesson's own
types and vision code. This example assumes the vision result supplies visibility, a calibrated distance
in millimetres, and a signed steering correction in percentage points.

```java
/**
 * Approaches a visible box using the lesson's calibrated vision function.
 * @param colour target colour to find
 * @throws Exception if the target is lost, capture fails or movement is refused
 */
private void approachBox(Colour colour) throws Exception {
    while (true) {
        var target = OpenCV.findBox(keel.takeImage(), colour);
        if (!target.found()) {
            throw new IllegalStateException("Cannot see " + colour);
        }
        if (target.distanceMm() <= 30) {
            return;
        }

        double steering = Math.clamp(target.steering(), -5.0, 5.0);
        keel.driveFor(15 + steering, 15 - steering, 100);
    }
}
```

The equivalent Python method is:

```python
def approach_box(self, colour: Colour) -> None:
    """Use the lesson's vision function to approach a visible coloured box."""
    while True:
        target = find_box(self.keel.take_image(), colour)
        if not target.found:
            raise RuntimeError(f"Cannot see {colour}")
        if target.distance_mm <= 30:
            return

        steering = max(-5.0, min(5.0, target.steering))
        self.keel.drive_for(15 + steering, 15 - steering, 100)
```

These are ordinary algorithm loops: look, decide, make a short movement, look again. Each movement has
finished before the next image or OpenCV call begins. The library separately handles the hardware's 10 ms
cycle. The sample uses a 30 mm observation threshold; final physical clearance also depends on sensing
accuracy, camera position and stopping distance.

## Bounds, faults and cancellation

`TaskOptions` provides the same defaults in both languages:

| Bound | Default |
|---|---|
| Maximum individual movement | 30,000 ms |
| Wait for arming | 3,000 ms |
| Total task lifetime | 300,000 ms |
| Camera frame wait | 1,000 ms |

For example, select the real-time simulator explicitly in Java:

```java
TaskOptions options = TaskOptions.defaults().withOpenOptions(
    OpenOptions.builder().backend(Backend.SIMULATOR).build());
Keel.run(new FirstTask(), options);
```

In Python, pass equivalent board settings and, optionally, a shorter total task limit:

```python
from keel.task import TaskOptions

options = TaskOptions(
    board_kwargs={"backend": keel.Backend.SIMULATOR},
    task_timeout_ms=60_000,
)
keel.run(FirstTask(), options)
```

The Java option types above are `io.github.bisocmorg.keel.task.TaskOptions`,
`io.github.bisocmorg.keel.OpenOptions` and `io.github.bisocmorg.keel.Backend`. Option limits must be positive
integer milliseconds, at most one day. A movement's duration may be zero: it validates the arguments and
returns without arming. Two zero duties with a positive duration hold zero drive without arming.

An explicit movement action is permission to attempt arming. Opening the task or taking an image does not
start movement. The runner waits for actual `ARMED` before the movement duration starts. A fault is never
cleared automatically, and a failed or cancelled task cannot resume movement by catching its exception.

The control thread enforces action and task deadlines independently of the student's thread. It only
refreshes a motor command while a valid bounded movement is active. Between movements it commands zero
drive. A completed movement waits for a subsequent zero-output observation before returning, so the next
action cannot overwrite an unprocessed stop.

The stop is applied on the first control cycle at or after the action deadline, subject to normal scheduling
latency. If the control thread itself stalls, the native motor lease and MCU heartbeat limits are the
fallback; a host-language deadline is not a hard real-time physical stopping guarantee.

`stop` is terminal cancellation and can be requested from another thread. Task failure, cancellation or
timeout removes motor permission. Camera operations, invalid commands, thread misuse and hardware faults
report exceptions with context; inspect the IDE hover documentation for each operation's contract.

Java and Python cannot forcibly terminate arbitrary student code safely. If that code never returns, the
hardware deadline still stops the task's motor authority, but the application may remain stuck in that code.
Heavy Python code or a native extension holding the GIL can prevent the Python control thread from running;
the native motor lease and hardware heartbeat limits remain the fallback. See
[threading and the GIL](../python/README.md#threads-the-gil-and-signals).

## Thread and resource ownership

`Keel.run`/`keel.run` executes student code on the calling thread and owns a separate hardware control thread.
Movement and camera methods are used only by that task thread. Java tasks require an ordinary platform
thread, and task execution uses real time: virtual-clock simulator options are rejected. Use the real-time
simulator for task programs, and keep the existing low-level virtual-clock tools for deterministic control
tests.

The task facade is only valid during its execution. Do not retain it for later work or share it as an
unbounded background motor controller. The runner closes the camera, control loop and device on exit. It
does not hand the raw device or control loop to student code.

## Examples and hover documentation

- [Java SequentialMoves](../java/examples/SequentialMoves.java)
- [Python sequential_moves](../python/examples/sequential_moves.py)

The existing `Drive`/`drive` examples also use sequential tasks. The button, LED,
ranging and safety examples use the task-layer operations or managed sessions
appropriate to their interactive/diagnostic behaviour. They no longer construct a
hardware `ControlLoop`. Camera-only examples use the high-level `Camera` API and
retain their ability to run without the board library.

Both examples require explicit `--allow-motion` for hardware or `--simulator` for the real-time simulator.
The public classes, methods, parameters, units and failure modes are documented in Java Javadoc and typed
Python docstrings, so students can read the contract in their editor without leaving the task code.

## Peripherals in a task

Task programs can read `snapshot()` and `capabilities()`, set one or all RGB LEDs,
set the button indicator LEDs, and request a bounded distance measurement. Java
uses names such as `setRgbAll`, `setButtonLeds` and `measureDistance`; Python uses
`set_rgb_all`, `set_button_leds` and `measure_distance`.

These calls do not expose the native device or hardware loop. Distance results
retain their validity/status: a missing echo or unavailable sensor is not a
measurement of an empty path. The camera remains lazy, and peripheral access does
not implicitly start motor movement.

## Interactive sessions

`RobotSession` is the task-layer interface for dashboards and diagnostic programs
that need an explicitly armed, reusable connection. It owns the hardware loop and
can be used from application/request threads. Ordinary student sequences should
continue to use `KeelTask` and its blocking, bounded movement methods.

| Java | Python | Behaviour |
|---|---|---|
| `RobotSession.open()` | `RobotSession()` | Open an initially disarmed connection; close it with try-with-resources or `with` |
| `arm()` | `arm()` | Explicitly request arming and wait for the actual armed state |
| `requestArm()` | `request_arm()` | Return a future for request acceptance; the handshake continues afterwards |
| `refreshDrive(left, right, ms)` | `refresh_drive(left, right, ms)` | Replace a finite motor lease, in percent and milliseconds; requires prior arming |
| `refreshDriveUntil(left, right, deadline)` | `refresh_drive_until(left, right, deadline)` | Use the original monotonic nanosecond expiry of a received command |
| `disarm()` | `disarm()` | Revoke motion and older queued work; keep the connection open |
| `emergencyStop()` | `emergency_stop()` | Latch a hardware emergency stop |
| `clearFault()` | `clear_fault()` | Explicitly attempt fault recovery, without arming or restoring a drive command |
| `snapshot()`, `stats()` | `snapshot()`, `stats()` | Read state and timing information without maintaining the control loop |

For button-driven applications, `buttonsPressed()` / `buttons_pressed()` and
`buttonsReleased()` / `buttons_released()` consume accumulated edge masks. A
short press is retained between application polls; the snapshot also reports
the buttons currently held.

The connection lasts until it is closed. `TaskOptions.taskTimeoutMs` /
`task_timeout_ms` is a sequential-task lifetime limit and does not limit this
explicitly long-lived connection. Each motor lease is still finite and capped by
the configured maximum motion duration. Expiry disarms; a later drive packet cannot
rearm the robot. Valid refreshes replace a lease without inserting artificial
stop/start gaps. A zero-duty command clears the drive lease while retaining the
explicitly requested armed state.

Network applications must retain their own authentication, controller ownership
and command ordering. The demonstrators pass original receipt-time deadlines and
validate queued operations against current ownership and stop generation. A
heartbeat or unrelated request never extends a drive command. Session methods
that accept a permission check run it immediately before native execution, then
check the session's stop generation before acting.

Session recovery is intentionally explicit. It does not change `KeelTask`'s
terminal fault/cancellation contract: catching an error in a student task still
does not restore its hardware authority.
