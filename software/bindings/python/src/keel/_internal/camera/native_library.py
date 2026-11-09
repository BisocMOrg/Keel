# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""NativeCameraLibrary: the loaded libkeel-camera, its bound functions, and the process-wide load.

The library is loaded once, on the first camera call or by keel.camera.load(), under a lock, and independently of
libkeel.so: a program that only uses the camera never loads the board library, and one that never uses the camera
never loads this one. A successful load is published in a module attribute that later calls read without the lock;
a failed load is not kept, so a corrected KEEL_CAMERA_LIBRARY takes effect on the next attempt.
"""

from __future__ import annotations

import ctypes
import os
import threading

from keel._internal.string_reader import read_string
from keel.camera.errors.exceptions import CameraLibraryError

from . import _generated, library_loader
from ._generated import Natives


class NativeCameraLibrary:
    """A loaded libkeel-camera: its path, the bound functions and the cached version string."""

    __slots__ = ("_version", "natives", "path")

    def __init__(self, path: str, natives: Natives) -> None:
        """Wrap the bound functions of the library loaded from path."""
        self.path = path
        self.natives = natives
        self._version: str | None = None

    def abi_version(self) -> int:
        """Return keel_camera_abi_version(): (major << 16) | minor of the loaded library."""
        return _generated.keel_camera_abi_version(self.natives)

    def version_string(self) -> str:
        """Return keel_camera_version_string(), read once."""
        if self._version is None:
            address = _generated.keel_camera_version_string(self.natives)
            self._version = read_string(address, "keel_camera_version_string")
        return self._version


_lock = threading.Lock()
_current: NativeCameraLibrary | None = None


def get() -> NativeCameraLibrary:
    """Return the loaded camera library, loading it first if necessary."""
    library = _current
    if library is not None:
        return library
    return load()


def load(path: str | os.PathLike[str] | None = None) -> NativeCameraLibrary:
    """Load libkeel-camera once and return it; see keel.camera.load()."""
    global _current
    with _lock:
        if _current is not None:
            if path is not None and os.fspath(path) and os.fspath(path) != _current.path:
                raise CameraLibraryError(
                    f"libkeel-camera is already loaded from {_current.path}; a process loads one camera library only"
                )
            return _current
        library_loader.check_platform(library_loader.PLATFORM)
        name = library_loader.resolve(path, os.environ.get(library_loader.ENVIRONMENT, ""))
        library = bind(name)
        library_loader.check_abi(library.abi_version(), library.path)
        _current = library
        return library


def bind(name: str) -> NativeCameraLibrary:
    """Open the library with ctypes.CDLL and bind all 25 functions with their prototypes."""
    try:
        handle = ctypes.CDLL(name)
    except OSError as error:
        raise library_loader.missing(name, error) from error
    natives: Natives = {}
    for function, (restype, argtypes) in _generated.PROTOTYPES.items():
        try:
            pointer = handle[function]
        except AttributeError as error:
            raise CameraLibraryError(f"{name} does not export {function}; it is not libkeel-camera 1.x") from error
        pointer.restype = restype
        pointer.argtypes = argtypes
        natives[function] = pointer
    return NativeCameraLibrary(name, natives)
