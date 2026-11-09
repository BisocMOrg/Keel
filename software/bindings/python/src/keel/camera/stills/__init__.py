# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""JPEG stills up to the full sensor resolution: StillImage, its StillInfo and their flags."""

from ._generated import StillFlag, StillInfo
from .still_image import StillImage

__all__ = ["StillFlag", "StillImage", "StillInfo"]
