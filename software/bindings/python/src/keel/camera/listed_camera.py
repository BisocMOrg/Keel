# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""ListedCamera: one camera of keel.camera.list_cameras(), with its sensor modes."""

from __future__ import annotations

from dataclasses import dataclass

from ._generated import CameraInfo, SensorMode

__all__ = ["ListedCamera"]


@dataclass(frozen=True, slots=True)
class ListedCamera:
    """One camera that was present when list_cameras() ran: its CameraInfo and its sensor modes."""

    info: CameraInfo
    """The camera's index, model, sensor, full resolution and capabilities."""

    modes: tuple[SensorMode, ...]
    """The sensor's read-out modes, ordered by bit depth, then by size."""

    @property
    def index(self) -> int:
        """The index to pass to keel.camera.open()."""
        return self.info.index

    @property
    def model(self) -> str:
        """The product name, such as "Camera Module 3", or the sensor name for other cameras."""
        return self.info.model

    @property
    def sensor(self) -> str:
        """The sensor name, such as "imx219" or "imx708_wide"."""
        return self.info.sensor

    @property
    def autofocus(self) -> bool:
        """True when the camera has an autofocus lens (Camera Module 3)."""
        return self.info.autofocus
