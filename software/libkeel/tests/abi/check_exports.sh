#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Export gate for a built shared library (libkeel.so.1 or libkeel-camera.so.1):
#   1. the library exports exactly the functions of the symbols list, under
#      the version node of its version script (KEEL_1.0, KEEL_CAMERA_1.0), so
#      keel_port_*, keel__* and the camera library's internals stay hidden;
#   2. the version script lists exactly the same names;
#   3. every global symbol defined in the static archive, when one is given,
#      is keel_* or keel__* (the MCU policy's own names are renamed by its
#      wrapper translation unit).
#
# Usage: check_exports.sh <library.so> <archive.a or -> <symbols.txt> <version-script>
# Environment: NM (nm).
set -eu

[ "$#" -eq 4 ] || { echo "usage: $0 <library.so> <archive.a or -> <symbols.txt> <version-script>" >&2; exit 2; }
shared=$1
archive=$2
symbols=$3
map=$4
NM=${NM:-nm}
name=$(basename "$shared")
map_name=$(basename "$map")
node=$(sed -n 's/^\([A-Z][A-Z_0-9.]*\) {$/\1/p' "$map")
[ -n "$node" ] || { echo "FAIL  $map_name names no version node" >&2; exit 1; }
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT INT TERM
status=0

grep -v '^#' "$symbols" | grep -v '^$' | sort >"$out/expected"

# Defined dynamic symbols, without the version suffix; the version node
# itself is an absolute symbol (type A) and not a function.
"$NM" -D --defined-only "$shared" | awk '$2 != "A" { print $3 }' | sed 's/@.*//' | sort >"$out/exported"
if ! cmp -s "$out/expected" "$out/exported"; then
    echo "FAIL  $name exports differ from $(basename "$symbols") (< expected, > exported):"
    diff "$out/expected" "$out/exported" | sed 's/^/      /' || true
    status=1
else
    echo "PASS  $name exports exactly the $(wc -l <"$out/expected") functions of $(basename "$symbols")"
fi
if "$NM" -D --defined-only "$shared" | awk '$2 != "A" { print $3 }' | grep -v "@@$node\$" >"$out/unversioned"; then
    echo "FAIL  exports outside the $node version node:"
    sed 's/^/      /' "$out/unversioned"
    status=1
else
    echo "PASS  every export carries the $node version"
fi

sed -n '/global:/,/local:/p' "$map" | grep -o 'keel_[a-z0-9_]*' | sort >"$out/mapped"
if ! cmp -s "$out/expected" "$out/mapped"; then
    echo "FAIL  $map_name differs from $(basename "$symbols") (< expected, > $map_name):"
    diff "$out/expected" "$out/mapped" | sed 's/^/      /' || true
    status=1
else
    echo "PASS  $map_name lists exactly the functions of $(basename "$symbols")"
fi

if [ "$archive" != - ]; then
    "$NM" -g --defined-only "$archive" | awk 'NF == 3 { print $3 }' | sort -u >"$out/archive"
    if grep -v '^keel_' "$out/archive" >"$out/foreign"; then
        echo "FAIL  $(basename "$archive") defines globals outside keel_/keel__:"
        sed 's/^/      /' "$out/foreign"
        status=1
    else
        echo "PASS  $(basename "$archive") defines only keel_ and keel__ globals ($(wc -l <"$out/archive") symbols)"
    fi
fi
exit "$status"
