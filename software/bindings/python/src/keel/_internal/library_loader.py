# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Finding and checking libkeel: the platform check, the resolution order and the ABI check."""

from __future__ import annotations

import os
import sys

from keel import abi
from keel.errors.exceptions import LibraryError

from . import _generated

SONAME = "libkeel.so.1"
"""The shared library's SONAME, resolved by the dynamic loader when KEEL_LIBRARY is not set."""

PLATFORM = sys.platform
"""The platform the load check tests. libkeel and its simulator are Linux-only."""

_PINNING = (
    "Use the keel-robot release whose X.Y equals the installed libkeel1 "
    '(dpkg-query -W libkeel1, then pip install "keel-robot==X.Y.*"), or upgrade libkeel1.'
)


def check_platform(platform: str) -> None:
    """Raise LibraryError unless platform is Linux."""
    if not platform.startswith("linux"):
        raise LibraryError(f"libkeel requires Linux; found {platform}")


def resolve(path: str | os.PathLike[str] | None, environment: str) -> str:
    """Return what to load: an explicit absolute path, else KEEL_LIBRARY (absolute; empty is unset), else the SONAME.

    Raises LibraryError for a relative path.
    """
    if path is not None and os.fspath(path):
        candidate, source = os.fspath(path), "keel.load(path)"
    else:
        candidate, source = environment, "KEEL_LIBRARY"
    if not candidate:
        return SONAME
    if not os.path.isabs(candidate):
        raise LibraryError(f"{source} must be an absolute path, not {candidate!r}")
    return candidate


def check_abi(version: int, path: str) -> None:
    """Raise LibraryError unless version has this binding's ABI major and at least its minor.

    A binding generated from a header with minor Y may use every function and field of minor Y, so it requires
    minor Y as a whole.
    """
    major, minor = version >> 16, version & 0xFFFF
    if major != abi.MAJOR or minor < abi.MINOR:
        binding = ".".join(str(part) for part in _generated.VERSION)
        raise LibraryError(
            f"{path} has ABI {major}.{minor}, but keel-robot {binding} needs ABI {abi.MAJOR}.{abi.MINOR} "
            f"(major {abi.MAJOR}, minor {abi.MINOR} or newer). {_PINNING}"
        )
