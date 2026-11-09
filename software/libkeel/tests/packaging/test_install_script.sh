#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Tests of install.sh against a stand-in release served through file://
# URLs: in --dry-run mode, package selection from SHA256SUMS (the board and
# the camera packages), --dev, --python, --no-camera, --camera and the camera
# packages left out where their dependencies are missing (with dpkg-deb),
# --version, --i2c-fast, checksum verification and the refusals; then a real
# install piped into sh and bash with stand-in system tools, and a truncated
# download.
#
# Usage: test_install_script.sh   (needs curl with file:// support)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
script="$here/../../install.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
me=$(id -un)

# release <dir> <file>...: stand-in .debs and their SHA256SUMS.
release() {
    dir=$1
    shift
    mkdir -p "$dir"
    for file in "$@"; do
        printf 'stand-in for %s\n' "$file" >"$dir/$file"
    done
    (cd "$dir" && sha256sum "$@" >SHA256SUMS)
}

# check <name> <expected status> <expected output pattern> <release dir> <install.sh args>...
check() {
    name=$1
    want=$2
    pattern=$3
    dir=$4
    shift 4
    status=0
    KEEL_RELEASE_URL="file://$dir" sh "$script" "$@" >"$work/out" 2>&1 || status=$?
    # A diagnostic from a tool the script runs is a defect even when the result is right.
    if [ "$status" -eq "$want" ] && grep -q -- "$pattern" "$work/out" \
            && ! grep -Eq '^(awk|sed|grep|sh|curl|mktemp|wc): ' "$work/out"; then
        echo "PASS  $name"
    else
        echo "FAIL  $name (status $status, wanted $want and /$pattern/):"
        sed 's/^/      /' "$work/out"
        failures=$((failures + 1))
    fi
}

board="./libkeel1_1.0.0_arm64.deb ./keel-common_1.0.0_all.deb ./keel-tools_1.0.0_arm64.deb"
camera="./libkeel-camera1_1.0.0_arm64.deb ./keel-camera_1.0.0_arm64.deb"
good="$work/good"
release "$good" libkeel1_1.0.0_arm64.deb libkeel-camera1_1.0.0_arm64.deb keel-common_1.0.0_all.deb \
    keel-tools_1.0.0_arm64.deb keel-camera_1.0.0_arm64.deb libkeel-dev_1.0.0_arm64.deb \
    libkeel-camera-dev_1.0.0_arm64.deb libkeel1_1.0.0_amd64.deb libkeel-camera1_1.0.0_amd64.deb \
    keel-tools_1.0.0_amd64.deb keel-camera_1.0.0_amd64.deb python3-keel_1.0.0_all.deb

check "selects the arm64 board and camera packages" 0 "would run: apt-get install -y $board $camera$" \
    "$good" --dry-run --user "$me"
check "passes the user to keel-setup" 0 "would run: keel-setup --user $me$" "$good" --dry-run --user "$me"
check "--dev adds both development packages" 0 \
    "./keel-tools_1.0.0_arm64.deb ./libkeel-dev_1.0.0_arm64.deb $camera ./libkeel-camera-dev_1.0.0_arm64.deb$" \
    "$good" --dry-run --dev
check "--python adds python3-keel" 0 "./keel-tools_1.0.0_arm64.deb ./python3-keel_1.0.0_all.deb $camera$" \
    "$good" --dry-run --python
check "--no-camera leaves out the camera packages" 0 "would run: apt-get install -y $board$" \
    "$good" --dry-run --no-camera
check "--no-camera --dev installs only the board's development package" 0 \
    "would run: apt-get install -y $board ./libkeel-dev_1.0.0_arm64.deb$" "$good" --dry-run --no-camera --dev
check "--i2c-fast reaches keel-setup" 0 "would run: keel-setup --i2c-fast$" "$good" --dry-run --i2c-fast
check "--version selects that release" 0 "all downloads match SHA256SUMS" "$good" --dry-run --version 1.0.0
check "--version without a match" 1 "lists 0 files for libkeel1" "$good" --dry-run --version 2.0.0
check "--version must be X.Y.Z" 1 "must look like 1.0.0" "$good" --dry-run --version 1.0
check "unknown user" 1 "no such user" "$good" --dry-run --user "no-such-user-$$"
check "unknown option" 2 "unknown option --frobnicate" "$good" --dry-run --frobnicate

tampered="$work/tampered"
release "$tampered" libkeel1_1.0.0_arm64.deb libkeel-camera1_1.0.0_arm64.deb keel-common_1.0.0_all.deb \
    keel-tools_1.0.0_arm64.deb keel-camera_1.0.0_arm64.deb
printf 'changed after signing\n' >>"$tampered/keel-tools_1.0.0_arm64.deb"
check "a tampered download is refused" 1 "checksum mismatch" "$tampered" --dry-run

partial="$work/partial"
release "$partial" libkeel1_1.0.0_arm64.deb libkeel-camera1_1.0.0_arm64.deb keel-tools_1.0.0_arm64.deb
check "a release without keel-common is refused" 1 "lists 0 files for keel-common" "$partial" --dry-run
no_camera="$work/no-camera"
release "$no_camera" libkeel1_1.0.0_arm64.deb keel-common_1.0.0_all.deb keel-tools_1.0.0_arm64.deb
check "a release without the camera packages is refused" 1 "lists 0 files for libkeel-camera1" "$no_camera" \
    --dry-run
check "a release without the camera packages installs with --no-camera" 0 "would run: apt-get install -y $board$" \
    "$no_camera" --dry-run --no-camera
core="$work/core"
release "$core" libkeel1_1.0.0_arm64.deb libkeel-camera1_1.0.0_arm64.deb keel-common_1.0.0_all.deb \
    keel-tools_1.0.0_arm64.deb keel-camera_1.0.0_arm64.deb libkeel-dev_1.0.0_arm64.deb
check "--python needs python3-keel in the release" 1 "lists 0 files for python3-keel" "$core" --dry-run --python
check "--dev needs libkeel-camera-dev in the release" 1 "lists 0 files for libkeel-camera-dev" "$core" --dry-run --dev

# The camera packages need the libcamera they were built against. A camera
# library whose dependency this system lacks is left out, unless --camera.
if command -v dpkg-deb >/dev/null 2>&1 && command -v dpkg-query >/dev/null 2>&1; then
    # camera_release <dir> <Depends>: a release whose libkeel-camera1 is a real package.
    camera_release() {
        release "$1" libkeel1_1.0.0_arm64.deb keel-common_1.0.0_all.deb keel-tools_1.0.0_arm64.deb \
            keel-camera_1.0.0_arm64.deb
        mkdir -p "$1.pkg/DEBIAN"
        printf '%s\n' 'Package: libkeel-camera1' 'Version: 1.0.0' 'Architecture: arm64' 'Maintainer: stand-in' \
            "Depends: $2" 'Description: stand-in' >"$1.pkg/DEBIAN/control"
        dpkg-deb --root-owner-group --build "$1.pkg" "$1/libkeel-camera1_1.0.0_arm64.deb" >/dev/null
        (cd "$1" && sha256sum -- *.deb >SHA256SUMS)
    }
    absent="keel-test-absent-$$"
    camera_release "$work/lacking" "$absent (>= 0.7.0)"
    check "a camera library whose dependency is missing is left out" 0 \
        "the camera packages are left out: this system does not provide $absent (>= 0.7.0)," "$work/lacking" --dry-run
    check "the board packages still install" 0 "would run: apt-get install -y $board$" "$work/lacking" --dry-run
    check "--camera installs the camera packages anyway" 0 "would run: apt-get install -y $board $camera$" \
        "$work/lacking" --dry-run --camera
    camera_release "$work/present" "$absent | dpkg"
    check "a camera library whose dependencies are present is installed" 0 \
        "would run: apt-get install -y $board $camera$" "$work/present" --dry-run
else
    echo "SKIP  camera dependency checks: no dpkg-deb"
fi

two="$work/two"
release "$two" libkeel1_1.0.0_arm64.deb libkeel1_1.0.1_arm64.deb libkeel-camera1_1.0.0_arm64.deb \
    keel-common_1.0.0_all.deb keel-tools_1.0.0_arm64.deb keel-camera_1.0.0_arm64.deb
check "an ambiguous release is refused" 1 "lists 2 files for libkeel1" "$two" --dry-run
check "--version resolves the ambiguity" 0 "all downloads match" "$two" --dry-run --version 1.0.0

if [ "$(id -u)" -ne 0 ]; then
    check "a real install needs root" 1 "run as root" "$good" --user "$me"
fi

# A real install as "curl ... | sudo sh -s -- --user NAME" runs it, with
# stand-ins for root, dpkg, apt-get, keel-setup and keel-doctor. Each records
# its arguments and whatever it reads from standard input: under a pipe that
# is the script itself, so a tool that inherited it would eat the rest.
bin="$work/bin"
log="$work/log"
mkdir -p "$bin" "$log"
real_id=$(command -v id)
# shellcheck disable=SC2016 # the stand-ins expand their own arguments when they run
printf '#!/bin/sh\nif [ "${1:-}" = -u ]; then echo 0; else exec %s "$@"; fi\n' "$real_id" >"$bin/id"
# shellcheck disable=SC2016
printf '#!/bin/sh\n[ "${1:-}" = --print-architecture ] && echo arm64\n' >"$bin/dpkg"
for tool in apt-get keel-setup keel-doctor; do
    printf '#!/bin/sh\nprintf "%%s\\n" "$*" >>"%s/%s.args"\ncat >>"%s/%s.stdin"\n' \
        "$log" "$tool" "$log" "$tool" >"$bin/$tool"
done
chmod +x "$bin"/*

for shell in sh bash; do
    command -v "$shell" >/dev/null 2>&1 || continue
    rm -f "$log"/*
    status=0
    # shellcheck disable=SC2002 # a pipe, exactly as curl | sh delivers the script
    cat "$script" | KEEL_RELEASE_URL="file://$good" PATH="$bin:$PATH" "$shell" -s -- --user "$me" \
        >"$work/out" 2>&1 || status=$?
    problem=""
    if [ "$status" -ne 0 ]; then
        problem="exit status $status"
    elif ! grep -q -- "-o Dpkg::Options::=--force-confold .*./libkeel1_1.0.0_arm64.deb" \
            "$log/apt-get.args" 2>/dev/null; then
        problem="apt-get did not install the packages keeping changed conffiles"
    elif [ "$(cat "$log/keel-setup.args" 2>/dev/null)" != "--user $me" ]; then
        problem="keel-setup did not run for $me"
    elif [ "$(cat "$log/keel-doctor.args" 2>/dev/null)" != "--user $me" ]; then
        problem="keel-doctor did not describe $me"
    elif [ -s "$log/apt-get.stdin" ] || [ -s "$log/keel-setup.stdin" ] \
            || [ -s "$log/keel-doctor.stdin" ]; then
        problem="a tool read the script from standard input"
    fi
    if [ -z "$problem" ]; then
        echo "PASS  piped through $shell -s, every step runs without standard input"
    else
        echo "FAIL  piped through $shell -s: $problem"
        sed 's/^/      /' "$work/out" "$log"/* 2>/dev/null
        failures=$((failures + 1))
    fi
done

# A download cut short must run nothing, not a prefix of the script.
head -c "$(($(wc -c <"$script") * 2 / 3))" "$script" >"$work/truncated.sh"
rm -f "$log"/*
status=0
KEEL_RELEASE_URL="file://$good" PATH="$bin:$PATH" sh "$work/truncated.sh" --user "$me" \
    >"$work/out" 2>&1 || status=$?
if [ "$status" -ne 0 ] && [ -z "$(ls "$log")" ] && ! grep -q '^install.sh: ' "$work/out"; then
    echo "PASS  a truncated download runs nothing"
else
    echo "FAIL  a truncated download ran (status $status):"
    sed 's/^/      /' "$work/out"
    failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
    echo "test_install_script: $failures case(s) failed"
    exit 1
fi
echo "test_install_script: all cases passed"
