#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# One-command install of libkeel on a Raspberry Pi 4 running 64-bit Linux:
#
#   curl -fsSL https://github.com/BisocMOrg/Keel/releases/latest/download/install.sh \
#       | sudo sh -s -- --user "$USER"
#
# It downloads the release's SHA256SUMS, picks the arm64 packages from it,
# verifies every download against it and installs with apt the board packages
# libkeel1, keel-common and keel-tools (plus libkeel-dev with --dev and
# python3-keel with --python) and the camera packages libkeel-camera1 and
# keel-camera (plus libkeel-camera-dev with --dev). The camera packages need
# the libcamera of the distribution the release was built on (apt brings it,
# with its Raspberry Pi IPA modules): where that is not available they are
# left out with a message, and --no-camera always leaves them out. It then
# runs keel-setup for the user and prints keel-doctor's report when that tool
# is installed. The options are listed by --help (usage below).
set -eu

REPOSITORY_URL="https://github.com/BisocMOrg/Keel"
PACKAGE_ARCH="arm64"

usage() {
    cat <<'EOF'
Usage: install.sh [options]   (as root: curl ... | sudo sh -s -- [options])

Installs libkeel on a Raspberry Pi 4 with 64-bit Linux from a GitHub release.

Options:
  --user NAME      account that runs robot applications (default: $SUDO_USER)
  --dev            also install the development files (libkeel-dev, and
                   libkeel-camera-dev with the camera packages)
  --python         also install python3-keel (the Python binding: import keel)
  --no-camera      leave out the camera packages (libkeel-camera1, keel-camera)
  --camera         install the camera packages even if this system seems to
                   lack their libcamera (by default they are left out then)
  --version X.Y.Z  install that release instead of the latest one
  --i2c-fast       let keel-setup set I2C1 to 400 kHz
  --dry-run        download and verify, then print what would run
  --help           show this text
Environment:
  KEEL_RELEASE_URL base URL of the release files (a mirror, or file:// for tests)
EOF
    exit "$1"
}

say() { printf 'install.sh: %s\n' "$*"; }
die() { printf 'install.sh: error: %s\n' "$*" >&2; exit 1; }

# missing_dependencies <deb>: the dependencies of a package that this system
# has neither installed nor available from apt, each as " name (version)". A
# file that dpkg-deb cannot read yields nothing, and apt has the last word.
missing_dependencies() {
    depends=$(dpkg-deb --field "$1" Depends 2>/dev/null) || return 0
    printf '%s\n' "$depends" | tr ',' '\n' | while IFS= read -r dependency; do
        found=0
        for alternative in $(printf '%s\n' "$dependency" | sed 's/([^)]*)//g; s/|/ /g'); do
            name=${alternative%%:*}
            # shellcheck disable=SC2016 # dpkg-query expands its own field name
            if dpkg-query -W -f '${db:Status-Status}' "$name" 2>/dev/null </dev/null | grep -qx installed \
                    || apt-cache show "$name" >/dev/null 2>&1 </dev/null; then
                found=1
                break
            fi
        done
        [ "$found" -eq 1 ] || printf ' %s' "$(printf '%s' "$dependency" | sed 's/^ *//; s/ *$//')"
    done
}

# Everything runs from main(), called on the last line: a download cut short
# is a syntax error that runs nothing, and under "curl | sh" the shell has
# read the whole script before any command can read standard input.
main() {
    user=${SUDO_USER:-}
    dev=0
    python=0
    version=""
    i2c_fast=0
    dry_run=0
    camera=auto
    while [ "$#" -gt 0 ]; do
        case $1 in
            --user) [ "$#" -ge 2 ] || die "--user needs a name"; user=$2; shift 2 ;;
            --dev) dev=1; shift ;;
            --python) python=1; shift ;;
            --camera) camera=on; shift ;;
            --no-camera) camera=off; shift ;;
            --version) [ "$#" -ge 2 ] || die "--version needs X.Y.Z"; version=$2; shift 2 ;;
            --i2c-fast) i2c_fast=1; shift ;;
            --dry-run) dry_run=1; shift ;;
            --help|-h) usage 0 ;;
            *) printf 'install.sh: unknown option %s\n' "$1" >&2; usage 2 >&2 ;;
        esac
    done

    if [ -n "$version" ]; then
        printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "--version must look like 1.0.0"
    fi
    if [ -n "$user" ]; then
        id "$user" >/dev/null 2>&1 || die "no such user: $user"
    fi
    if [ "$dry_run" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
        die "run as root: curl ... | sudo sh -s -- --user \"\$USER\" (or add --dry-run)"
    fi

    machine_arch=$(dpkg --print-architecture 2>/dev/null || uname -m)
    if [ "$machine_arch" != "$PACKAGE_ARCH" ]; then
        [ "$dry_run" -eq 1 ] || die "this system is $machine_arch; the packages need 64-bit (arm64) Linux on a Raspberry Pi 4"
        say "warning: this system is $machine_arch; showing the arm64 install anyway (--dry-run)"
    fi

    # libkeel releases are tagged libkeel-vX.Y.Z in the monorepo.
    if [ -n "${KEEL_RELEASE_URL:-}" ]; then
        base=${KEEL_RELEASE_URL%/}
    elif [ -n "$version" ]; then
        base="$REPOSITORY_URL/releases/download/libkeel-v$version"
    else
        base="$REPOSITORY_URL/releases/latest/download"
    fi

    if command -v curl >/dev/null 2>&1; then
        fetch() { curl -fsSL --retry 3 -o "$2" "$1"; }
    elif command -v wget >/dev/null 2>&1; then
        fetch() { wget -q -O "$2" "$1"; }
    else
        die "neither curl nor wget is installed"
    fi
    command -v sha256sum >/dev/null 2>&1 || die "sha256sum is missing (coreutils)"

    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    trap 'exit 130' INT TERM

    say "fetching $base/SHA256SUMS"
    fetch "$base/SHA256SUMS" "$work/SHA256SUMS" || die "cannot download $base/SHA256SUMS"

    # The version pattern restricts names to package_version_arch.deb, so a name
    # taken from SHA256SUMS can never point outside the work directory.
    if [ -n "$version" ]; then
        version_pattern=$(printf '%s' "$version" | sed 's/\./\\./g')
    else
        version_pattern='[0-9][0-9A-Za-z.+~-]*'
    fi

    packages="libkeel1:$PACKAGE_ARCH keel-common:all keel-tools:$PACKAGE_ARCH"
    if [ "$dev" -eq 1 ]; then
        packages="$packages libkeel-dev:$PACKAGE_ARCH"
    fi
    if [ "$python" -eq 1 ]; then
        packages="$packages python3-keel:all"
    fi
    camera_packages=""
    if [ "$camera" != off ]; then
        camera_packages="libkeel-camera1:$PACKAGE_ARCH keel-camera:$PACKAGE_ARCH"
        if [ "$dev" -eq 1 ]; then
            camera_packages="$camera_packages libkeel-camera-dev:$PACKAGE_ARCH"
        fi
    fi

    : >"$work/selected"
    files=""
    camera_files=""
    camera_library=""
    for entry in $packages $camera_packages; do
        name=${entry%%:*}
        arch=${entry#*:}
        # The pattern travels through the environment: awk -v would turn its
        # backslashes into escape sequences.
        KEEL_PATTERN="^${name}_${version_pattern}_${arch}\\.deb\$" awk '
            NF == 2 && $1 ~ /^[0-9a-f]+$/ && length($1) == 64 {
                file = $2
                sub(/^\*/, "", file)
                if (file ~ ENVIRON["KEEL_PATTERN"]) print $1 "  " file
            }' "$work/SHA256SUMS" >"$work/match"
        count=$(wc -l <"$work/match")
        [ "$count" -eq 1 ] || die "SHA256SUMS lists $count files for $name ($arch)${version:+ $version}; is $base a libkeel release?"
        cat "$work/match" >>"$work/selected"
        file=$(awk '{ print $2 }' "$work/match")
        say "downloading $file"
        fetch "$base/$file" "$work/$file" || die "cannot download $base/$file"
        case " $camera_packages " in
            *" $entry "*) camera_files="$camera_files ./$file" ;;
            *) files="$files ./$file" ;;
        esac
        [ "$name" != libkeel-camera1 ] || camera_library=$file
    done

    (cd "$work" && sha256sum --check --strict --quiet selected) \
        || die "checksum mismatch: the downloads do not match SHA256SUMS"
    say "all downloads match SHA256SUMS"

    # The camera packages link the libcamera of the release's distribution.
    if [ "$camera" = auto ]; then
        missing=$(missing_dependencies "$work/$camera_library")
        if [ -n "$missing" ]; then
            say "the camera packages are left out: this system does not provide$missing," \
                "which libkeel-camera1 needs (--camera installs them anyway; the camera guide, camera.md," \
                "describes building the camera library for this system)"
            camera_files=""
        fi
    fi
    files="$files$camera_files"

    setup_args=""
    [ -z "$user" ] || setup_args=" --user $user"
    [ "$i2c_fast" -eq 0 ] || setup_args="$setup_args --i2c-fast"

    doctor_args=""
    [ -z "$user" ] || doctor_args=" --user $user"

    if [ "$dry_run" -eq 1 ]; then
        say "would run: apt-get install -y$files"
        say "would run: keel-setup$setup_args"
        say "would run: keel-doctor$doctor_args (if installed)"
        exit 0
    fi

    # apt resolves the packages' dependencies (libc, python3, systemd) itself.
    # Nothing below reads standard input: under "curl | sh" it is the script
    # itself, and dpkg's conffile question (an admin-edited limits file on an
    # upgrade) is answered by keeping the admin's file instead of asking.
    # shellcheck disable=SC2086 # the file list and the argument lists are split on purpose
    (cd "$work" && DEBIAN_FRONTEND=noninteractive apt-get install -y \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold $files </dev/null) \
        || die "apt-get install failed; run 'apt-get update' and try again"
    # shellcheck disable=SC2086
    keel-setup $setup_args </dev/null \
        || die "keel-setup failed; the packages are installed, rerun keel-setup by hand"
    if [ -z "$user" ]; then
        say "no --user given: run 'sudo keel-setup --user NAME' to give an account device access"
    fi

    if command -v keel-doctor >/dev/null 2>&1; then
        # As root, --user describes that account's access, not root's.
        # shellcheck disable=SC2086
        keel-doctor $doctor_args </dev/null || true
        [ -n "$user" ] || say "the report above describes root; run keel-doctor as the robot's user after logging in again"
    else
        say "keel-doctor is not part of this release; skipping the system check"
    fi
    say "done: log in again for new group memberships, and reboot if keel-setup changed config.txt"
}

main "$@"
