#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Builds and runs the consumers against an installed libkeel only, the way
# applications and bindings will:
#   1. C via pkg-config (keel.pc), and a port.h program via keel-static.pc
#      that links libkeel.a and needs no libkeel.so;
#   2. C++ via find_package(Keel) and Keel::keel;
#   3. C via find_package(Keel) and Keel::keel_static (no libkeel.so needed);
#   4. Python ctypes, loading by KEEL_LIBRARY and by the SONAME;
#   5. the camera library, when installed: C via pkg-config keel-camera and
#      C++ via find_package(Keel COMPONENTS camera), without libkeel.so;
#   6. staged only: the board's files without the camera's (libkeel-dev
#      without libkeel-camera-dev): find_package(Keel) still works and
#      reports the camera component as not found.
# Every program opens the simulator, arms, drives and closes.
#
# Usage:
#   check_consumers.sh staged <build-dir> <source-dir> <work-dir>
#       cmake --install the build under DESTDIR=<work-dir>/root (prefix
#       /usr/local; the KEEL_PACKAGING system files land in the same root)
#   check_consumers.sh system <source-dir> <work-dir>
#       use the installed packages (libkeel1 and libkeel-dev, and libkeel-camera1
#       and libkeel-camera-dev when installed) in /usr
# Environment: CMAKE (cmake), CC (cc), CXX (c++), PKG_CONFIG (pkg-config), PYTHON (python3).
set -eu

usage() { echo "usage: $0 staged <build-dir> <source-dir> <work-dir> | system <source-dir> <work-dir>" >&2; exit 2; }
[ "$#" -ge 1 ] || usage
mode=$1
shift
case $mode in
    staged) [ "$#" -eq 3 ] || usage; build=$1; source=$2; work=$3 ;;
    system) [ "$#" -eq 2 ] || usage; source=$1; work=$2 ;;
    *) usage ;;
esac
CMAKE=${CMAKE:-cmake}
# Under "make test" (the Debian build) the parent's jobserver flags would
# leak into the consumer project's own make.
unset MAKEFLAGS MFLAGS MAKELEVEL
CC=${CC:-cc}
CXX=${CXX:-c++}
PKG_CONFIG=${PKG_CONFIG:-pkg-config}
PYTHON=${PYTHON:-python3}
consumers="$source/tests/consumers"

rm -rf "$work"
mkdir -p "$work/project"
if [ "$mode" = staged ]; then
    prefix="$work/root/usr/local"
    DESTDIR="$work/root" "$CMAKE" --install "$build" --prefix /usr/local >"$work/install.log"
    pc=$(find "$prefix" -name keel.pc)
    libdir=$(dirname "$(dirname "$pc")")
    header="$prefix/include/keel/keel.h"
    PKG_CONFIG_PATH=$(dirname "$pc")
    export PKG_CONFIG_PATH
    prefix_arg="-DCMAKE_PREFIX_PATH=$prefix"
    # The staged library is not on the loader path; the SONAME needs a hint.
    LD_LIBRARY_PATH=$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    export LD_LIBRARY_PATH
else
    libdir=$("$PKG_CONFIG" --variable=libdir keel)
    header="$("$PKG_CONFIG" --variable=includedir keel)/keel/keel.h"
    prefix_arg=""
fi

# 1. C via pkg-config.
# shellcheck disable=SC2046 # pkg-config output is split on purpose
"$CC" $("$PKG_CONFIG" --cflags keel) "$consumers/c_consumer.c" $("$PKG_CONFIG" --libs keel) \
    -o "$work/pc_consumer"
"$work/pc_consumer" >"$work/pc.log"
echo "PASS  C via pkg-config"
# The port API exists only in libkeel.a, so this links only if keel-static.pc
# really selects the archive; the program must not need libkeel.so either.
# shellcheck disable=SC2046
"$CC" $("$PKG_CONFIG" --cflags keel-static) "$consumers/port_consumer.c" \
    $("$PKG_CONFIG" --libs keel-static) -o "$work/port_consumer"
"$work/port_consumer" >"$work/port.log"
if readelf -d "$work/port_consumer" | grep -q 'NEEDED.*libkeel'; then
    echo "FAIL  the keel-static.pc program depends on libkeel.so"
    exit 1
fi
echo "PASS  C and the port API via pkg-config keel-static (no libkeel.so dependency)"

# 2 and 3. CMake package.
cp "$consumers/c_consumer.c" "$consumers/cxx_consumer.cpp" "$work/project/"
cat >"$work/project/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(keel_consumer C CXX)
find_package(Keel 1.0 REQUIRED)
find_package(Threads REQUIRED)
add_executable(cxx_consumer cxx_consumer.cpp)
set_target_properties(cxx_consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(cxx_consumer PRIVATE Keel::keel Threads::Threads)
add_executable(static_consumer c_consumer.c)
target_link_libraries(static_consumer PRIVATE Keel::keel_static)
EOF
# shellcheck disable=SC2086 # prefix_arg is empty or one word
"$CMAKE" -S "$work/project" -B "$work/project/build" $prefix_arg \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" >"$work/configure.log"
"$CMAKE" --build "$work/project/build" >"$work/build.log"
"$work/project/build/cxx_consumer" >"$work/cxx.log"
echo "PASS  C++ via find_package(Keel), Keel::keel"
"$work/project/build/static_consumer" >"$work/static.log"
if readelf -d "$work/project/build/static_consumer" | grep -q 'NEEDED.*libkeel'; then
    echo "FAIL  static_consumer depends on libkeel.so"
    exit 1
fi
echo "PASS  C via find_package(Keel), Keel::keel_static (no libkeel.so dependency)"

# 4. Python ctypes: the KEEL_LIBRARY path, then the SONAME.
KEEL_HEADER=$header KEEL_LIBRARY="$libdir/libkeel.so.1" CC=$CC \
    "$PYTHON" "$consumers/test_ctypes.py" metadata loop >"$work/ctypes_path.log" 2>&1 \
    || { cat "$work/ctypes_path.log"; exit 1; }
echo "PASS  Python ctypes via KEEL_LIBRARY"
env -u KEEL_LIBRARY KEEL_HEADER="$header" CC="$CC" \
    "$PYTHON" "$consumers/test_ctypes.py" metadata loop >"$work/ctypes_soname.log" 2>&1 \
    || { cat "$work/ctypes_soname.log"; exit 1; }
echo "PASS  Python ctypes via the SONAME libkeel.so.1"

# 5. The camera library, when it is installed: C via pkg-config keel-camera
# and C++ via find_package(Keel COMPONENTS camera). Neither may need
# libkeel.so. Without a camera (and without the synthetic one) the programs
# only list and exit.
if "$PKG_CONFIG" --exists keel-camera; then
    # shellcheck disable=SC2046
    "$CC" $("$PKG_CONFIG" --cflags keel-camera) "$consumers/camera_c_consumer.c" \
        $("$PKG_CONFIG" --libs keel-camera) -o "$work/camera_consumer"
    "$work/camera_consumer" >"$work/camera.log" || { cat "$work/camera.log"; exit 1; }
    if readelf -d "$work/camera_consumer" | grep -q 'NEEDED.*libkeel\.so'; then
        echo "FAIL  the camera consumer depends on libkeel.so"
        exit 1
    fi
    echo "PASS  camera C via pkg-config keel-camera (no libkeel.so dependency)"
    mkdir -p "$work/camera_project"
    cp "$consumers/camera_cxx_consumer.cpp" "$work/camera_project/"
    cat >"$work/camera_project/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21)
project(keel_camera_consumer CXX)
find_package(Keel 1.0 REQUIRED COMPONENTS camera)
find_package(Threads REQUIRED)
add_executable(camera_cxx_consumer camera_cxx_consumer.cpp)
set_target_properties(camera_cxx_consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(camera_cxx_consumer PRIVATE Keel::keel_camera Threads::Threads)
CMAKE
    # shellcheck disable=SC2086
    "$CMAKE" -S "$work/camera_project" -B "$work/camera_project/build" $prefix_arg \
        -DCMAKE_CXX_COMPILER="$CXX" >"$work/camera_configure.log"
    "$CMAKE" --build "$work/camera_project/build" >"$work/camera_build.log"
    "$work/camera_project/build/camera_cxx_consumer" >"$work/camera_cxx.log" \
        || { cat "$work/camera_cxx.log"; exit 1; }
    echo "PASS  camera C++ via find_package(Keel COMPONENTS camera), Keel::keel_camera"
else
    echo "SKIP  camera consumers: libkeel-camera is not installed"
fi

# 6. A copy of the staged install without the camera's development files, as
# libkeel-dev installs alone: the CMake package must work without them.
if [ "$mode" = staged ] && [ -n "$(find "$prefix" -name 'KeelCameraTargets.cmake')" ]; then
    cp -R "$work/root" "$work/board_root"
    board_prefix="$work/board_root/usr/local"
    find "$board_prefix" \( -name 'KeelCameraTargets*.cmake' -o -name 'libkeel-camera.so*' \
        -o -name camera.h -o -name keel-camera.pc \) -exec rm -f {} +
    mkdir -p "$work/board_project"
    cp "$consumers/c_consumer.c" "$work/board_project/"
    cat >"$work/board_project/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21)
project(keel_board_consumer C)
find_package(Keel 1.0 QUIET COMPONENTS camera)
if(Keel_FOUND OR TARGET Keel::keel_camera)
    message(FATAL_ERROR "find_package(Keel COMPONENTS camera) succeeded without the camera's files")
endif()
find_package(Keel 1.0 REQUIRED)
add_executable(board_consumer c_consumer.c)
target_link_libraries(board_consumer PRIVATE Keel::keel)
CMAKE
    "$CMAKE" -S "$work/board_project" -B "$work/board_project/build" -DCMAKE_PREFIX_PATH="$board_prefix" \
        -DCMAKE_C_COMPILER="$CC" >"$work/board_configure.log" || { cat "$work/board_configure.log"; exit 1; }
    "$CMAKE" --build "$work/board_project/build" >"$work/board_build.log"
    "$work/board_project/build/board_consumer" >"$work/board.log"
    echo "PASS  find_package(Keel) without the camera's files; the camera component is not found"
fi
