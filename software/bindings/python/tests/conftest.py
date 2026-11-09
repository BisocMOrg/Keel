# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Test configuration: the library under test with its guards, and the fixtures every suite shares.

Every test except the hardware, portability and camera tests runs under the simulator guard of
support.native_calls.NativeCallController; hardware tests run under its motion guard instead. Both sit in the table
of bound foreign functions, so they also cover the calls the control loop makes. The camera tests (tests/camera and
tests/internal/camera) never load libkeel.so, so neither guard applies to them: they carry the camera marker and run
on the synthetic camera of a libkeel-camera test build.
"""

from __future__ import annotations

import gc
import os
from collections.abc import Iterator
from types import ModuleType

import pytest

import keel
from keel._internal import _generated, native_library
from keel._internal.native_library import NativeLibrary
from support.camera_source import ENCODER, FAULT, TEST_SOURCE, require_test_build
from support.native_calls import NativeCallController
from support.simulator import open_sim

CAMERA_SUITES = ("/tests/camera/", "/tests/internal/camera/")
"""The directories whose tests are camera tests."""


def pytest_collection_modifyitems(items: list[pytest.Item]) -> None:
    """Mark every test of the camera suites as a camera test."""
    for item in items:
        path = str(item.path).replace("\\", "/")
        if any(suite in path for suite in CAMERA_SUITES):
            item.add_marker(pytest.mark.camera)


@pytest.fixture(scope="session")
def natives() -> NativeCallController:
    """The loaded library under test, with the guards installed."""
    if not os.environ.get("KEEL_LIBRARY") and os.environ.get("KEEL_TEST_HARDWARE") != "1":
        pytest.fail(
            "set KEEL_LIBRARY to the absolute path of a simulator build of libkeel.so.1 "
            "(cmake --preset sim-only in software/libkeel)"
        )
    return NativeCallController(native_library.get())


@pytest.fixture(autouse=True)
def _guard(request: pytest.FixtureRequest) -> Iterator[None]:
    """Install the simulator guard, or the hardware motion guard; portability and camera tests get neither."""
    if request.node.get_closest_marker("portability") or request.node.get_closest_marker("camera"):
        yield
        return
    controller: NativeCallController = request.getfixturevalue("natives")
    controller.policy = "hardware" if request.node.get_closest_marker("hardware") else "simulator"
    try:
        yield
    finally:
        controller.reset()
        gc.unfreeze()


@pytest.fixture(autouse=True)
def _camera_source(request: pytest.FixtureRequest, monkeypatch: pytest.MonkeyPatch) -> None:
    """Select the synthetic camera with no fault for the camera tests, except the hardware and portability tests."""
    if not request.node.get_closest_marker("camera"):
        return
    if request.node.get_closest_marker("hardware") or request.node.get_closest_marker("portability"):
        return
    require_test_build()
    monkeypatch.setenv(TEST_SOURCE, "1")
    monkeypatch.delenv(FAULT, raising=False)
    monkeypatch.delenv(ENCODER, raising=False)


@pytest.fixture
def sim() -> Iterator[keel.Device]:
    """A virtual-clock simulator device, closed after the test."""
    device = open_sim()
    try:
        yield device
    finally:
        device.close()


@pytest.fixture
def library(natives: NativeCallController) -> NativeLibrary:
    """The loaded library."""
    return natives.library


@pytest.fixture
def structs() -> ModuleType:
    """The generated private layer (structs, conversions, builders and checked call wrappers)."""
    return _generated
