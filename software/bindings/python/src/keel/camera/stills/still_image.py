# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""StillImage: one JPEG still from Camera.capture_still()."""

from __future__ import annotations

from dataclasses import dataclass

from ._generated import StillInfo

__all__ = ["StillImage"]


@dataclass(frozen=True, slots=True)
class StillImage:
    """One JPEG image and its StillInfo (size, encoder and timestamp)."""

    info: StillInfo
    """The size, flags and timestamp the still capture wrote."""

    data: bytes
    """The JPEG file contents (info.bytes of them): write them to a .jpg file, or decode them with PIL or OpenCV."""

    def __bytes__(self) -> bytes:
        """Return the JPEG file contents."""
        return self.data

    @property
    def width(self) -> int:
        """Width in pixels."""
        return self.info.width

    @property
    def height(self) -> int:
        """Height in lines."""
        return self.info.height
