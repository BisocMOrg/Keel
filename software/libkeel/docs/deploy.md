<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Deploying libkeel

How libkeel gets onto a robot: which builds exist and why one is enough, what the packages install, how to
install them, how to give an application device access and real-time priority, and how to build the packages
yourself.

## 1. One build per target, and why one is enough

A compiled library is tied to three things: the CPU architecture, the operating system and the C library. The C
source of libkeel is portable; a compiled `libkeel.so.1` is not. So the release builds one binary per target and
nobody compiles on the robot.

For the robot there is exactly one target: **aarch64 Linux with glibc**. That single build runs on every
Raspberry Pi 4, Pi 400 and Compute Module 4 with a 64-bit operating system: Ubuntu 22.04 or later (the robot runs
Ubuntu 26.10), Raspberry Pi OS bookworm or later (64-bit) and Debian 12 or later. It works on all of them
because the library depends on nothing else:

- the kernel interfaces it uses are stable: the GPIO character device v2 (Linux 5.10 or later) and `i2c-dev`;
- glibc is backwards compatible, and the library needs no glibc symbol newer than version 2.34. The package check
  (`tests/packaging/check_debs.sh`) fails the build if that ever changes;
- it needs no other library: threads are part of glibc since 2.34.

What a build does **not** cover: 32-bit operating systems (armhf; not built for 1.0), the Raspberry Pi 5 (a
different GPIO chip, so `keel_open` reports `KEEL_E_NOT_FOUND`) and musl-based systems such as Alpine (they would
need their own build).

The camera library is the exception to "one build is enough". `libkeel-camera.so.1` links libcamera, which has no
stable ABI: each libcamera minor version has its own SONAME and package (`libcamera0.7` on Ubuntu 26.10). The
release's camera packages (`libkeel-camera1`, `libkeel-camera-dev` and `keel-camera`) are therefore built for the
robot's operating system, Ubuntu 26.10, and install only where its libcamera is available. The board packages
(`libkeel1`, `libkeel-dev`, `keel-common`, `keel-tools`) do not depend on the camera packages or on libcamera, and
`install.sh` leaves the camera packages out where their libcamera is missing. On another distribution, build the
packages there (section 7) or build from source (section 8); the camera library builds against libcamera 0.2 or
later and is skipped when libcamera is absent. Besides libcamera it needs only the kernel's V4L2 devices and, for
stills larger than 1920x1920, libjpeg; [camera.md](camera.md) section 1 lists what a minimal image must contain.

Other builds exist for other jobs:

- **x86-64 Linux** builds are for development, continuous integration and the simulator. The board backend is
  compiled in, but it only finds a board on a Pi.
- **Bindings** (Python, Java) never compile C. On the robot they load the system library by its SONAME,
  `libkeel.so.1`; for development they load the file named by `KEEL_LIBRARY` (an absolute path). See
  [bindings.md](bindings.md).
- **Static linking:** a C program that should be one self-contained file links `libkeel.a` (`Keel::keel_static`,
  or `pkg-config --cflags --libs keel-static`, which names the archive by path; `pkg-config --static keel` would
  still link `libkeel.so`). Programs that use `port.h` must link this way, because the shared library does not
  export the port API. It still needs glibc and the kernel interfaces, and it still needs the system setup below.

## 2. The packages

| Package | Architecture | Contents |
|---|---|---|
| `libkeel1` | arm64 | `libkeel.so.1` and `libkeel.so.1.0.0`; depends on `keel-common` of the same version |
| `libkeel-dev` | arm64 | `keel/keel.h`, `keel/port.h`, `libkeel.so`, `libkeel.a`, the CMake package `Keel` (`Keel::keel`, `Keel::keel_static`), `keel.pc`, `keel-static.pc` |
| `keel-common` | all | `/usr/lib/udev/rules.d/99-keel.rules`, `/usr/lib/sysusers.d/keel.conf` (the `keel` group), `/usr/lib/modules-load.d/keel.conf` (`i2c-dev`), `/etc/security/limits.d/90-keel-rtprio.conf` (`@keel - rtprio 40`), the example unit `keel-app.service` |
| `keel-tools` | arm64 | `keel-setup`, `keel-doctor`, `keel-live` and `keel-demo` |
| `libkeel-camera1` | arm64 | `libkeel-camera.so.1` and `libkeel-camera.so.1.0.0`, the camera library ([camera.md](camera.md)); depends on `libcamera0.7` and `libcamera-ipa` |
| `libkeel-camera-dev` | arm64 | `keel/camera.h`, `libkeel-camera.so`, the CMake target `Keel::keel_camera` (`find_package(Keel COMPONENTS camera)`), `keel-camera.pc`; depends on `libkeel-dev` |
| `keel-camera` | arm64 | the `keel-camera` tool |
| `python3-keel` | all | the Python binding (`import keel`, with the camera in `keel.camera`) for the system `python3`, with its examples in `/usr/share/doc/python3-keel/examples`; depends on `libkeel1` of the same major version and at least the same minor, recommends `libkeel-camera1` (needed only for the camera) and suggests `python3-numpy` (`software/bindings/python`) |

When `keel-common` is installed, its postinst creates the `keel` group (`systemd-sysusers`), loads `i2c-dev` so
`/dev/i2c-1` exists at once, and asks udev to re-apply the rule to the existing GPIO chip and I2C nodes. It never
edits the boot configuration, never changes the I2C bus speed and never adds a user to a group: `keel-setup` does
those, and only when asked.

## 3. Installing on the robot

### One command

```sh
curl -fsSL https://github.com/BisocMOrg/Keel/releases/latest/download/install.sh | sudo sh -s -- --user "$USER"
sudo reboot
```

`install.sh` does the following, and stops at the first failure:

1. downloads the release's `SHA256SUMS` and picks the arm64 packages listed in it;
2. downloads each package and verifies it against `SHA256SUMS`;
3. installs the board packages `libkeel1`, `keel-common` and `keel-tools` and the camera packages
   `libkeel-camera1` and `keel-camera` with `apt-get` (with `--dev`, also `libkeel-dev` and `libkeel-camera-dev`;
   with `--python`, also `python3-keel`); apt adds libcamera and its IPA module. When this system cannot provide
   a dependency of `libkeel-camera1` (another distribution's libcamera), it leaves the camera packages out and says
   so; `--no-camera` always leaves them out, `--camera` installs them regardless;
4. runs `keel-setup --user NAME` (section 5);
5. prints the report of `keel-doctor --user NAME` when the release includes it: device access, groups and the
   real-time limit of that account as they will be after it logs in again (the script itself runs as root).

The whole script runs from a `main` function on its last line, so a download cut short runs nothing, and no step
reads standard input, which under `curl | sh` is the script itself. On an upgrade, a configuration file you edited
(such as the rtprio limit) is kept rather than asked about.

Options: `--user NAME` (default: the user who ran `sudo`), `--dev`, `--python`, `--no-camera`, `--camera`,
`--version X.Y.Z` (install that release instead of the latest), `--i2c-fast` (passed to `keel-setup`), `--dry-run`
(download and verify, then print what would run). Log in again afterwards so the new group memberships apply, and
reboot so the boot configuration does.

### Two steps by hand

For anyone who prefers not to pipe a script into `sudo sh`:

```sh
VERSION=1.0.0
BASE=https://github.com/BisocMOrg/Keel/releases/download/libkeel-v$VERSION
for f in SHA256SUMS libkeel1_${VERSION}_arm64.deb keel-common_${VERSION}_all.deb \
         keel-tools_${VERSION}_arm64.deb libkeel-camera1_${VERSION}_arm64.deb keel-camera_${VERSION}_arm64.deb; do
    curl -fsSLO "$BASE/$f"
done
sha256sum --check --ignore-missing SHA256SUMS
sudo apt-get install ./libkeel1_${VERSION}_arm64.deb ./keel-common_${VERSION}_all.deb \
    ./keel-tools_${VERSION}_arm64.deb ./libkeel-camera1_${VERSION}_arm64.deb ./keel-camera_${VERSION}_arm64.deb
sudo keel-setup --user "$USER"
sudo reboot
```

Releases of the library are tagged `libkeel-vX.Y.Z` in the repository; each release carries the packages, their
`SHA256SUMS` and `install.sh`. The repository also hosts the hardware and firmware, whose releases are published
with `--latest=false` (section 7), so "latest" always names a library release. Should a non-library release become
"latest" anyway, the one-command URL has no `install.sh` to download (`curl` reports a 404 and nothing runs); use a
pinned release instead:

```sh
curl -fsSL https://github.com/BisocMOrg/Keel/releases/download/libkeel-v1.0.0/install.sh \
    | sudo sh -s -- --version 1.0.0 --user "$USER"
```

## 4. Device access and real-time priority

**Device nodes.** `99-keel.rules` covers only the Pi 4's main GPIO controller (`fe200000.gpio`, which carries
every line the baseboard uses) and `/dev/i2c-1` (the RGB LED driver). Where the distribution already has `gpio`
and `i2c` groups (Raspberry Pi OS), the nodes keep them; elsewhere (Ubuntu) they get the `keel` group. The rule
assigns with `=`, not `:=`, so the distribution's own model is never overridden. `keel-setup --user NAME` adds
the user to `keel` and to `gpio` and `i2c` where they exist.

**Camera nodes.** The camera's media and video nodes (`/dev/media*`, `/dev/video*`) and the memory heaps
(`/dev/dma_heap/*`) belong to the distribution's `video` group; Keel adds no rule for them. `keel-setup --user NAME`
adds the user to `video`. Without it, the camera library reports `KEEL_E_PERMISSION` naming the node and this
remedy.

**Real-time priority.** The motor PWM runs on a worker thread at SCHED_FIFO priority 40, below the kernel's
threaded interrupt handlers (50). The limit `@keel - rtprio 40` allows it for members of `keel` in login
sessions. Without it the worker runs best effort: `keel_open` still succeeds but sets
`KEEL_OPEN_WARN_BEST_EFFORT_TIMING`, `KEEL_CAP_REALTIME_PWM` is clear and `snapshot.timing_class` is
`KEEL_TIMING_BEST_EFFORT`. The library never changes the scheduling of the rest of the process.

**After installing or changing groups, log in again.** Group membership and limits are read at login.

## 5. The boot configuration (`keel-setup`)

`keel-setup` keeps the baseboard's pins free of other kernel functions. It writes one marked block at the end of
`/boot/firmware/config.txt` (or `/boot/config.txt` on older images):

```
# BEGIN keel-setup (managed block: run keel-setup again to change it)
[all]
# BCM8-11 drive the motors: SPI0 must not claim them.
dtparam=spi=off
# BCM0/1 are the safety MCU's programming lines: no HAT EEPROM probe on them.
force_eeprom_read=0
# I2C1 reaches the RGB LED driver.
dtparam=i2c_arm=on
# END keel-setup
```

- It also turns any active `dtparam=spi=on` line elsewhere in the file off, keeping its comment.
- `--i2c-fast` adds `dtparam=i2c_arm_baudrate=400000`. That speed applies to every device on I2C1, so it is
  opt-in; the LEDs work at the default 100 kHz.
- Running it again replaces the block in place; the result depends only on the options of the last run.
- Before any change it saves the original as `config.txt.keel-backup-<UTC time>` and replaces the file
  atomically. It never reboots: reboot for the change to take effect.
- `--dry-run` prints the planned diff and group changes as any user; `--json` prints a summary;
  `--no-boot-config` manages groups only. It refuses to run on anything but a BCM2711 board.

To undo it, delete the block (or restore a backup) and reboot.

## 6. Running an application as a service

`/usr/share/doc/keel-common/examples/keel-app.service` is a starting point:

- `User=` is the account given to `keel-setup --user`; systemd gives the service that user's groups, so the
  device nodes are accessible without root. `SupplementaryGroups=video` adds the camera's group in any case.
- `LimitRTPRIO=40` grants the PWM worker its priority. Services do not go through PAM, so the limits file does not
  apply to them.
- `Restart=no`: a restart could re-arm without an operator. Let a person decide.
- The application handles SIGTERM by calling `keel_disarm` (or `keel_emergency_stop`) and `keel_close`, from a
  normal thread: no libkeel function is async-signal-safe, so the signal handler only sets a `volatile
  sig_atomic_t` flag that the loop checks (or the application waits for the signal with `sigwait()` or a
  `signalfd`). If it hangs anyway, the safety MCU removes motor power when the heartbeat stops.

## 7. Building the packages

The packages are built with debhelper from `debian/`, using the project's own CMake install rules
(`-DKEEL_PACKAGING=ON` adds the `keel-common` files and `keel-setup`, see `cmake/packaging.cmake`).

```sh
# On an x86-64 machine running the robot's Ubuntu release (26.10), for the robot:
sudo dpkg --add-architecture arm64        # with an arm64 apt source
sudo apt-get install crossbuild-essential-arm64 debhelper cmake libc6-dev:arm64 \
    pkgconf:arm64 libcamera-dev:arm64 libjpeg-dev:arm64
dpkg-buildpackage -a arm64 -Pnocheck -b -us -uc
tests/packaging/check_debs.sh .. arm64

# Natively (runs the whole test suite during the build):
sudo apt-get install debhelper cmake g++ pkgconf python3 curl libcamera-dev libjpeg-dev
dpkg-buildpackage -b -us -uc
tests/packaging/check_debs.sh .. amd64
```

The cross build runs on the robot's Ubuntu release because `libkeel-camera1` must link, and depend on, the
libcamera of the system it is installed on (section 1). On a system without libcamera, build from source
(section 8): CMake then skips the camera library. `dpkg-buildpackage` writes the packages to the parent
directory. `dh_makeshlibs` compares the exported symbols with `debian/libkeel1.symbols` and
`debian/libkeel-camera1.symbols` and fails on any difference; `check_debs.sh` then checks each package's contents,
the SONAMEs, the exports, the glibc floor of `libkeel1` and the maintainer script. The CI workflow
(`.github/workflows/libkeel.yml`) builds both, the arm64 packages in an Ubuntu 26.10 container, and attaches the
arm64 packages, `SHA256SUMS` and `install.sh` to each `libkeel-v*` release.

To release version X.Y.Z: set `KEEL_VERSION_*` in `keel.h`, add a `debian/changelog` entry, give the
`CHANGELOG.md` section a date (`## [X.Y.Z] - YYYY-MM-DD`) and update `PROJECT_NUMBER` in the `Doxyfile` (the
`packaging.metadata` test checks that all four agree), then push the tag `libkeel-vX.Y.Z`. The release job
refuses a tag whose changelog section is not dated.

The monorepo also releases hardware and firmware. Their release workflows must create releases with
`gh release create --latest=false` (or `make_latest: false`), so that `releases/latest/download/install.sh` keeps
pointing at the newest library release.

## 8. Building from source on the robot

Without packages, build on the Pi and install the system integration with the library:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DKEEL_PACKAGING=ON
cmake --build build
sudo cmake --install build
sudo systemd-sysusers && sudo udevadm control --reload && sudo modprobe i2c-dev
sudo keel-setup --user "$USER" && sudo reboot
```

The system files go to the absolute directories udev, systemd and PAM read, whatever the prefix; stage an
install with `DESTDIR=...`. With `libcamera-dev`, `libjpeg-dev` and `g++` installed, the same build includes the
camera library and `keel-camera`; without libcamera it skips them and says so.

## 9. Upgrading and removing

`sudo apt-get install` with newer packages (or `install.sh` again) upgrades in place; every program and every
binding uses the new library at its next start, because they all load `libkeel.so.1`. A new major version would
come with a new SONAME and a new runtime package name, so it installs beside the old one.

`sudo apt-get remove libkeel1 libkeel-dev keel-common keel-tools libkeel-camera1 libkeel-camera-dev keel-camera
python3-keel` removes the packages (`sudo apt-get autoremove` then removes libcamera if nothing else uses it).
`sudo apt-get remove libkeel-camera1` alone removes the camera packages and keeps the board's. The `keel` group, the
group memberships and the `config.txt` block stay; remove them by hand if you want them gone.

## 10. When `keel_open` fails

Every failure fills the open report with the failing resource, the OS error, the path and a one-line hint.

| Status | Usual reason | Remedy |
|---|---|---|
| `KEEL_E_PERMISSION` | the user cannot open `/dev/gpiochipN` | `sudo keel-setup --user NAME`, then log in again |
| `KEEL_E_IN_USE` | a line is held by `spi0` or another program | `sudo keel-setup` (SPI0 off) and reboot; stop the other program |
| `KEEL_E_NOT_FOUND` | not a Raspberry Pi 4, a kernel older than 5.10, or the backend not built in | use a Pi 4 with a 64-bit OS; `KEEL_BACKEND=simulator` on a PC |
| warning `NO_RGB` | `/dev/i2c-1` missing or the LED driver silent | `sudo modprobe i2c-dev`; check that I2C1 is on (`keel-setup`) |
| warning `BEST_EFFORT_TIMING` | no real-time privilege | log in again after `keel-setup`; `LimitRTPRIO=40` for services |

The camera library reports its failures the same way, with a message that names the remedy; the table in
[camera.md](camera.md) lists them. `keel-camera list` is the quickest check of the camera and its permissions.
