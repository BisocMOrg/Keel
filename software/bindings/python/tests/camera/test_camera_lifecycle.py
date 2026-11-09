# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Lifetime and threads: close from any thread, the owning thread, the finalizer, interpreter exit and fork."""

from __future__ import annotations

import gc
import os
import socket
import subprocess
import sys
import threading
import time
import warnings
from collections.abc import Callable
from pathlib import Path

import pytest

from keel import camera
from keel.errors import Status
from support.camera_source import open_fds, threads
from support.environment import subprocess_env


def test_a_with_block_closes_the_camera() -> None:
    with camera.open(width=160, height=120) as cam:
        assert not cam.closed
        assert repr(cam) == "<keel.camera.Camera 0: Camera Module 2 (imx219), open>"
    assert cam.closed
    assert repr(cam) == "<keel.camera.Camera 0: Camera Module 2 (imx219), closed>"
    with camera.open(width=160, height=120):  # released
        pass


def test_close_is_idempotent_and_later_calls_raise_closed() -> None:
    cam = camera.open(width=160, height=120)
    cam.close()
    cam.close()
    for call in (cam.start, cam.stop, cam.read_frame, cam.stats, cam.stream_info, cam.last_error):
        with pytest.raises(camera.CameraClosedError, match="the camera is closed") as caught:
            call()
        assert caught.value.status == Status.E_INVALID_HANDLE
        assert caught.value.report is None
    assert list(cam.frames()) == []
    assert list(cam.chunks()) == []


def test_cameras_come_from_open() -> None:
    with pytest.raises(TypeError, match=r"use keel.camera.open\(\)"):
        camera.Camera()


def test_another_thread_may_only_close(cam: camera.Camera) -> None:
    cam.start()
    errors: list[BaseException] = []

    def foreign() -> None:
        try:
            cam.read_frame()
        except camera.CameraError as error:
            errors.append(error)

    worker = threading.Thread(target=foreign)
    worker.start()
    worker.join()
    (error,) = errors
    assert isinstance(error, camera.CameraThreadError)
    assert error.status == Status.E_CONFLICT
    assert error.report is None
    assert "only the thread that opened the camera" in str(error)
    assert cam.read_frame().width == 320  # the owner is unaffected


def test_close_from_another_thread_wakes_a_blocked_read(fault: Callable[[str], None]) -> None:
    fault("stall")
    cam = camera.open(width=160, height=120, framerate=120)
    cam.start()
    for _ in range(3):
        cam.read_frame()
    closer = threading.Timer(0.3, cam.close)
    closer.start()
    started = time.monotonic()
    with pytest.raises(camera.CameraClosedError):
        cam.read_frame(timeout=10)
    assert time.monotonic() - started < 5
    closer.join()
    assert cam.closed


def test_close_from_another_thread_ends_the_frame_loop(fault: Callable[[str], None]) -> None:
    fault("stall")
    cam = camera.open(width=160, height=120, framerate=120)
    cam.start()
    threading.Timer(0.3, cam.close).start()
    assert len(list(cam.frames(timeout=10))) == 3


def test_concurrent_closes_all_return_after_the_close() -> None:
    cam = camera.open(width=160, height=120, framerate=120)
    cam.start()
    barrier = threading.Barrier(4)

    def close() -> None:
        barrier.wait()
        cam.close()

    workers = [threading.Thread(target=close) for _ in range(4)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    with camera.open(width=160, height=120):  # every closer saw the camera released
        pass


def test_a_dropped_camera_is_closed_by_its_finalizer(caplog: pytest.LogCaptureFixture) -> None:
    held = [camera.open(width=160, height=120, framerate=120)]
    held[0].start()
    with pytest.warns(ResourceWarning, match="keel.camera.Camera was not closed"):
        held.clear()  # the last reference: the finalizer runs inside this block
    assert "its finalizer closed it" in caplog.text
    with camera.open(width=160, height=120):  # released
        pass


def test_interpreter_exit_closes_an_open_camera(tmp_path: Path) -> None:
    path = tmp_path / "exit.h264"
    code = (
        "import logging, sys, time\n"
        "from keel import camera\n"
        "logging.basicConfig(level=logging.WARNING)\n"
        "cam = camera.open(width=160, height=120, framerate=120)\n"
        "cam.set_encoder(camera.Codec.H264)\n"
        f"cam.start_output({str(path)!r})\n"
        "cam.start()\n"
        "time.sleep(0.3)\n"
        "sys.exit(7)\n"
    )
    result = subprocess.run(
        [sys.executable, "-c", code], capture_output=True, text=True, env=subprocess_env(), check=False, timeout=60
    )
    assert result.returncode == 7, result.stderr
    assert "keel.camera.Camera was not closed; its finalizer closed it" in result.stderr
    assert path.read_bytes().startswith(b"\x00\x00\x00\x01")


def test_a_forked_child_cannot_use_or_close_the_parents_camera(cam: camera.Camera) -> None:
    cam.start()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)  # fork() in a multi-threaded process
        pid = os.fork()
    if pid == 0:  # pragma: no cover  (the child reports through its exit status)
        status = 1
        try:
            cam.read_frame()
        except camera.CameraClosedError as error:
            status = 0 if error.status == Status.E_NOT_OWNER else 2
        cam.close()
        os._exit(status)
    _, status = os.waitpid(pid, 0)
    assert os.waitstatus_to_exitcode(status) == 0
    assert cam.read_frame().width == 320  # the parent's camera is untouched


def test_reopening_leaves_no_thread_or_descriptor_behind(tmp_path: Path) -> None:
    with camera.open(width=160, height=120, framerate=120) as warm:
        warm.set_encoder(camera.Codec.H264)
        warm.start()
        warm.read_chunk()
    gc.collect()
    descriptors, tasks = open_fds(), threads()
    for number in range(20):
        with camera.open(width=160, height=120, framerate=120) as cam:
            cam.set_encoder(camera.Codec.H264)
            cam.start_output(tmp_path / f"{number}.h264")
            cam.start()
            cam.read_frame()
    gc.collect()
    assert open_fds() == descriptors
    assert threads() == tasks


def test_a_failed_close_is_logged_not_raised(caplog: pytest.LogCaptureFixture) -> None:
    cam = camera.open(width=160, height=120, framerate=120)
    cam.set_encoder(camera.Codec.H264)
    with socket.create_server(("127.0.0.1", 0)) as server:
        output = cam.start_output(f"tcp://127.0.0.1:{server.getsockname()[1]}")
        connection, _ = server.accept()
        cam.start()
        connection.recv(100)
        connection.close()
    deadline = time.monotonic() + 3
    while cam.output_stats(output).state != camera.OutputState.FAILED and time.monotonic() < deadline:
        time.sleep(0.02)
    cam.close()
    assert "keel_camera_close: KEEL_E_PERIPHERAL" in caplog.text
