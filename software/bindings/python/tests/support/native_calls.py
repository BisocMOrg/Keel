# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""NativeCallController: the test-side control of the bound foreign functions, and the motion guard.

The controller wraps every entry of the loaded library's table of bound foreign functions, below the public API, so
its guards also cover the calls the control loop makes:

- the simulator policy closes a device that keel_open() returns without Capability.SIMULATED at once and fails the
  test, before anything could arm it;
- the hardware policy runs motion_guard before every call, which refuses every call that could move the robot.

The same table lets a test record which foreign calls were made and replace a function with a stand-in.
"""

from __future__ import annotations

import ctypes
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from typing import Any

import pytest

import keel
from keel._internal.native_library import NativeLibrary

MOTION_CALLS = frozenset({"keel_request_arm", "keel_brake", "keel_servo_start"})


class NativeCallController:
    """The test-side controller of the bound foreign functions of the loaded library."""

    def __init__(self, library: NativeLibrary) -> None:
        self.library = library
        self.original = dict(library.natives)
        self.policy = "simulator"
        self.calls: list[str] | None = None
        self.overrides: dict[str, Callable[..., Any]] = {}
        for name, function in self.original.items():
            library.natives[name] = self._wrap(name, function)

    def _wrap(self, name: str, function: Callable[..., Any]) -> Callable[..., Any]:
        def call(*args: Any) -> Any:
            if self.calls is not None:
                self.calls.append(name)
            if self.policy == "hardware":
                motion_guard(name, args)
            result = self.overrides.get(name, function)(*args)
            if name == "keel_open" and result == 0 and self.policy == "simulator":
                self._require_simulator(args[1]._obj.value)
            return result

        return call

    def _require_simulator(self, token: int) -> None:
        capabilities = ctypes.c_uint32()
        self.original["keel_get_capabilities"](token, ctypes.byref(capabilities))
        if not capabilities.value & keel.Capability.SIMULATED:
            self.original["keel_close"](token, None)
            pytest.fail("keel_open returned a device that is not the simulator; it was closed at once")

    @contextmanager
    def record(self) -> Iterator[list[str]]:
        """Record the names of the foreign calls made inside the block."""
        calls: list[str] = []
        self.calls = calls
        try:
            yield calls
        finally:
            self.calls = None

    def reset(self) -> None:
        self.calls = None
        self.overrides.clear()
        self.policy = "simulator"


def motion_guard(name: str, args: tuple[Any, ...]) -> None:
    """Fail the test before a call that could move the robot."""
    moving = name in MOTION_CALLS
    if name == "keel_set_motors":
        moving = args[1] != 0 or args[2] != 0
    elif name == "keel_cycle":
        cycle_input = args[1]._obj
        moving = cycle_input.command == keel.CycleCommand.BRAKE or (
            cycle_input.command == keel.CycleCommand.DRIVE and (cycle_input.left or cycle_input.right)
        )
    if moving:
        pytest.fail(f"the motion guard refused {name}: hardware tests never move the robot")
