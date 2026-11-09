# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Loading libkeel and reading its metadata: load(), the ABI and build versions, and status and fault-cause names.

Every function here loads the library first if necessary. The keel package re-exports them all.
"""

from __future__ import annotations

import os
from typing import SupportsIndex

from ._internal import native_library
from .errors import StatusClass

__all__ = [
    "abi_version",
    "fault_cause_name",
    "load",
    "status_class",
    "status_name",
    "status_string",
    "version_string",
]


def load(path: str | os.PathLike[str] | None = None) -> None:
    """Load libkeel now instead of on first use.

    Resolution order: path, if given, else the environment variable KEEL_LIBRARY (an empty value counts as unset),
    else the SONAME libkeel.so.1 through the dynamic loader. path and KEEL_LIBRARY must be absolute. After loading,
    all 37 functions must resolve and the library's ABI must have this binding's major and at least its minor.
    Loading happens once per process; later calls return at once.

    Raises LibraryError on anything but Linux, for a relative path, a library that cannot be opened, a missing
    symbol or an incompatible ABI. A failed load is not kept, so a corrected KEEL_LIBRARY works on the next attempt.
    """
    native_library.load(path)


def abi_version() -> tuple[int, int]:
    """Return the (major, minor) ABI of the loaded library (keel_abi_version; any thread)."""
    version = native_library.get().abi_version()
    return version >> 16, version & 0xFFFF


def version_string() -> str:
    """Return the library's build identification, such as "1.0.0 (aarch64-linux-gnu; backends r3,simulator)"."""
    return native_library.get().version_string()


def status_name(code: SupportsIndex) -> str:
    """Return the symbolic name of a status code, such as "KEEL_E_IO"; "KEEL_E_UNKNOWN" for an undefined code."""
    return native_library.get().status_name(code)


def status_string(code: SupportsIndex) -> str:
    """Return the one-sentence description of a status code, naming the remedy where there is one."""
    return native_library.get().status_string(code)


def status_class(code: SupportsIndex) -> StatusClass:
    """Return the class of a status code; libkeel reads an undefined code as StatusClass.FAULT."""
    return native_library.get().status_class(code)


def fault_cause_name(cause: SupportsIndex) -> str:
    """Return the symbolic name of a fault cause, such as "TICK_LATE" (the FaultCause member name); "UNKNOWN"."""
    return native_library.get().fault_cause_name(cause)
