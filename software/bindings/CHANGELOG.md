<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Changelog

All notable changes to the Keel bindings. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The bindings are released together with libkeel from one
`libkeel-vX.Y.Z` tag and share its version; a binding X.Y needs libkeel ABI X.Y (the same major and a minor of at
least Y). A fix that touches only a binding ships as a libkeel patch release.

## [Unreleased]

- Migrate the Java and Python board examples and both browser demonstrators to the task layer. Add
  `RobotSession` for interactive applications with explicit arming/recovery, finite renewable motor leases,
  original command deadlines, and managed peripheral/status access. Camera-only examples remain independent
  of the board library and use the existing high-level camera API.
- Add peripheral methods to sequential tasks and run the demonstrators' protocol/safety tests in CI.
  Deployment bundles include matching bindings and samples; Python selects the bundled binding first.
- Add matching Java and Python sequential task APIs: a `KeelTask` subclass runs ordinary student code with
  automatic arming for bounded motor actions, stationary pauses and lazy BGR888 camera images.
- Add signed-percent `driveFor`/`drive_for`, timed left/right turns and terminal task cancellation. Action
  and total-task deadlines are enforced independently of the student code; faults are never cleared automatically.
- Document task execution, units, ownership and failure behaviour in the shared guide, language READMEs,
  Java Javadoc and typed Python docstrings. Add guarded simulator/hardware examples.
- Keep the existing device, control-loop and native ABI contracts unchanged; there are no built-in
  target-tracking or OpenCV behaviours.

## [1.0.0] - 2026-11-07

The first release of the Python and Java bindings, for libkeel 1.0 (ABI 1.0, SONAME `libkeel.so.1`).

### Python (`keel-robot`, `import keel`)

- All 37 functions of `keel.h` through `keel.open()`, `keel.Device` and the module functions, over ctypes; no
  dependencies and no native code.
- The types grouped in the packages `keel.errors`, `keel.loop`, `keel.state`, `keel.peripherals` and `keel.sim`,
  every public name re-exported by `keel`.
- Every enumeration as an open `IntEnum` (an unknown value becomes an `UNRECOGNIZED_<n>` pseudo-member) or an
  `IntFlag` that keeps unknown bits; the limits and ABI constants in `keel.limits` and `keel.abi`; frozen data
  classes for every output struct.
- One exception per status class, with REJECTED subclasses; `InvalidArgumentError` is also a `ValueError`.
- Integers that ctypes would truncate are refused before the call (`TypeError`, `OverflowError`); out-of-range
  motor duties are saturated so that libkeel refuses them and coasts.
- `keel.ControlLoop`: one `keel_cycle` per period on one thread, on the device clock, with honest health, a step
  budget, one-shot intents, go requests through futures, faulted mode that reports every fault latch once,
  statistics and duty checks that turn an out-of-range duty into a failed cycle; four foreign calls per cycle.
- `close()` that returns only after `keel_close` and completes even when Ctrl-C arrives during it, a finalizer
  that closes a dropped device, `stop_on_signals()` for SIGINT, SIGTERM, SIGHUP and SIGQUIT, Ctrl-C as an
  emergency stop, and safe behaviour in a forked child.
- Type information (`py.typed`), five examples, a wheel and an sdist, and the Debian package `python3-keel`.
- `keel.camera`: all 25 functions of `camera.h` (libkeel-camera 1.0) over ctypes, loaded separately and only on
  first camera use (`KEEL_CAMERA_LIBRARY`, else `libkeel-camera.so.1`), never loading `libkeel.so`. `open()` and
  `Camera` with context managers, `frames()` and `chunks()` iterators, `Frame.array()` as a numpy view with no copy
  (numpy optional, the extra `keel-robot[numpy]`), outputs, stills and autofocus; the packages
  `keel.camera.frames`, `encoding`, `outputs`, `stills` and `errors`; one exception per status that needs its own
  handling, with the error record; a finalizer and interpreter-exit close. Three camera examples; `python3-keel`
  recommends `libkeel-camera1`.

### Java (`io.github.bisocmorg:keel`)

- All 37 functions of `keel.h` through `Keel` and `Device`, over the Foreign Function and Memory API; Java 25 or
  later, no JNI, no native code and no runtime dependencies.
- Every enumeration as a Java enum with an `UNRECOGNIZED` constant, raw `...Value` record components and raw mask
  methods, so no value from a newer library is lost; records for every output struct and `OpenOptions` with a
  builder.
- An `abstract sealed` exception hierarchy with one type per status class, so a pattern `switch` over the classes
  is exhaustive.
- `ControlLoop` on a dedicated platform thread with the same model as the Python loop; tick, cycle, sleep and `run`
  refuse virtual threads. `join()` and `close()` wait for the loop however it runs, and the loop's fatal error
  reaches try-with-resources once and as it is.
- `close()` that never throws and returns only after `keel_close`, a `Cleaner` and a shutdown hook that stop and
  close every device still open, and confined arenas only.
- The JPMS module `io.github.bisocmorg.keel`, which exports the root package and its subpackages `errors`, `loop`,
  `state`, `peripherals` and `sim`; five examples; and the jar with its sources, javadoc and pom.
- `io.github.bisocmorg.keel.camera` and its subpackages `frames`, `encoding`, `outputs`, `stills` and `errors`: all
  25 functions of `camera.h` over the FFM API, loaded separately on first camera use, never loading `libkeel.so`.
  `Camera` (AutoCloseable) with option records and builders, `frames()` and `chunks()` iterables, `Frame` backed by a
  direct buffer with `ByteBuffer`, `MemorySegment` and `BufferedImage` views (`requires static transitive
  java.desktop`), a sealed `CameraException` hierarchy, a cleaner and a shutdown hook; three camera examples.
