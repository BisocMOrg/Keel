# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Fixtures of the camera API suites: an injected fault and an open synthetic camera.

The root conftest marks these tests as camera tests and selects the synthetic camera for them.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator

import pytest

from keel import camera
from support.camera_source import FAULT


@pytest.fixture
def fault(monkeypatch: pytest.MonkeyPatch) -> Callable[[str], None]:
    """Inject one KEEL_CAMERA_TEST_FAULT for the cameras the test opens next."""

    def inject(name: str) -> None:
        monkeypatch.setenv(FAULT, name)

    return inject


@pytest.fixture
def cam() -> Iterator[camera.Camera]:
    """Camera 0 (the synthetic Camera Module 2) at 320x240 I420 and 120 fps, closed after the test."""
    with camera.open(width=320, height=240, framerate=120) as opened:
        yield opened
