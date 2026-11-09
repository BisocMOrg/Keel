# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Reading the static strings libkeel returns, never past their NUL nor past KEEL_STRING_MAX bytes."""

from __future__ import annotations

import ctypes

from keel import limits
from keel.errors.exceptions import LibraryError


def read_string(address: int | None, function: str) -> str:
    """Read a static string that libkeel returned, never past its NUL nor past KEEL_STRING_MAX bytes.

    The scan reads one byte at a time through a bounded view, so it needs no libc and never reads memory beyond the
    terminator. A NULL pointer or a missing NUL breaks the contract of keel.h and raises LibraryError.
    """
    if not address:
        raise LibraryError(f"{function} returned NULL, which keel.h rules out")
    view = (ctypes.c_ubyte * limits.STRING_MAX).from_address(address)
    for length in range(limits.STRING_MAX):
        if view[length] == 0:
            return ctypes.string_at(address, length).decode("utf-8", "replace")
    raise LibraryError(f"{function} returned a string with no NUL within KEEL_STRING_MAX ({limits.STRING_MAX}) bytes")
