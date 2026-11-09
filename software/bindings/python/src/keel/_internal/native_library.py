# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""NativeLibrary: the loaded libkeel, its bound functions and cached strings, and the process-wide load.

The library is loaded once, on first use or by keel.load(), under a lock. A successful load is published in a module
attribute that every later call reads without the lock; a failed load is not kept, so a corrected KEEL_LIBRARY takes
effect on the next attempt. Every foreign call goes through the NativeLibrary.natives table.
"""

from __future__ import annotations

import ctypes
import os
import threading
from typing import SupportsIndex

from keel import abi
from keel.errors._generated import StatusClass
from keel.errors.exceptions import LibraryError

from . import _generated, library_loader
from ._generated import Natives
from .string_reader import read_string


class NativeLibrary:
    """A loaded libkeel: its path, the bound functions and the cached static strings."""

    __slots__ = ("_causes", "_classes", "_names", "_strings", "natives", "path")

    def __init__(self, path: str, natives: Natives) -> None:
        """Wrap the bound functions of the library loaded from path."""
        self.path = path
        self.natives = natives
        self._names: dict[int, str] = {}
        self._strings: dict[int, str] = {}
        self._causes: dict[int, str] = {}
        self._classes: dict[int, StatusClass] = {}

    def abi_version(self) -> int:
        """Return keel_abi_version(): (major << 16) | minor of the loaded library."""
        return _generated.keel_abi_version(self.natives)

    def version_string(self) -> str:
        """Return keel_version_string()."""
        return read_string(_generated.keel_version_string(self.natives), "keel_version_string")

    def status_name(self, code: SupportsIndex) -> str:
        """Return keel_status_name(code), cached per code."""
        key = _generated.check_i32("keel_status_name", "status", code)
        name = self._names.get(key)
        if name is None:
            name = read_string(_generated.keel_status_name(self.natives, key), "keel_status_name")
            self._names[key] = name
        return name

    def status_string(self, code: SupportsIndex) -> str:
        """Return keel_status_string(code), cached per code."""
        key = _generated.check_i32("keel_status_string", "status", code)
        text = self._strings.get(key)
        if text is None:
            text = read_string(_generated.keel_status_string(self.natives, key), "keel_status_string")
            self._strings[key] = text
        return text

    def status_class(self, code: SupportsIndex) -> StatusClass:
        """Return keel_status_class(code), cached per code."""
        key = _generated.check_i32("keel_status_class", "status", code)
        value = self._classes.get(key)
        if value is None:
            value = StatusClass(_generated.keel_status_class(self.natives, key))
            self._classes[key] = value
        return value

    def fault_cause_name(self, cause: SupportsIndex) -> str:
        """Return keel_fault_cause_name(cause), cached per cause."""
        key = _generated.check_u32("keel_fault_cause_name", "cause", cause)
        name = self._causes.get(key)
        if name is None:
            name = read_string(_generated.keel_fault_cause_name(self.natives, key), "keel_fault_cause_name")
            self._causes[key] = name
        return name


_lock = threading.Lock()
_current: NativeLibrary | None = None


def get() -> NativeLibrary:
    """Return the loaded library, loading it first if necessary."""
    library = _current
    if library is not None:
        return library
    return load()


def load(path: str | os.PathLike[str] | None = None) -> NativeLibrary:
    """Load libkeel once and return it; see keel.load()."""
    global _current
    with _lock:
        if _current is not None:
            if path is not None and os.fspath(path) and os.fspath(path) != _current.path:
                raise LibraryError(f"libkeel is already loaded from {_current.path}; a process loads one library only")
            return _current
        library_loader.check_platform(library_loader.PLATFORM)
        name = library_loader.resolve(path, os.environ.get("KEEL_LIBRARY", ""))
        library = bind(name)
        library_loader.check_abi(library.abi_version(), library.path)
        _current = library
        return library


def bind(name: str) -> NativeLibrary:
    """Open the library with ctypes.CDLL and bind all 37 functions with their prototypes."""
    try:
        handle = ctypes.CDLL(name)
    except OSError as error:
        raise LibraryError(
            f"cannot load {name}: {error}. On the robot, install the libkeel1 package; elsewhere, build libkeel "
            "(cmake --preset sim-only) and set KEEL_LIBRARY to the absolute path of libkeel.so.1"
        ) from error
    natives: Natives = {}
    for function, (restype, argtypes) in _generated.PROTOTYPES.items():
        try:
            pointer = handle[function]
        except AttributeError as error:
            raise LibraryError(f"{name} does not export {function}; it is not libkeel {abi.MAJOR}.x") from error
        pointer.restype = restype
        pointer.argtypes = argtypes
        natives[function] = pointer
    return NativeLibrary(name, natives)
