# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Outputs the library writes itself: files, a TCP connection, a listening port, UDP, and the refusals."""

from __future__ import annotations

import socket
import time
from collections.abc import Iterator
from pathlib import Path

import pytest

from keel import camera
from keel.errors import Status
from support.camera_source import free_port


@pytest.fixture
def h264() -> Iterator[camera.Camera]:
    with camera.open(width=320, height=240, framerate=120) as cam:
        cam.set_encoder(camera.Codec.H264)
        yield cam


def receive(connection: socket.socket, size: int, timeout: float = 3.0) -> bytes:
    """At least size bytes from a stream socket, or what arrived before the timeout."""
    connection.settimeout(timeout)
    data = b""
    while len(data) < size:
        piece = connection.recv(65536)
        if not piece:
            break
        data += piece
    return data


def wait_for(condition: object, seconds: float = 3.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if callable(condition) and condition():
            return True
        time.sleep(0.02)
    return False


def test_an_h264_file(h264: camera.Camera, tmp_path: Path) -> None:
    path = tmp_path / "drive.h264"
    output = h264.start_output(path)
    stats = h264.output_stats(output)
    assert stats.state == camera.OutputState.WAITING
    h264.start()
    assert wait_for(lambda: h264.output_stats(output).chunks >= 5)
    h264.stop_output(output)
    data = path.read_bytes()
    assert data.startswith(b"\x00\x00\x00\x01")
    assert data[4] & 0x1F == 7  # the file starts at a keyframe with its parameter sets
    assert h264.stats().outputs == 0
    with pytest.raises(camera.CameraNotFoundError):
        h264.output_stats(output)
    with pytest.raises(camera.CameraNotFoundError):
        h264.stop_output(output)


def test_an_mjpeg_file(tmp_path: Path) -> None:
    path = tmp_path / "drive.mjpeg"
    with camera.open(width=320, height=240, framerate=120) as cam:
        cam.set_encoder(camera.Codec.MJPEG)
        cam.start_output(str(path))
        cam.start()
        time.sleep(0.2)
    data = path.read_bytes()  # close finished the file
    assert data.startswith(b"\xff\xd8")
    assert data.endswith(b"\xff\xd9")
    assert data.count(b"\xff\xd8") > 3


def test_a_tcp_connection_to_a_receiver(h264: camera.Camera) -> None:
    with socket.create_server(("127.0.0.1", 0)) as server:
        port = server.getsockname()[1]
        output = h264.start_output(f"tcp://127.0.0.1:{port}")
        connection, _ = server.accept()
        with connection:
            h264.start()
            data = receive(connection, 2000)
            assert data.startswith(b"\x00\x00\x00\x01")
            stats = h264.output_stats(output)
            assert stats.clients == 1
            assert stats.bytes > 0
            assert stats.status == Status.OK


def test_a_listening_port_serves_receivers_from_a_keyframe(h264: camera.Camera) -> None:
    port = free_port()
    output = h264.start_output(f"tcp://127.0.0.1:{port}?listen", max_clients=2)
    assert h264.output_stats(output).clients == 0
    h264.start()
    time.sleep(0.2)
    with socket.create_connection(("127.0.0.1", port)) as first, socket.create_connection(("127.0.0.1", port)) as other:
        for receiver in (first, other):
            data = receive(receiver, 2000)
            assert data[4] & 0x1F == 7
        assert wait_for(lambda: h264.output_stats(output).clients == 2)
        assert h264.output_stats(output).state == camera.OutputState.RUNNING


def test_udp_datagrams(h264: camera.Camera) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(3)
        port = receiver.getsockname()[1]
        output = h264.start_output(f"udp://127.0.0.1:{port}", datagram_bytes=1000)
        h264.start()
        datagram = receiver.recv(65536)
        assert 0 < len(datagram) <= 1000
        assert datagram.startswith(b"\x00\x00\x00\x01")
        assert wait_for(lambda: h264.output_stats(output).chunks > 0)


def test_a_receiver_that_goes_away_fails_only_its_output(h264: camera.Camera) -> None:
    with socket.create_server(("127.0.0.1", 0)) as server:
        output = h264.start_output(f"tcp://127.0.0.1:{server.getsockname()[1]}")
        connection, _ = server.accept()
        h264.start()
        receive(connection, 500)
        connection.close()
    assert wait_for(lambda: h264.output_stats(output).state == camera.OutputState.FAILED)
    stats = h264.output_stats(output)
    assert stats.status == Status.E_PERIPHERAL
    assert "disconnected" in stats.message
    assert h264.stats().state == camera.CaptureState.RUNNING
    h264.read_chunk()
    with pytest.raises(camera.CameraError, match="disconnected") as caught:
        h264.stop_output(output)
    assert caught.value.status == Status.E_PERIPHERAL


def test_a_receiver_that_refuses(h264: camera.Camera) -> None:
    with pytest.raises(camera.CameraNotFoundError, match="refused") as caught:
        h264.start_output(f"tcp://127.0.0.1:{free_port()}")
    assert caught.value.report is not None
    assert caught.value.report.resource == camera.CameraResource.NETWORK


def test_a_listening_port_in_use(h264: camera.Camera) -> None:
    with socket.create_server(("127.0.0.1", 0)) as taken:
        port = taken.getsockname()[1]
        with pytest.raises(camera.CameraBusyError, match="already in use"):
            h264.start_output(f"tcp://127.0.0.1:{port}?listen")


@pytest.mark.parametrize(
    ("destination", "words"),
    [
        ("", "empty"),
        ("http://host/x", "unknown scheme"),
        ("/tmp/x.avi", "unknown file type"),
        ("/tmp/x.mjpeg", "MJPEG"),
        ("tcp://host:70000", "port"),
        ("udp://host:5000?listen", "cannot listen"),
    ],
)
def test_destinations_that_are_refused(h264: camera.Camera, destination: str, words: str) -> None:
    with pytest.raises(camera.CameraArgumentError, match=words):
        h264.start_output(destination)


def test_a_missing_directory_is_not_found(h264: camera.Camera) -> None:
    with pytest.raises(camera.CameraNotFoundError, match="cannot create"):
        h264.start_output("/nonexistent-directory/x.h264")


@pytest.mark.parametrize("options", [{"max_clients": 2}, {"datagram_bytes": 1000}, {"queue": 257}])
def test_options_meant_for_another_output_are_refused(
    h264: camera.Camera, tmp_path: Path, options: dict[str, int]
) -> None:
    with pytest.raises(camera.CameraArgumentError):
        h264.start_output(tmp_path / "x.h264", **options)  # type: ignore[arg-type]


def test_container_and_protocol_outputs_need_libavformat(h264: camera.Camera, tmp_path: Path) -> None:
    if "libav" in camera.version_string():
        pytest.skip("this build has libavformat")
    with pytest.raises(camera.CameraUnsupportedError, match="libavformat"):
        h264.start_output(tmp_path / "x.mp4")
    with pytest.raises(camera.CameraUnsupportedError):
        h264.start_output("rtsp://127.0.0.1:8554/robot")


def test_an_output_needs_an_encoder(cam: camera.Camera, tmp_path: Path) -> None:
    with pytest.raises(camera.CameraStateError, match="keel_camera_set_encoder"):
        cam.start_output(tmp_path / "x.h264")


def test_at_most_eight_outputs(h264: camera.Camera, tmp_path: Path) -> None:
    outputs = [h264.start_output(tmp_path / f"{number}.h264") for number in range(camera.limits.OUTPUTS_MAX)]
    assert h264.stats().outputs == camera.limits.OUTPUTS_MAX
    with pytest.raises(camera.CameraTimeoutError) as caught:
        h264.start_output(tmp_path / "one-more.h264")
    assert caught.value.status == Status.E_AGAIN
    with pytest.raises(camera.CameraStateError):
        h264.set_encoder(camera.Codec.MJPEG)
    for output in outputs:
        h264.stop_output(output)


def test_a_destination_too_long_for_the_header_is_refused_before_the_call(h264: camera.Camera) -> None:
    with pytest.raises(ValueError, match="url must encode to at most 255 bytes"):
        h264.start_output("/tmp/" + "x" * 300 + ".h264")
    with pytest.raises(TypeError):
        h264.start_output(5000)  # type: ignore[arg-type]
