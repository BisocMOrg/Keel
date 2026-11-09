# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Immutable limits and native-open options for one sequential task."""

from __future__ import annotations

import operator
from collections.abc import Mapping
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any

from keel._generated import OpenFlag
from keel.camera.frames import PixelFormat

_MAX_MS = 86_400_000


def _milliseconds(name: str, value: object, *, allow_zero: bool = False) -> int:
    """Validate an integer duration in milliseconds, at most one day."""
    if isinstance(value, bool):
        raise TypeError(f"{name} must be integer milliseconds, not bool")
    try:
        result = operator.index(value)  # type: ignore[arg-type]
    except TypeError:
        raise TypeError(f"{name} must be integer milliseconds") from None
    minimum = 0 if allow_zero else 1
    if not minimum <= result <= _MAX_MS:
        raise ValueError(f"{name} must be {minimum}..{_MAX_MS} milliseconds")
    return result


@dataclass(frozen=True)
class TaskOptions:
    """Set the finite limits and native open options for a task.

    All durations are integer milliseconds, from 1 to 86,400,000. max_motion_ms
    limits each drive or turn; arm_timeout_ms bounds automatic arming;
    task_timeout_ms bounds the complete task from runtime startup; and
    camera_timeout_ms bounds each frame read. Deadlines use a monotonic clock.

    board_kwargs are passed to keel.open(), and camera_kwargs to
    keel.camera.open(). Copies are retained so later mapping changes cannot
    change an executing task. Virtual-clock boards are rejected: task code,
    ordinary sleep(), and deadlines all use real time. Camera frames always use
    BGR888, suitable for OpenCV; omit pixel_format or explicitly choose BGR888.

    A task deadline removes hardware authority even if user code has not
    returned. Python cannot safely terminate arbitrary user code: an infinite
    computation can continue, but later task operations will be rejected.
    """

    max_motion_ms: int = 30_000
    """Maximum duration of one drive or turn, in integer milliseconds."""
    arm_timeout_ms: int = 3_000
    """Maximum automatic-arming and controller-start wait, in milliseconds."""
    task_timeout_ms: int = 300_000
    """Total task lifetime before hardware authority is revoked, in milliseconds."""
    camera_timeout_ms: int = 1_000
    """Maximum wait for each captured frame, in milliseconds."""
    board_kwargs: Mapping[str, Any] = field(default_factory=dict)
    """Keyword options for keel.open(); defaults select the normal board backend."""
    camera_kwargs: Mapping[str, Any] = field(default_factory=dict)
    """Keyword options for keel.camera.open(); pixel_format is always BGR888."""

    def __post_init__(self) -> None:
        """Validate limits and freeze copies of both native option mappings."""
        for name in ("max_motion_ms", "arm_timeout_ms", "task_timeout_ms", "camera_timeout_ms"):
            object.__setattr__(self, name, _milliseconds(name, getattr(self, name)))
        board = dict(self.board_kwargs)
        if operator.index(board.get("flags", 0)) & OpenFlag.VIRTUAL_CLOCK:
            raise ValueError("KeelTask requires a real-time board; VIRTUAL_CLOCK is not supported")
        camera = dict(self.camera_kwargs)
        if operator.index(camera.get("pixel_format", PixelFormat.BGR888)) != PixelFormat.BGR888:
            raise ValueError("KeelTask.take_image requires the BGR888 pixel format")
        camera["pixel_format"] = PixelFormat.BGR888
        object.__setattr__(self, "board_kwargs", MappingProxyType(board))
        object.__setattr__(self, "camera_kwargs", MappingProxyType(camera))
