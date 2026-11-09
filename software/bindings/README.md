<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Keel bindings

The production language bindings of libkeel, the host library of the Keel robot baseboard:

| Binding | Package | Import | Documentation |
|---|---|---|---|
| Python | `keel-robot` (PyPI name), `python3-keel` (Debian) | `import keel` | [python/README.md](python/README.md) |
| Java | `io.github.bisocmorg:keel` | module `io.github.bisocmorg.keel`: package `io.github.bisocmorg.keel` and its subpackages `errors`, `loop`, `state`, `peripherals`, `sim`, `camera` | [java/README.md](java/README.md) |

Both are pure code over the system's `libkeel.so.1`: Python through ctypes, Java through the Foreign Function and
Memory API. Neither contains native code or bundles the library, so a robot has exactly one copy of the safety code
and `apt upgrade` updates every language at once. libkeel is the only safety logic: the bindings pass every call
through, map every status to the exception of its class and add a control-loop helper that implements the loop
contract of [docs/bindings.md](../libkeel/docs/bindings.md).

Both also bind the camera library `libkeel-camera.so.1` (`camera.h`), as `keel.camera` and
`io.github.bisocmorg.keel.camera`: Camera Module 2 and 3, raw frames (numpy arrays in Python, direct buffers and
`BufferedImage` in Java), the hardware H.264 and MJPEG encoders, files and network streams the library writes
itself, and stills. The camera library is loaded separately, on first camera use, and needs neither `libkeel.so` nor
a device; a robot needs the `libkeel-camera1` package for it ([camera.md](../libkeel/docs/camera.md)).

## Compatibility

For complete browser-controlled examples in both languages, see the
[Java and Python demonstrator](../demonstrator/README.md). Each server uses its own
binding and serves an interactive 3D robot, camera feed, photos and timing statistics.

- Linux only: the robot (Raspberry Pi 4, arm64) and any Linux PC with a simulator build of libkeel. On other
  platforms the bindings import, and loading the library fails with a clear error.
- Python 3.11 or later (3.11 to 3.14, and the free-threaded 3.14t); Java 25 or later.
- libkeel 1.x with the binding's minor or newer. The bindings and libkeel are released together and share one
  version; a binding refuses, at load time and naming both versions, a library with another major or an older minor.

## Installing

| Where | Python | Java |
|---|---|---|
| Robot, from the release packages | `install.sh --python` (or `sudo apt install ./python3-keel_X.Y.Z_all.deb`) | `sudo apt install openjdk-25-jre-headless`, then the release jar on the class path |
| A venv on the robot or a desktop | `pip install "keel-robot==X.Y.*"`, pinned to the installed `libkeel1` | - |
| A Maven or Gradle build | - | `io.github.bisocmorg:keel:X.Y.Z` (release jar and pom, installed with `mvn install:install-file`) |
| A desktop with a simulator build | `KEEL_LIBRARY=/path/to/libkeel.so.1` | `KEEL_LIBRARY=/path/to/libkeel.so.1` |
| The camera, from a test build | `KEEL_CAMERA_LIBRARY=/path/to/libkeel-camera.so.1` | `KEEL_CAMERA_LIBRARY=/path/to/libkeel-camera.so.1` |

On the robot, `install.sh` installs libkeel from the release ([deploy.md](../libkeel/docs/deploy.md)):

```sh
curl -fsSL https://github.com/BisocMOrg/Keel/releases/latest/download/install.sh | sudo sh -s -- --user "$USER" --python
```

On a desktop, build the simulator-only library once and point the bindings at it:

```sh
cmake --preset sim-only && cmake --build --preset sim-only --target keel keel_camera   # in software/libkeel
export KEEL_LIBRARY="$PWD/build/sim-only/libkeel.so.1"
export KEEL_CAMERA_LIBRARY="$PWD/build/sim-only/libkeel-camera.so.1"   # needs libcamera-dev for the build
```

## Layout

| Path | Contents |
|---|---|
| [CHANGELOG.md](CHANGELOG.md) | what changed in each release |
| `tools/keelgen.py`, `tools/keelgen_java.py` | the generator of everything that restates `keel.h` and `camera.h` (the Java emitter in its own file), and the drift check CI runs |
| `python/` | the Python binding: package, tests, examples and the `python3-keel` Debian packaging |
| `java/` | the Java binding: Maven project, tests and examples |

Everything derived from `keel.h` and `camera.h` (enumerations, struct layouts, prototypes, constants) is generated
and checked in.
After a change to the header, regenerate and commit the output; CI fails while it differs from a fresh run:

```sh
python3 software/bindings/tools/keelgen.py            # needs pycparser and a C compiler for the preprocessor
python3 software/bindings/tools/keelgen.py --check    # what CI runs
```

## Further reading

- [docs/safety.md](../libkeel/docs/safety.md): the safety design every binding wraps: states, the loop contract,
  stops, statuses and the fault catalogue.
- [docs/bindings.md](../libkeel/docs/bindings.md): the rules every binding follows.
- `include/keel/keel.h`: the contract; `include/keel/camera.h` for the camera.
- [docs/camera.md](../libkeel/docs/camera.md): the camera guide, with its Python and Java sections.
- `tests/bindings/scenarios.json`: the language-neutral conformance scenarios both bindings replay.

## Licence

MIT. Copyright (c) 2026 BisocM.
