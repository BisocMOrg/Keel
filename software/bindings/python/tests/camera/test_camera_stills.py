# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""JPEG stills: the hardware encoder up to 1920x1920, libjpeg above it, into memory and into files."""

from __future__ import annotations

from pathlib import Path

import pytest

from keel import camera


def test_a_still_on_the_hardware_encoder(cam: camera.Camera) -> None:
    still = cam.capture_still(width=640, height=480, quality=90)
    assert (still.width, still.height) == (640, 480)
    assert still.info.bytes == len(still.data) == len(bytes(still))
    assert still.data.startswith(b"\xff\xd8")
    assert still.data.endswith(b"\xff\xd9")
    assert not still.info.software
    assert still.info.timestamp_ns > 0


def test_a_full_resolution_still_uses_libjpeg(cam: camera.Camera) -> None:
    if "jpeg" not in camera.version_string():
        with pytest.raises(camera.CameraUnsupportedError):
            cam.capture_still()
        return
    still = cam.capture_still()
    assert (still.width, still.height) == (3280, 2464)
    assert still.info.software
    assert still.info.flags == camera.StillFlag.SOFTWARE
    assert still.data.startswith(b"\xff\xd8")


def test_a_still_into_a_file(cam: camera.Camera, tmp_path: Path) -> None:
    path = tmp_path / "photo.jpg"
    info = cam.capture_still_file(path, width=1920, height=1080)
    assert (info.width, info.height) == (1920, 1080)
    data = path.read_bytes()
    assert len(data) == info.bytes
    assert data.startswith(b"\xff\xd8")
    with pytest.raises(camera.CameraNotFoundError):
        cam.capture_still_file("/nonexistent-directory/photo.jpg", width=640, height=480)


def test_video_returns_after_a_still(cam: camera.Camera) -> None:
    cam.capture_still(width=640, height=480)
    cam.start()
    frame = cam.read_frame()
    assert (frame.width, frame.height) == (320, 240)


def test_stills_need_the_capture_stopped(cam: camera.Camera) -> None:
    cam.start()
    with pytest.raises(camera.CameraStateError):
        cam.capture_still(width=640, height=480)
    with pytest.raises(camera.CameraStateError):
        cam.capture_still_file("/tmp/never.jpg", width=640, height=480)


@pytest.mark.parametrize("options", [{"width": 640}, {"width": 641, "height": 480}, {"quality": 101}])
def test_still_options_are_checked_by_the_library(cam: camera.Camera, options: dict[str, int]) -> None:
    with pytest.raises(camera.CameraArgumentError):
        cam.capture_still(**options)  # type: ignore[arg-type]


def test_a_negative_timeout_is_refused_before_the_call(cam: camera.Camera) -> None:
    with pytest.raises(ValueError, match="timeout must not be negative"):
        cam.capture_still(timeout=-1)


def test_a_still_on_the_autofocus_camera() -> None:
    with camera.open(1, width=320, height=240) as module3:
        still = module3.capture_still(width=1536, height=864, timeout=5)
    assert (still.width, still.height) == (1536, 864)
