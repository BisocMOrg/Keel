#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Header gate for libkeel's public headers (keel.h, port.h) and the camera
# library's (camera.h):
#   1. a consumer of each compiles as C99, C11, C++11 and C++17 with strict
#      warnings;
#   2. layout.c and camera_layout.c pin every struct offset and size on the
#      host (C and C++11), i386, aarch64 LP64 and aarch64 ILP32 (GCC 15
#      deprecates ILP32: that warning alone is silenced);
#   3. check_constants.py and check_camera_constants.py: every constant
#      survives `cc -E -P -DKEEL_API=`, matches the compiler and the golden
#      file, and the functions match symbols.txt and camera_symbols.txt;
#   4. tests/docs/check_header_docs.py: no undocumented declaration.
#
# Usage: check_headers.sh [--require-cross] [--require-parser]
#   Cross targets whose toolchain is missing are reported as SKIP; with
#   --require-cross (CI) every cross target must build (i386 and ILP32 are
#   freestanding compiles and need only gcc and aarch64-linux-gnu-gcc). With
#   --require-parser (CI) a missing pycparser fails check_constants.py instead
#   of skipping its C-parser step.
# Environment: CC (gcc), CXX (g++), AARCH64_CC (aarch64-linux-gnu-gcc), PYTHON (python3).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
inc="$root/include"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT INT TERM

CC=${CC:-gcc}
CXX=${CXX:-g++}
AARCH64_CC=${AARCH64_CC:-aarch64-linux-gnu-gcc}
PYTHON=${PYTHON:-python3}
require_cross=0
parser_flag=""
for option in "$@"; do
    case $option in
        --require-cross) require_cross=1 ;;
        --require-parser) parser_flag=--require-parser ;;
        *) echo "usage: $0 [--require-cross] [--require-parser]" >&2; exit 2 ;;
    esac
done

cflags="-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow
        -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wcast-qual -Wredundant-decls"
cxxflags="-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow
          -Wundef -Wcast-qual -Wredundant-decls"
failures=0
passed=0

# Runs one check; prints its output only when it fails.
step() {
    name=$1
    shift
    if "$@" >"$out/log" 2>&1; then
        echo "PASS  $name"
        passed=1
    else
        echo "FAIL  $name"
        sed 's/^/      /' "$out/log"
        failures=$((failures + 1))
        passed=0
    fi
}

# Like step, but also prints the tool's summary when it passes.
step_verbose() {
    step "$@"
    if [ "$passed" -eq 1 ]; then
        sed 's/^/      /' "$out/log"
    fi
}

# A target is usable when its compiler builds an empty translation unit.
usable() {
    echo 'int keel_probe;' >"$out/probe.c"
    "$@" -c "$out/probe.c" -o "$out/probe.o" >/dev/null 2>&1
}

# 1. Consumers in every supported language mode.
for std in c99 c11; do
    # shellcheck disable=SC2086 # flag lists are split on purpose
    step "consumer $std" "$CC" -std=$std $cflags -I"$inc" -c "$here/header_use.c" -o "$out/use.o"
    # shellcheck disable=SC2086
    step "camera consumer $std" "$CC" -std=$std $cflags -I"$inc" -c "$here/camera_header_use.c" \
        -o "$out/use.o"
done
for std in c++11 c++17; do
    # shellcheck disable=SC2086
    step "consumer $std" "$CXX" -std=$std $cxxflags -I"$inc" -x c++ -c "$here/header_use.c" \
        -o "$out/use.o"
    # shellcheck disable=SC2086
    step "camera consumer $std" "$CXX" -std=$std $cxxflags -I"$inc" -x c++ -c \
        "$here/camera_header_use.c" -o "$out/use.o"
done

# 2. Layout on every target. The headers need only freestanding <stdint.h> and
# <stddef.h>, so targets without a C library still prove the layout.
host=$("$CC" -dumpmachine 2>/dev/null | cut -d- -f1)
# shellcheck disable=SC2086
step "layout ${host:-host} C11" "$CC" -std=c11 $cflags -I"$inc" -c "$here/layout.c" -o "$out/l.o"
# shellcheck disable=SC2086
step "layout ${host:-host} C++11" "$CXX" -std=c++11 $cxxflags -Wold-style-cast -I"$inc" -x c++ \
    -c "$here/layout.c" -o "$out/l.o"
# shellcheck disable=SC2086
step "camera layout ${host:-host} C11" "$CC" -std=c11 $cflags -I"$inc" -c "$here/camera_layout.c" \
    -o "$out/l.o"
# shellcheck disable=SC2086
step "camera layout ${host:-host} C++11" "$CXX" -std=c++11 $cxxflags -Wold-style-cast -I"$inc" -x c++ \
    -c "$here/camera_layout.c" -o "$out/l.o"
for target in "i386|$CC -m32 -ffreestanding" \
              "aarch64 LP64|$AARCH64_CC" \
              "aarch64 ILP32|$AARCH64_CC -mabi=ilp32 -ffreestanding -Wno-deprecated"; do
    label=${target%%|*}
    compiler=${target#*|}
    # shellcheck disable=SC2086
    if usable $compiler; then
        # shellcheck disable=SC2086
        step "layout $label" $compiler -std=c11 $cflags -I"$inc" -c "$here/layout.c" -o "$out/l.o"
        # shellcheck disable=SC2086
        step "camera layout $label" $compiler -std=c11 $cflags -I"$inc" -c "$here/camera_layout.c" \
            -o "$out/l.o"
    elif [ "$require_cross" -eq 1 ]; then
        echo "FAIL  layout $label: $compiler unavailable"
        failures=$((failures + 1))
    else
        echo "SKIP  layout $label: $compiler unavailable"
    fi
done

# 3 and 4. Preprocessed constants, symbols and documentation.
# shellcheck disable=SC2086 # an empty flag must vanish, not become ""
step_verbose "constants and symbols" "$PYTHON" "$here/check_constants.py" --cc "$CC" $parser_flag
# shellcheck disable=SC2086
step_verbose "camera constants and symbols" "$PYTHON" "$here/check_camera_constants.py" --cc "$CC" \
    $parser_flag
step_verbose "documentation" "$PYTHON" "$root/tests/docs/check_header_docs.py"

if [ "$failures" -ne 0 ]; then
    echo "check_headers: $failures step(s) failed"
    exit 1
fi
echo "check_headers: all steps passed"
