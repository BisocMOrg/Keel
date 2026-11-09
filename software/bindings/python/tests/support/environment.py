# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The test environment: the binding's directory, the libkeel source tree and child Python processes."""

from __future__ import annotations

import os
import re
from pathlib import Path

import keel

PYTHON = Path(__file__).resolve().parent.parent.parent
"""The software/bindings/python directory."""


def libkeel_source() -> Path | None:
    """The software/libkeel directory: KEEL_SOURCE_DIR, else the repository around this binding, else None."""
    configured = os.environ.get("KEEL_SOURCE_DIR")
    candidate = Path(configured) if configured else PYTHON.parent.parent / "libkeel"
    return candidate if (candidate / "include" / "keel" / "keel.h").is_file() else None


def subprocess_env(**extra: str) -> dict[str, str]:
    """The environment for a child Python process that imports the keel package under test."""
    environment = dict(os.environ)
    package_root = str(Path(keel.__file__).resolve().parent.parent)
    environment["PYTHONPATH"] = os.pathsep.join(filter(None, [package_root, environment.get("PYTHONPATH")]))
    environment.update(extra)
    return environment


def backends(version: str) -> list[str]:
    """The backends listed in a keel_version_string(), such as ["r3", "simulator"]."""
    match = re.search(r"backends ([\w,]+)\)", version)
    return match.group(1).split(",") if match else []
