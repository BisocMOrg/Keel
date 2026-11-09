# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The camera examples against the synthetic camera, each in a child process, and the README's camera section."""

from __future__ import annotations

import importlib.util
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from types import ModuleType

import pytest

from support.environment import PYTHON, subprocess_env

EXAMPLES = PYTHON / "examples"
README = PYTHON / "README.md"


def run(name: str, *arguments: str, timeout: float = 60) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-W", "error", str(EXAMPLES / f"{name}.py"), *arguments],
        capture_output=True,
        text=True,
        env=subprocess_env(),
        timeout=timeout,
        check=False,
    )


def example(name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(f"keel_example_{name}", EXAMPLES / f"{name}.py")
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_frames_into_vision_code() -> None:
    result = run("camera_frames", "--frames", "20", "--width", "320", "--height", "240", "--framerate", "120")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "frames: numpy yes" in result.stdout
    assert "frames: Camera Module 2 (imx219)" in result.stdout
    assert re.search(r"frame \d+: brightness \d+\.\d, motion \d+\.\d", result.stdout)
    assert "frames: done, 20 read" in result.stdout
    assert result.stderr == ""


def test_frames_without_numpy_use_bytes(monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
    monkeypatch.setitem(sys.modules, "numpy", None)
    monkeypatch.setitem(sys.modules, "cv2", None)
    status = example("camera_frames").main(["--frames", "10", "--width", "160", "--height", "120"])
    output = capsys.readouterr().out
    assert status == 0, output
    assert "frames: numpy no, OpenCV no" in output
    assert re.search(r"frame \d+: brightness \d+\.\d, motion \d+\.\d\n", output)


def test_stream_to_a_file(tmp_path: Path) -> None:
    path = tmp_path / "clip.h264"
    result = run("camera_stream", str(path), "--seconds", "1.5", "--width", "320", "--height", "240")
    assert result.returncode == 0, result.stdout + result.stderr
    assert f"stream: H264 320x240 to {path}" in result.stdout
    assert re.search(r"stream: RUNNING, 1 receiver\(s\), \d+ chunks sent, 0 dropped", result.stdout)
    assert "stream: done" in result.stdout
    assert path.read_bytes().startswith(b"\x00\x00\x00\x01")


def test_stream_to_a_listening_port_and_ctrl_c() -> None:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen(
        [
            sys.executable,
            "-W",
            "error",
            str(EXAMPLES / "camera_stream.py"),
            f"tcp://127.0.0.1:{port}?listen",
            "--mjpeg",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=subprocess_env(),
    )
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                receiver = socket.create_connection(("127.0.0.1", port), timeout=3)
                break
            except ConnectionRefusedError:
                assert time.monotonic() < deadline
                time.sleep(0.1)
        with receiver:
            assert receiver.recv(2).startswith(b"\xff")
            time.sleep(1.5)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    assert process.returncode == 130, stdout + stderr
    assert "stream: MJPEG 640x480" in stdout
    assert "1 receiver(s)" in stdout
    assert "stream: stopped" in stdout


def test_a_still_photo(tmp_path: Path) -> None:
    path = tmp_path / "photo.jpg"
    result = run("camera_still", "-o", str(path), "--width", "640", "--height", "480", "--quality", "90")
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.startswith("still: 640x480, ")
    assert "by the hardware encoder" in result.stdout
    assert path.read_bytes().startswith(b"\xff\xd8")
    full = run("camera_still", "-o", str(tmp_path / "full.jpg"))
    assert full.returncode == 0, full.stdout + full.stderr
    assert full.stdout.startswith("still: 3280x2464, ")


@pytest.mark.parametrize(
    ("name", "arguments"),
    [
        ("camera_frames", ["--frames", "0"]),
        ("camera_stream", ["--seconds", "-1"]),
        ("camera_still", ["--width", "640"]),
        ("camera_still", ["--bogus"]),
    ],
)
def test_usage_errors_exit_with_status_2(name: str, arguments: list[str]) -> None:
    result = run(name, *arguments)
    assert result.returncode == 2
    assert "usage:" in result.stderr


@pytest.mark.parametrize(
    ("name", "arguments", "words"),
    [
        ("camera_frames", ["--camera", "5"], "frames: keel_camera_open: KEEL_E_NOT_FOUND: there is no camera 5"),
        ("camera_stream", ["/tmp/x.avi"], "stream: keel_camera_output_start: KEEL_E_INVALID_ARGUMENT"),
        ("camera_still", ["--camera", "5"], "still: keel_camera_open: KEEL_E_NOT_FOUND"),
    ],
)
def test_camera_errors_exit_with_status_1(name: str, arguments: list[str], words: str) -> None:
    result = run(name, *arguments)
    assert result.returncode == 1, result.stdout + result.stderr
    assert words in result.stderr


def readme_camera_block() -> str:
    """The Python block of the README's camera section."""
    text = README.read_text(encoding="utf-8")
    section = text[text.index("## Camera") :]
    match = re.search(r"```python\n(.*?)```", section, re.S)
    assert match is not None
    return match.group(1)


def test_the_readme_camera_example(tmp_path: Path) -> None:
    result = subprocess.run(
        [sys.executable, "-W", "error", "-c", readme_camera_block()],
        capture_output=True,
        text=True,
        env=subprocess_env(),
        cwd=tmp_path,
        timeout=60,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert (tmp_path / "drive.h264").read_bytes().startswith(b"\x00\x00\x00\x01")
    assert (tmp_path / "photo.jpg").read_bytes().startswith(b"\xff\xd8")
