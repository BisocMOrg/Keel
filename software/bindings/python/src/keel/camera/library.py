# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Loading libkeel-camera and reading its metadata: load(), and the ABI and build versions.

Every function here loads the camera library first if necessary; keel.camera re-exports them all. The camera library
is independent of libkeel.so: loading one never loads the other.
"""

from __future__ import annotations

import os

from keel._internal.camera import native_library

__all__ = ["abi_version", "load", "version_string"]


def load(path: str | os.PathLike[str] | None = None) -> None:
    """Load libkeel-camera before the first camera call, which would otherwise load it.

    Resolution order: path, if given, else the environment variable KEEL_CAMERA_LIBRARY (an empty value counts as
    unset), else the SONAME libkeel-camera.so.1 through the dynamic loader. path and KEEL_CAMERA_LIBRARY must be
    absolute. After loading, all 25 functions must resolve and the library's ABI must have this binding's major and
    at least its minor. Loading happens once per process; later calls return at once.

    Raises CameraLibraryError (a keel.LibraryError) on anything but Linux, for a relative path, a library that cannot
    be opened (the libkeel-camera1 package is not installed), a missing symbol or an incompatible ABI. A failed load
    is not kept, so a corrected KEEL_CAMERA_LIBRARY works on the next attempt.
    """
    native_library.load(path)


def abi_version() -> tuple[int, int]:
    """Return the (major, minor) ABI of the loaded camera library (keel_camera_abi_version; any thread)."""
    version = native_library.get().abi_version()
    return version >> 16, version & 0xFFFF


def version_string() -> str:
    """Return the camera library's build identification, such as "1.0.0 (aarch64-linux-gnu; libcamera v0.7.0; jpeg)".

    It names the libcamera it was built against and the optional features built in: "jpeg" (libjpeg, for stills
    above 1920x1920), "libav" (FFmpeg's libavformat, for container files and streaming protocols) and "test-hooks".
    """
    return native_library.get().version_string()
