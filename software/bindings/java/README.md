<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Keel Java binding

`io.github.bisocmorg:keel` is the Java binding of libkeel, the host library of the Keel robot baseboard. It is a
pure-Java jar over the Foreign Function and Memory API (`java.lang.foreign`): no JNI, no native code and no runtime
dependencies. It loads the system's `libkeel.so.1`, so a robot has exactly one copy of the safety code. The package
`io.github.bisocmorg.keel.camera` drives the robot's Raspberry Pi camera through its own library,
`libkeel-camera.so.1` ([Camera](#camera)).

- Java 25 or later (the long-term-support release with the final FFM API), on Linux.
- libkeel 1.x with this binding's minor or newer. The binding and libkeel share one version: binding 1.0.0 needs
  ABI 1.0, and loading refuses an older minor or another major, naming both versions.
- JPMS module `io.github.bisocmorg.keel`, exporting `io.github.bisocmorg.keel` and its API packages (see
  [Packages](#packages)).

The safety rules it implements are those of libkeel: [`keel.h`](../../libkeel/include/keel/keel.h) is the contract,
[`docs/safety.md`](../../libkeel/docs/safety.md) explains the safety design and
[`docs/bindings.md`](../../libkeel/docs/bindings.md) the binding rules.

## Install

The binding is published with each libkeel release on GitHub (`keel-X.Y.Z.jar`, with its sources jar, javadoc jar
and pom). To use it as a Maven dependency, install the release files into your local repository:

```sh
mvn install:install-file -Dfile=keel-1.0.0.jar -DpomFile=keel-1.0.0.pom \
    -Dsources=keel-1.0.0-sources.jar -Djavadoc=keel-1.0.0-javadoc.jar
```

```xml
<dependency>
  <groupId>io.github.bisocmorg</groupId>
  <artifactId>keel</artifactId>
  <version>1.0.0</version>
</dependency>
```

On the robot, the library comes from the `libkeel1` package (`install.sh` installs it), and Java from
`sudo apt install openjdk-25-jre-headless`. There is no Debian package of the jar: Java applications ship their
dependencies, and the jar holds no safety code of its own. The camera package needs `libkeel-camera1` as well, which
`install.sh` installs; nothing else in the jar does.

On a desktop, build a simulator-only libkeel and point the binding at it:

```sh
cmake --preset sim-only && cmake --build --preset sim-only --target keel     # in software/libkeel
export KEEL_LIBRARY="$PWD/build/sim-only/libkeel.so.1" KEEL_BACKEND=simulator
```

The library is found through `KEEL_LIBRARY` (an absolute path; an empty value counts as unset), otherwise through
the dynamic loader by its SONAME `libkeel.so.1`. It is loaded on first use, or explicitly with `Keel.load()`.

## Native access

The FFM methods the binding needs are restricted, so the JVM must grant the binding native access:

```sh
# Class path (the jar and the application in the unnamed module):
java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar:app.jar com.example.App
# Module path:
java --enable-native-access=io.github.bisocmorg.keel -p keel-1.0.0.jar:app.jar -m com.example.app/com.example.App
```

An executable application jar can instead carry `Enable-Native-Access: ALL-UNNAMED` in its own manifest. The binding
jar does not carry that attribute: the JVM ignores it in a library jar on the class path. Without the grant the JVM
warns today; with `--illegal-native-access=deny` (the future default) loading fails with a `LibraryException` that
names the option to add.

## Quick start

```java
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.state.State;

public final class QuickStart {
    public static void main(String[] args) {
        try (Device device = Keel.open()) {                // the board, or KEEL_BACKEND=simulator
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.snapshot().state() == State.ARMED) {
                    cycle.drive(200, 200);                 // permille, sent again every cycle
                }
            }).build();
            device.requestArm();                           // before the first tick every thread is the owner
            loop.run(300);                                 // 3 s on the device clock, on this platform thread
            device.disarm();
        }
    }
}
```

`Keel.open()` opens the backend that `KEEL_BACKEND` names, otherwise the robot's board; it never falls back to the
simulator silently. For tests and desktops, open the simulator explicitly:

```java
Device device = Keel.open(OpenOptions.builder().backend(Backend.SIMULATOR).flags(OpenFlag.VIRTUAL_CLOCK).build());
```

The [examples](examples) are single-file programs for the source launcher. Each accepts `--simulator`, and arms only on
the simulator or with `--allow-motion` (the `--drive LEFT RIGHT MS` option of `Buttons` is itself that intent, and
without it `Buttons` never moves). They exit with 0 on success, 1 on failure, 2 for a usage error and 128 + N after
signal N (130 after Ctrl-C, 143 after SIGTERM):

```sh
java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/Buttons.java --simulator
```

| Example | Shows |
|---|---|
| `Buttons` | button LEDs mirror the buttons, edges printed, B ends, Ctrl-C is an emergency stop, `--drive LEFT RIGHT MS` |
| `Drive` | open, arm, drive 200 permille forward for about 2 s, disarm, close |
| `Leds` | RGB frames and button LEDs; an error colour while FAULTED; a missing LED driver handled |
| `Ranging` | `rangeStart` and `rangePoll` in the step; statuses, never sentinel distances; `rangeMeasure` from another thread; `--count N` |
| `Safety` | an emergency stop from a user-interface thread, the fault record, clearing through the loop, the re-arm hold-off, a refused foreign go call |
| `CameraFrames` | camera frames into vision code: brightness and motion per frame from the Y plane, with no copy (no board needed) |
| `CameraStream` | the camera to the network or a file, the receivers reported once a second; `tcp://:5000?listen` by default |
| `CameraStill` | one JPEG photo, by default at the sensor's full resolution |

## Packages

| Package | Contents |
|---|---|
| `io.github.bisocmorg.keel` | `Keel` (loading, library metadata, `open`), `Device`, `OpenOptions`, `OpenReport` with `OpenFlag`, `OpenWarning` and `OpenResource`, `Backend`, `Capability`, `TimingClass`, `Abi`, `AbiVersion`, `Limits` |
| `io.github.bisocmorg.keel.errors` | `KeelException` and its subclasses, `LibraryException`, `Status`, `StatusClass` |
| `io.github.bisocmorg.keel.loop` | `ControlLoop`, `Cycle`, `LoopStats`, `CycleInput`, `CycleCommand`, `CycleFlag`, `Health` |
| `io.github.bisocmorg.keel.state` | `State`, `Snapshot`, `SnapshotFlag`, `SafetyInput`, `AckPhase`, `Fault`, `FaultCause`, `FaultFlag` |
| `io.github.bisocmorg.keel.peripherals` | `Button`, `ButtonState`, `Led`, `RangeMeasurement`, `RangeStatus`, `SensorPower` |
| `io.github.bisocmorg.keel.sim` | `SimItem`, `SimFlag`, `SimState` |
| `io.github.bisocmorg.keel.camera` | `Camera`, `CameraOptions`, `ListedCamera`, `CameraInfo`, `SensorMode`, `CameraStats`, `CaptureState`, the focus, white-balance and flip options, `CameraAbi`, `CameraLimits` |
| `io.github.bisocmorg.keel.camera.frames` | `Frame`, `FrameInfo`, `PixelFormat`, `StreamInfo` |
| `io.github.bisocmorg.keel.camera.encoding` | `EncoderOptions`, `EncodedChunk`, `ChunkInfo`, `Codec`, `H264Profile` and their flags |
| `io.github.bisocmorg.keel.camera.outputs` | `OutputOptions`, `OutputStats`, `OutputState` |
| `io.github.bisocmorg.keel.camera.stills` | `StillOptions`, `StillImage`, `StillInfo`, `StillFlag` |
| `io.github.bisocmorg.keel.camera.errors` | `CameraException` and its subclasses, `CameraLibraryException`, `ErrorReport`, `CameraResource` |

`Abi` holds the constants of the `keel.h` the binding was generated from; `AbiVersion` is the version
`Keel.abiVersion()` reads from the loaded library. The packages `io.github.bisocmorg.keel.internal` and
`io.github.bisocmorg.keel.internal.camera` hold the loaders, the downcalls and the struct conversion; the module does
not export them, and they are not API.

## The loop and the safety contract

libkeel's heartbeat keeps motor permission alive only while the application's own control loop calls `keel_cycle`
every 10 ms with honest health. `ControlLoop` is that loop:

- one `keel_cycle` per period on a dedicated platform thread, on the device clock (`keel_sleep_until`), so the same
  code runs on the simulator's virtual clock;
- each cycle runs the requests queued from other threads (`requestArm()`, `clearFault()`, `submit(...)`), then your
  step, then sends the health and the step's motor command;
- health is `Health.ALL` only if that work finished without an exception, within the step budget (40 ms by default)
  and with your I/O healthy (`cycle.reportIoFailure()` otherwise). Anything less fail-stops on purpose: a latched
  Category-0 stop whose fault record names the missing bits;
- a duty outside -1000..1000 from `cycle.drive()` fails the cycle the same way, and `setCommand()` refuses it on the
  posting thread and coasts;
- the loop never arms, never clears a fault and never re-sends a command for you. A drive command lives for the
  command timeout (500 ms by default): send it every cycle while it should stay in force;
- in FAULTED the loop keeps running without motion, so your step can show an error colour and read buttons;
  `loop.clearFault()` clears from the owner thread, and `requestArm()` stays refused for 1 s after the latch.

`start()` runs the loop on a new non-daemon platform thread named `keel-loop`; `run(cycles)` and
`runUntil(done, maxCycles)` run it on the calling platform thread. `join()` and `close()` wait for the loop however
it runs. One loop serves a device for its whole life. `onFault` runs once for every fault latch, including a fault
that latches again in the cycle that cleared the previous one.

The loop's fatal error (a `KeelException`, an `Error` that escaped the step, or an exception thrown by the `runUntil`
condition) is rethrown as it is by `run`, `runUntil` and `join`. `close()` rethrows it only if none of them has, so a
failing loop in try-with-resources reports its own exception once. Interrupting the thread that runs the loop is a
stop request: the loop disarms and ends, and the thread's interrupt status stays set.

### Threads: stop from anywhere, go only from the loop

The control owner is the OS thread of the first successful tick after open or `clearFault()`. Go calls
(`requestArm`, `clearFault`, non-zero `setMotors`, `brake`, `servoStart`, `servoSetPulse`) from any other thread throw
`ConflictException`; post them to the loop instead. Stop calls (`disarm`, `emergencyStop`, `coast`, `servoEnd`,
`close`, and `ControlLoop.stop()`) are accepted from every thread in every state and take no binding lock: motor
enable falls before libkeel waits for any lock.

### Virtual threads

A virtual thread that calls a foreign function pins its carrier, and its carrier can change between calls, which
libkeel would see as a tick from another thread. So:

- `Device.tick`, `Device.cycle`, `Device.sleepUntil` and `ControlLoop.run`/`runUntil` throw `WrongThreadException`
  on a virtual thread, before any call. `ControlLoop.start()` is allowed: it creates the loop's own platform thread.
- Every other call is allowed on a virtual thread and pins its carrier for its documented time: open and close
  about 100 ms, `rangeMeasure` at most 35 ms, an LED call at most 20 ms per I2C transaction on a hung bus, everything
  else under 2 ms.
- A go call from a virtual thread runs on a carrier, never the loop's thread, so it gets `ConflictException` once an
  owner is recorded.

### The shutdown hook and Ctrl-C

The binding registers one JVM shutdown hook at the first open. On SIGINT, SIGTERM, SIGHUP or `System.exit`, it calls
`emergencyStop()` and then `close()` on every device still open, so every exit but SIGKILL ends in a Category-0
stop. After SIGKILL the safety MCU removes motor power when the heartbeat window closes (about 86 ms after the last
heartbeat). The binding installs no signal handlers of its own. A device that is dropped without `close()` is closed
by a cleaner, with a warning; close devices with try-with-resources.

## Exceptions

Every non-OK status throws one unchecked type per status class, all in `io.github.bisocmorg.keel.errors`. The
class tells you what state the device is in after the call; catch the class-level type rather than branching on
codes.

| Type | Thrown for | Device after the call |
|---|---|---|
| `RejectedException` | class REJECTED; refinements `InvalidArgumentException`, `StateException`, `AgainException`, `UnsupportedException`, `ConflictException` | unchanged, except that a refused motion request while ARMED coasts |
| `SafetyFaultException` | class FAULT, undefined codes, and any class this binding does not know; carries `fault()` | latched FAULTED; Category-0 stop done; heartbeat stopped |
| `PeripheralException` | class PERIPHERAL (LED driver, ultrasonic sensor) | safety state unchanged |
| `SetupException` | every failed `Keel.open`; carries `report()` with the remedy | no device |
| `HandleException` | class HANDLE, and any call on a device the binding has closed or is closing | unusable by this caller |
| `LibraryException` | not Linux, no library, a missing symbol, an incompatible ABI, native access denied | - |

`KeelException` is `abstract sealed`, so a pattern `switch` over the five class-level types needs no `default`.
`close()` never throws: its status is diagnostic and stays in `closeStatus()` and `finalFault()`.

## Camera

`io.github.bisocmorg.keel.camera` drives a Camera Module 2 or Camera Module 3 on the Raspberry Pi's camera connector
through `libkeel-camera.so.1`, the camera library of libkeel ([camera.md](../../libkeel/docs/camera.md)). It needs
neither `libkeel.so` nor a `Device`, and a program that uses only the camera never loads the board library. The
camera library talks to libcamera and the Pi's hardware encoders directly; it starts no helper programs.

- **Install.** `install.sh` installs `libkeel-camera1`. The user must be in the `video` group:
  `sudo keel-setup --user NAME`, then log in again. Native access is granted as for libkeel.
- **Loading.** The camera library is loaded on the first camera call, by its SONAME or from the absolute path in
  `KEEL_CAMERA_LIBRARY`, and independently of `libkeel.so.1`. A missing library throws `CameraLibraryException` (a
  `LibraryException`) naming the package.

```java
import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import io.github.bisocmorg.keel.camera.stills.StillOptions;
import java.awt.image.BufferedImage;
import java.nio.file.Path;

public final class ReadmeCamera {
    public static void main(String[] args) {
        CameraOptions options = CameraOptions.builder().width(640).height(480).pixelFormat(PixelFormat.BGR888)
                .build();
        try (Camera camera = Camera.open(options)) {
            camera.setEncoder(Codec.H264);
            camera.startOutput(Path.of("drive.h264"));      // the library records from its own threads
            camera.start();
            int count = 0;
            for (Frame frame : camera.frames()) {
                BufferedImage image = frame.toBufferedImage(); // or frame.data(): the pixels, with no copy
                if (++count == 30) {                           // about one second
                    break;
                }
            }
            camera.stop();
            camera.captureStillFile(Path.of("photo.jpg"), StillOptions.of(1920, 1080));
        }
    }
}
```

- **Frames.** `readFrame()` and `frames()` give `Frame` objects: the layout, the sensor's sequence number, the start
  of exposure on `CLOCK_MONOTONIC` (the robot's control-loop clock) and the pixels in a direct buffer of the frame's
  own, read with `data()` (a `ByteBuffer`) or `segment()` (a `MemorySegment`) with no copy. Open the camera with
  `PixelFormat.I420` (the default), `RGB888` or `BGR888`; the Pi's image signal processor converts at no CPU cost.
  Rows may be padded: use `info().stride0()`, or `toByteArray()` for the rows without padding. `toBufferedImage()`
  gives a `TYPE_3BYTE_BGR` image; on the module path it needs `java.desktop`, which the module requires statically
  (add `--add-modules java.desktop` or `requires java.desktop`). The queue keeps the newest frame by default, so a
  slow model never slows the camera; `stats()` counts the frames it dropped.
- **Encoded video.** `setEncoder(Codec.H264)` or `Codec.MJPEG` (or `EncoderOptions`) before `start()`. `readChunk()`
  and `chunks()` give one H.264 access unit or one JPEG image at a time. `startOutput()` makes the library write the
  stream itself: a `.h264` or `.mjpeg` file, `tcp://HOST:PORT`, `tcp://:PORT?listen` (up to four viewers, each from
  its next keyframe) or `udp://HOST:PORT`; view a listening robot with
  `ffplay -f h264 -fflags nobuffer -flags low_delay -framedrop tcp://ROBOT:5000`. Container files and streaming
  protocols need a libkeel-camera built with FFmpeg's libavformat; the release packages are not, and throw
  `CameraUnsupportedException`.
- **Stills.** `captureStill()` and `captureStillFile()` take one JPEG while capture is stopped, by default at the
  sensor's full resolution.
- **Camera Module 3.** `CameraOptions.afMode`, `afRange` and `lensPosition` control the autofocus;
  `triggerAutofocus()` runs one scan. On Camera Module 2 they throw `CameraUnsupportedException`.
- **Threads.** The platform thread that opens a camera owns it: every method but `close()` must be called from that
  thread, or it throws `CameraThreadException` with no effect. A virtual thread cannot open a camera. `close()`
  works from any thread and wakes a read blocked in the owner, whose `frames()` loop then ends. A camera that is never
  closed is closed by a cleaner, with a warning, and every camera still open at JVM exit by a shutdown hook, so files
  are finished.
- **Exceptions.** Every failure throws a subclass of the sealed `CameraException`, chosen by the status
  (`io.github.bisocmorg.keel.camera.errors`), with the error record libkeel-camera wrote in `report()`; the message
  names the remedy.

## Garbage collection

The loop allocates one snapshot record per cycle. Plan for the 40 ms stall budget, and use a low-pause collector on
the robot: `-XX:+UseZGC`, or G1 with `-XX:MaxGCPauseMillis=20`.

## Troubleshooting

- `LibraryException: libkeel requires Linux`: the binding runs only where libkeel does.
- `LibraryException: cannot open libkeel.so.1`: install `libkeel1`, or set `KEEL_LIBRARY` to the absolute path of a
  libkeel build.
- `LibraryException: ... has ABI 1.0; this binding needs 1.1`: the installed library is older than the binding; use
  the binding release whose X.Y matches `dpkg-query -W libkeel1`, or upgrade the library.
- `LibraryException: native access is denied`: add the `--enable-native-access` option shown above.
- `SetupException`: its message ends with the remedy from the open report, for example a `keel-setup` command or
  `dtparam=spi=off`.
- `ConflictException` from a go call: the call came from a thread other than the control owner; post it to the loop.
- `WrongThreadException`: the loop or a tick ran on a virtual thread; use `ControlLoop.start()` or a platform thread.
- `CameraLibraryException: cannot load libkeel-camera.so.1`: install `libkeel-camera1`, or set `KEEL_CAMERA_LIBRARY`
  to the absolute path of a libkeel-camera build. `keel-camera list` checks the camera and its permissions.

## Building and testing

The build needs JDK 25 or later and Maven 3.9 or later (the enforcer plugin checks both; Ubuntu 24.04's packaged
Maven 3.8.7 is too old, so use a Maven release from the Apache archive there).

```sh
cd software/bindings/java
export KEEL_LIBRARY=/path/to/software/libkeel/build/sim-only/libkeel.so.1
export KEEL_CAMERA_LIBRARY=/path/to/software/libkeel/build/sim-only/libkeel-camera.so.1
mvn -B verify
```

Both libraries come from the simulator-only build of libkeel (`cmake --preset sim-only`, then
`cmake --build --preset sim-only --target keel keel_camera`, with `libcamera-dev` installed). The camera tests run on
its synthetic camera, which `KEEL_CAMERA_TEST_SOURCE=1` selects in test builds only; they never load `libkeel.so`.

`mvn verify` compiles with `-Xlint:all -Werror`, runs the unit tests against the simulator on the class path with
`--illegal-native-access=deny`, runs the examples, checks the jar on the module path, checks Javadoc with doclint and
enforces the JaCoCo coverage gate. Every test opens the simulator explicitly, and a suite-wide guard fails any test
that opens a device that is not simulated. The robot stage runs only the hardware tests, which never move the robot:
`KEEL_TEST_HARDWARE=1 mvn -B -Phardware test`; the camera's hardware tests use the connected camera and skip when
there is none.

The enums, constants, data records, struct layouts and function descriptors are generated from `keel.h` and
`camera.h` by `software/bindings/tools/keelgen.py`; do not edit those files (they say so in their first lines). After a header
change, run `python3 software/bindings/tools/keelgen.py --lang java` and commit the result; CI fails while they
differ.

The API reference is built with `mvn javadoc:javadoc` into `target/reports/apidocs`.

## Licence

MIT; see [LICENSE](../../../LICENSE). The jar carries the licence text as `META-INF/LICENSE`.
