# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Finding and checking libkeel-camera: the platform check, the resolution order and the ABI check."""

from __future__ import annotations

import os
import sys

from keel.camera import abi
from keel.camera.errors.exceptions import CameraLibraryError

from . import _generated

SONAME = "libkeel-camera.so.1"
"""The camera library's SONAME, resolved by the dynamic loader when KEEL_CAMERA_LIBRARY is not set."""

ENVIRONMENT = "KEEL_CAMERA_LIBRARY"
"""The environment variable that names the camera library by absolute path."""

PLATFORM = sys.platform
"""The platform the load check tests. libkeel-camera is Linux-only."""

_REMEDY = (
    "On the robot, install the libkeel-camera1 package (sudo apt install libkeel-camera1); elsewhere, build "
    "libkeel-camera from software/libkeel (it needs libcamera-dev) and set KEEL_CAMERA_LIBRARY to the absolute path "
    "of libkeel-camera.so.1"
)


def check_platform(platform: str) -> None:
    """Raise CameraLibraryError unless platform is Linux."""
    if not platform.startswith("linux"):
        raise CameraLibraryError(f"libkeel-camera requires Linux; found {platform}")


def resolve(path: str | os.PathLike[str] | None, environment: str) -> str:
    """Return what to load: an absolute path, else KEEL_CAMERA_LIBRARY (absolute; empty is unset), else the SONAME.

    Raises CameraLibraryError for a relative path.
    """
    if path is not None and os.fspath(path):
        candidate, source = os.fspath(path), "keel.camera.load(path)"
    else:
        candidate, source = environment, ENVIRONMENT
    if not candidate:
        return SONAME
    if not os.path.isabs(candidate):
        raise CameraLibraryError(f"{source} must be an absolute path, not {candidate!r}")
    return candidate


def missing(name: str, error: OSError) -> CameraLibraryError:
    """Return the error for a camera library the dynamic loader cannot open, with the remedy."""
    return CameraLibraryError(f"cannot load {name}: {error}. {_REMEDY}")


def check_abi(version: int, path: str) -> None:
    """Raise CameraLibraryError unless version has this binding's camera ABI major and at least its minor."""
    major, minor = version >> 16, version & 0xFFFF
    if major != abi.MAJOR or minor < abi.MINOR:
        binding = ".".join(str(part) for part in _generated.VERSION)
        raise CameraLibraryError(
            f"{path} has camera ABI {major}.{minor}, but this binding was generated from camera.h {binding} and needs "
            f"ABI {abi.MAJOR}.{abi.MINOR} (major {abi.MAJOR}, minor {abi.MINOR} or newer). Install the "
            "libkeel-camera1 package that matches the binding, or the binding that matches the installed library."
        )
