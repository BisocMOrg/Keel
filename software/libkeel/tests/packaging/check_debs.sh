#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Inspects the .debs of one dpkg-buildpackage run before they are published:
#   1. the seven packages exist for the architecture, at the keel.h version;
#   2. libkeel1 holds libkeel.so.1 (SONAME libkeel.so.1), exports exactly
#      tests/abi/symbols.txt under KEEL_1.0, needs no glibc symbol newer than
#      the ceiling, and depends on keel-common of the same version and libc6;
#   3. libkeel-camera1 holds libkeel-camera.so.1 (SONAME libkeel-camera.so.1),
#      exports exactly tests/abi/camera_symbols.txt under KEEL_CAMERA_1.0,
#      needs libcamera and libjpeg but neither libkeel.so nor FFmpeg, carries
#      no test hooks and depends on libcamera and its IPA modules;
#   4. libkeel-dev, libkeel-camera-dev, keel-common, keel-tools and
#      keel-camera hold exactly what the deployment guide promises; the board
#      packages (libkeel-dev, keel-tools) need nothing of the camera, so they
#      install without libcamera; keel-common's postinst creates the group and
#      loads i2c-dev without touching the boot configuration.
#
# Usage: check_debs.sh <directory with the .debs> <arch> [glibc ceiling, default 2.34]
# The glibc ceiling is the oldest supported target: Raspberry Pi OS bookworm
# has 2.36 and Ubuntu 22.04 has 2.35; 2.34 is the version that merged libpthread.
set -eu

[ "$#" -ge 2 ] || { echo "usage: $0 <deb-dir> <arch> [glibc-ceiling]" >&2; exit 2; }
debs=$(cd "$1" && pwd)
arch=$2
ceiling=${3:-2.34}
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0

pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; failures=$((failures + 1)); }

version=$(sed -n 's/^#define KEEL_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\)$/\2/p' \
    "$root/include/keel/keel.h" | paste -sd.)
case $arch in
    arm64) multiarch=aarch64-linux-gnu ;;
    amd64) multiarch=x86_64-linux-gnu ;;
    *) echo "check_debs: unsupported architecture $arch" >&2; exit 2 ;;
esac

# contents <package> <deb arch>: the regular files and links of a package, sorted.
contents() {
    dpkg-deb --fsys-tarfile "$debs/$1_${version}_$2.deb" | tar -t | sed 's|^\./|/|' \
        | grep -v '/$' | sort
}

for entry in libkeel1:$arch libkeel-dev:$arch keel-common:all keel-tools:$arch libkeel-camera1:$arch \
        libkeel-camera-dev:$arch keel-camera:$arch; do
    file="$debs/${entry%%:*}_${version}_${entry#*:}.deb"
    if [ -f "$file" ]; then pass "$(basename "$file")"; else fail "missing $(basename "$file")"; fi
done
[ "$failures" -eq 0 ] || exit 1

# ---- libkeel1 -------------------------------------------------------------------
dpkg-deb -x "$debs/libkeel1_${version}_$arch.deb" "$work/libkeel1"
library="$work/libkeel1/usr/lib/$multiarch/libkeel.so.$version"
if [ "$(contents libkeel1 "$arch" | grep -v '^/usr/share/doc/')" = "$(printf '%s\n' \
        "/usr/lib/$multiarch/libkeel.so.1" "/usr/lib/$multiarch/libkeel.so.$version")" ]; then
    pass "libkeel1 holds libkeel.so.1 and libkeel.so.$version only"
else
    fail "libkeel1 contents:"; contents libkeel1 "$arch" | sed 's/^/      /'
fi
soname=$(readelf -d "$library" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
if [ "$soname" = libkeel.so.1 ]; then pass "SONAME $soname"; else fail "SONAME is '$soname'"; fi
grep -v '^#' "$root/tests/abi/symbols.txt" | grep -v '^$' | sed 's/$/@@KEEL_1.0/' | sort >"$work/expected"
readelf --dyn-syms --wide "$library" | awk '$7 != "UND" && $5 == "GLOBAL" && $4 == "FUNC" { print $8 }' \
    | sort >"$work/exported"
if cmp -s "$work/expected" "$work/exported"; then
    pass "exports exactly the $(wc -l <"$work/expected") functions of symbols.txt under KEEL_1.0"
else
    fail "exports differ from symbols.txt (< expected, > exported):"
    diff "$work/expected" "$work/exported" | sed 's/^/      /' || true
fi
newest=$(readelf -V --wide "$library" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
if [ "$(printf '%s\n%s\n' "$newest" "$ceiling" | sort -V | tail -1)" = "$ceiling" ]; then
    pass "newest glibc symbol version $newest <= $ceiling"
else
    fail "needs glibc $newest, newer than the ceiling $ceiling"
fi
# keel_open() must reach keel_r3. The "built without" remedy is compiled
# only into a library that lacks the backend (src/core/api.c), and the chip
# label lives in the backend itself, which link-time optimisation drops when
# nothing calls it. Both are visible in a cross-built library as well.
if grep -aq 'built without the keel_r3 backend' "$library"; then
    fail "libkeel1 was built without the keel_r3 backend"
elif ! grep -aq 'pinctrl-bcm2711' "$library"; then
    fail "libkeel1 does not contain the keel_r3 backend (no pinctrl-bcm2711 chip label)"
else
    pass "keel_open reaches the keel_r3 backend"
fi
depends=$(dpkg-deb -f "$debs/libkeel1_${version}_$arch.deb" Depends)
case $depends in
    *"keel-common (= $version)"*libc6*) pass "libkeel1 Depends: $depends" ;;
    *) fail "libkeel1 Depends: $depends" ;;
esac


# ---- libkeel-camera1 ------------------------------------------------------------
camera_version=$(sed -n 's/^#define KEEL_CAMERA_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\)$/\2/p' \
    "$root/include/keel/camera.h" | paste -sd.)
dpkg-deb -x "$debs/libkeel-camera1_${version}_$arch.deb" "$work/camera"
camera_library="$work/camera/usr/lib/$multiarch/libkeel-camera.so.$camera_version"
if [ "$(contents libkeel-camera1 "$arch" | grep -v '^/usr/share/doc/')" = "$(printf '%s\n' \
        "/usr/lib/$multiarch/libkeel-camera.so.1" "/usr/lib/$multiarch/libkeel-camera.so.$camera_version")" ]; then
    pass "libkeel-camera1 holds libkeel-camera.so.1 and libkeel-camera.so.$camera_version only"
else
    fail "libkeel-camera1 contents:"; contents libkeel-camera1 "$arch" | sed 's/^/      /'
fi
soname=$(readelf -d "$camera_library" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
if [ "$soname" = libkeel-camera.so.1 ]; then pass "SONAME $soname"; else fail "SONAME is '$soname'"; fi
grep -v '^#' "$root/tests/abi/camera_symbols.txt" | grep -v '^$' | sed 's/$/@@KEEL_CAMERA_1.0/' | sort \
    >"$work/camera_expected"
readelf --dyn-syms --wide "$camera_library" \
    | awk '$7 != "UND" && $5 == "GLOBAL" && $4 == "FUNC" { print $8 }' | sort >"$work/camera_exported"
if cmp -s "$work/camera_expected" "$work/camera_exported"; then
    pass "exports exactly the $(wc -l <"$work/camera_expected") functions of camera_symbols.txt under KEEL_CAMERA_1.0"
else
    fail "camera exports differ from camera_symbols.txt (< expected, > exported):"
    diff "$work/camera_expected" "$work/camera_exported" | sed 's/^/      /' || true
fi
# The library needs libcamera and libjpeg, and never libkeel.so or FFmpeg.
needed=$(readelf -d "$camera_library" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | paste -sd' ')
case " $needed " in
    *" libkeel.so"*|*" libav"*) fail "libkeel-camera.so needs $needed" ;;
    *" libcamera.so."*" libcamera-base.so."*" libjpeg.so."*) pass "libkeel-camera.so needs $needed" ;;
    *) fail "libkeel-camera.so needs $needed" ;;
esac
if grep -aq 'test-hooks' "$camera_library" || grep -aq 'KEEL_CAMERA_TEST_SOURCE' "$camera_library"; then
    fail "libkeel-camera1 contains the test hooks"
else
    pass "libkeel-camera1 has no test hooks"
fi
depends=$(dpkg-deb -f "$debs/libkeel-camera1_${version}_$arch.deb" Depends)
case $depends in
    *libcamera-ipa*) ;;
    *) fail "libkeel-camera1 does not depend on libcamera-ipa: $depends" ;;
esac
case $depends in
    *libcamera0.*libc6*|*libc6*libcamera0.*) pass "libkeel-camera1 Depends: $depends" ;;
    *) fail "libkeel-camera1 Depends: $depends" ;;
esac

# ---- libkeel-dev, libkeel-camera-dev, keel-common, keel-tools, keel-camera ----------
# expect_files <package> <deb arch> <path>...: the package holds exactly these
# files besides /usr/share/doc.
expect_files() {
    package=$1
    package_arch=$2
    shift 2
    printf '%s\n' "$@" | sort >"$work/want"
    contents "$package" "$package_arch" | grep -v '^/usr/share/doc/' >"$work/have" || true
    if cmp -s "$work/want" "$work/have"; then
        pass "$package holds exactly its $# files"
    else
        fail "$package contents (< expected, > packaged):"
        diff "$work/want" "$work/have" | sed 's/^/      /' || true
    fi
}
expect_files libkeel-dev "$arch" /usr/include/keel/keel.h /usr/include/keel/port.h \
    "/usr/lib/$multiarch/libkeel.so" "/usr/lib/$multiarch/libkeel.a" \
    "/usr/lib/$multiarch/pkgconfig/keel.pc" "/usr/lib/$multiarch/pkgconfig/keel-static.pc" \
    "/usr/lib/$multiarch/cmake/Keel/KeelConfig.cmake" \
    "/usr/lib/$multiarch/cmake/Keel/KeelConfigVersion.cmake" \
    "/usr/lib/$multiarch/cmake/Keel/KeelTargets.cmake" \
    "/usr/lib/$multiarch/cmake/Keel/KeelTargets-none.cmake"
expect_files libkeel-camera-dev "$arch" /usr/include/keel/camera.h "/usr/lib/$multiarch/libkeel-camera.so" \
    "/usr/lib/$multiarch/pkgconfig/keel-camera.pc" \
    "/usr/lib/$multiarch/cmake/Keel/KeelCameraTargets.cmake" \
    "/usr/lib/$multiarch/cmake/Keel/KeelCameraTargets-none.cmake"
expect_files keel-common all /etc/security/limits.d/90-keel-rtprio.conf \
    /usr/lib/modules-load.d/keel.conf /usr/lib/sysusers.d/keel.conf \
    /usr/lib/udev/rules.d/99-keel.rules
if contents keel-common all | grep -qx /usr/share/doc/keel-common/examples/keel-app.service; then
    pass "keel-common ships the example unit"
else
    fail "keel-common lacks /usr/share/doc/keel-common/examples/keel-app.service"
fi
contents keel-tools "$arch" | grep -v '^/usr/share/doc/' >"$work/tools"
if grep -qx /usr/bin/keel-setup "$work/tools" && grep -qx /usr/bin/keel-doctor "$work/tools" \
        && ! grep -qx /usr/bin/keel-camera "$work/tools" && ! grep -vq '^/usr/bin/keel-' "$work/tools"; then
    pass "keel-tools holds $(paste -sd' ' "$work/tools")"
else
    fail "keel-tools contents:"; sed 's/^/      /' "$work/tools"
fi
expect_files keel-camera "$arch" /usr/bin/keel-camera
# The board packages install without the camera stack; the camera packages
# depend on the camera library of the same version.
for package in libkeel-dev keel-tools; do
    depends=$(dpkg-deb -f "$debs/${package}_${version}_$arch.deb" Depends)
    case $depends in
        *camera*) fail "$package depends on the camera: $depends" ;;
        *"libkeel1 (= $version)"*) pass "$package Depends: $depends" ;;
        *) fail "$package Depends: $depends" ;;
    esac
done
for package in libkeel-camera-dev keel-camera; do
    depends=$(dpkg-deb -f "$debs/${package}_${version}_$arch.deb" Depends)
    case $depends in
        *"libkeel-camera1 (= $version)"*) pass "$package Depends: $depends" ;;
        *) fail "$package Depends: $depends" ;;
    esac
done

dpkg-deb -e "$debs/keel-common_${version}_all.deb" "$work/common-control"
postinst="$work/common-control/postinst"
if grep -q 'systemd-sysusers.* keel.conf' "$postinst" && grep -q 'modprobe i2c-dev' "$postinst" \
        && ! grep -Eq 'config\.txt|i2c_arm_baudrate|usermod|gpasswd' "$postinst"; then
    pass "keel-common postinst creates the group and loads i2c-dev, and edits nothing else"
else
    fail "keel-common postinst:"; sed 's/^/      /' "$postinst"
fi

if [ "$failures" -ne 0 ]; then
    echo "check_debs: $failures check(s) failed"
    exit 1
fi
echo "check_debs: the $arch packages of libkeel $version are complete"
