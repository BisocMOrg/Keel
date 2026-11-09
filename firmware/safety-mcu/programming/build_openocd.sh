#!/usr/bin/env bash
# Build a pinned Raspberry Pi OpenOCD, without accessing target hardware.
# Usage: bash firmware/safety-mcu/programming/build_openocd.sh ABSOLUTE_INSTALL_PREFIX [BUILD_DIRECTORY]
# Dependencies: git build-essential autoconf automake libtool pkg-config
#               libgpiod-dev libusb-1.0-0-dev python3 (texinfo optional).
set -euo pipefail
COMMIT=acff23ffd100479d6481a1155123774b79a990b9
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${1:?Specify an absolute install prefix writable by this user}"
[[ $PREFIX == /* ]] || { echo "Install prefix must be absolute" >&2; exit 1; }
WORK="${2:-$PWD/build-openocd}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
SOURCE="$WORK/source"
if [ ! -d "$SOURCE/.git" ]; then
    mkdir -p "$SOURCE"
    git -C "$SOURCE" init
    git -C "$SOURCE" remote add origin https://github.com/raspberrypi/openocd.git
    git -C "$SOURCE" fetch --depth 1 origin "$COMMIT"
    git -C "$SOURCE" checkout --detach FETCH_HEAD
fi
[[ $(git -C "$SOURCE" rev-parse HEAD) == "$COMMIT" ]] || {
    echo "Existing build directory has a different source revision" >&2; exit 1;
}
git -C "$SOURCE" diff --exit-code HEAD --
git -C "$SOURCE" submodule update --init --depth 1 jimtcl
(
    cd "$SOURCE"
    ./bootstrap nosubmodule
)
mkdir -p "$WORK/objects"
(
    cd "$WORK/objects"
    "$SOURCE/configure" --prefix="$PREFIX" --enable-linuxgpiod --enable-internal-jimtcl \
        --disable-jlink --disable-internal-libjaylink --disable-werror \
        --disable-doxygen-html --disable-doxygen-pdf
    make -j"${JOBS:-2}"
    make install
)
python3 "$SCRIPT_DIR/verify_openocd.py" record "$PREFIX" --source "$SOURCE"
printf 'Use: sudo env KEEL_OPENOCD_PREFIX=%q bash firmware/safety-mcu/programming/flash_safety_mcu.sh firmware/safety-mcu/release/safety_controller.hex\n' "$PREFIX"
