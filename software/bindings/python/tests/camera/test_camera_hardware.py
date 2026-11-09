# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Robot stage: the binding against the robot's real camera, through the installed libkeel-camera.

Run on the robot only, with the libkeel-camera1 package installed, KEEL_CAMERA_LIBRARY and KEEL_CAMERA_TEST_SOURCE
unset and KEEL_TEST_HARDWARE=1:

    KEEL_TEST_HARDWARE=1 python3 -m pytest -m hardware -p no:cacheprovider tests/camera

The tests skip when no camera is connected. The camera is not part of the safety function, and nothing here touches
the Keel board.
"""

from __future__ import annotations

import os
import socket
import time
from collections.abc import Iterator
from pathlib import Path

import pytest

from keel import camera
from support.camera_source import free_port, threads

pytestmark = [
    pytest.mark.hardware,
    pytest.mark.skipif(os.environ.get("KEEL_TEST_HARDWARE") != "1", reason="robot only (set KEEL_TEST_HARDWARE=1)"),
]


@pytest.fixture(scope="module")
def listed() -> camera.ListedCamera:
    if os.environ.get("KEEL_CAMERA_LIBRARY") or os.environ.get("KEEL_CAMERA_TEST_SOURCE"):
        pytest.fail("unset KEEL_CAMERA_LIBRARY and KEEL_CAMERA_TEST_SOURCE: the robot stage tests the real camera")
    cameras = camera.list_cameras()
    if not cameras:
        pytest.skip("no camera is connected")
    return cameras[0]


@pytest.fixture
def cam(listed: camera.ListedCamera) -> Iterator[camera.Camera]:
    with camera.open(listed.index, width=640, height=480, pixel_format=camera.PixelFormat.BGR888) as opened:
        yield opened


def test_the_installed_library_and_the_listing(listed: camera.ListedCamera) -> None:
    version = camera.version_string()
    assert "test-hooks" not in version
    assert camera.abi_version() == (1, 0)
    assert listed.model.startswith("Camera Module")
    assert listed.sensor.startswith(("imx219", "imx708"))
    assert listed.autofocus == listed.sensor.startswith("imx708")
    assert listed.modes
    assert all(mode.max_framerate > 0 for mode in listed.modes)


def test_frames_carry_an_image(cam: camera.Camera) -> None:
    cam.start()
    frames = [cam.read_frame(timeout=3) for _ in range(10)]
    image = frames[-1].array()
    assert image.shape == (480, 640, 3)
    assert int(image.max()) > int(image.min())  # not a constant picture
    assert [frame.sequence for frame in frames] == sorted(frame.sequence for frame in frames)


def test_i420_frames_and_the_cpu_conversion(listed: camera.ListedCamera) -> None:
    with camera.open(listed.index, width=640, height=480) as cam:
        cam.start()
        frame = cam.read_frame(timeout=3)
    assert frame.plane(0).shape == (480, 640)
    rgb = frame.convert(camera.PixelFormat.RGB888)
    assert rgb.array().shape == (480, 640, 3)


def test_h264_starts_with_a_keyframe_within_two_seconds(listed: camera.ListedCamera) -> None:
    with camera.open(listed.index, width=640, height=480) as cam:
        cam.set_encoder(camera.Codec.H264)
        cam.start()
        started = time.monotonic()
        chunk = cam.read_chunk(timeout=2)
        assert time.monotonic() - started < 2
        assert chunk.keyframe
        assert chunk.info.headers
        assert chunk.data.startswith(b"\x00\x00\x00\x01")


def test_a_listening_port_streams_to_a_receiver(listed: camera.ListedCamera) -> None:
    port = free_port()
    with camera.open(listed.index, width=640, height=480) as cam:
        cam.set_encoder(camera.Codec.H264)
        output = cam.start_output(f"tcp://127.0.0.1:{port}?listen")
        cam.start()
        with socket.create_connection(("127.0.0.1", port), timeout=5) as receiver:
            data = b""
            while len(data) < 20000:
                data += receiver.recv(65536)
        assert data[4] & 0x1F == 7
        assert cam.output_stats(output).chunks > 0


def test_mjpeg_and_a_hardware_still(listed: camera.ListedCamera, tmp_path: Path) -> None:
    with camera.open(listed.index, width=1280, height=720) as cam:
        cam.set_encoder(camera.Codec.MJPEG)
        cam.start()
        chunk = cam.read_chunk(timeout=3)
        assert chunk.data.startswith(b"\xff\xd8")
        cam.stop()
        still = cam.capture_still(width=1920, height=1080)
        assert (still.width, still.height) == (1920, 1080)
        assert not still.info.software
        info = cam.capture_still_file(tmp_path / "photo.jpg", width=1000, height=750)
        assert (tmp_path / "photo.jpg").read_bytes()[:2] == b"\xff\xd8"
        assert info.width == 1000


def test_fixed_focus_cameras_refuse_focus_options(listed: camera.ListedCamera) -> None:
    if listed.autofocus:
        with camera.open(listed.index, af_mode=camera.AutofocusMode.AUTO) as cam:
            cam.start()
            assert cam.trigger_autofocus(timeout=3) in (camera.AutofocusState.FOCUSED, camera.AutofocusState.FAILED)
    else:
        with pytest.raises(camera.CameraUnsupportedError, match="fixed-focus"):
            camera.open(listed.index, af_mode=camera.AutofocusMode.CONTINUOUS)


def test_closing_leaves_no_library_thread(listed: camera.ListedCamera) -> None:
    before = threads()
    with camera.open(listed.index, width=640, height=480) as cam:
        cam.set_encoder(camera.Codec.H264)
        cam.start()
        cam.read_chunk(timeout=3)
    deadline = time.monotonic() + 3
    while threads() > before and time.monotonic() < deadline:
        time.sleep(0.05)
    assert threads() == before
