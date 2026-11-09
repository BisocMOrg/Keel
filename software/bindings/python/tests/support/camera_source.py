# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The synthetic camera of a libkeel-camera test build, and the patterns its frames carry.

A build with KEEL_CAMERA_TEST_HOOKS=ON (the CMake presets) replaces libcamera and the hardware encoders when
KEEL_CAMERA_TEST_SOURCE=1 is set (SPEC section 26.9): camera 0 is a Camera Module 2 (IMX219, fixed focus), camera 1 a
Camera Module 3 (IMX708, autofocus). KEEL_CAMERA_TEST_FAULT injects one failure. The library reads both variables at
each call, so a suite sets them between camera lifetimes, never while a camera captures.
"""

from __future__ import annotations

import os
import socket

import pytest

from keel import camera

TEST_SOURCE = "KEEL_CAMERA_TEST_SOURCE"
FAULT = "KEEL_CAMERA_TEST_FAULT"
ENCODER = "KEEL_CAMERA_TEST_ENCODER"
FAULTS = ("no-camera", "permission", "ipa-missing", "busy", "stall", "capture-fail", "encoder-missing", "encoder-fail")
PADDING = 0xA5
"""The byte the synthetic camera writes into row padding."""


def require_test_build() -> None:
    """Fail unless KEEL_CAMERA_LIBRARY names a libkeel-camera test build."""
    if not os.environ.get("KEEL_CAMERA_LIBRARY"):
        pytest.fail(
            "set KEEL_CAMERA_LIBRARY to the absolute path of a libkeel-camera test build "
            "(cmake --preset sim-only in software/libkeel, with libcamera-dev installed)"
        )
    if "test-hooks" not in camera.version_string():
        pytest.fail(f"KEEL_CAMERA_LIBRARY is not a test build: {camera.version_string()}")


def y_value(x: int, y: int, sequence: int) -> int:
    """The Y sample of the synthetic I420 pattern at pixel (x, y) of the frame with this sequence number."""
    return (7 * x + 3 * y + (sequence & 0xFF)) & 0xFF


def u_value(x: int, sequence: int) -> int:
    """The U sample at chroma column x."""
    return (5 * x + (sequence & 0xFF)) & 0xFF


def v_value(y: int, sequence: int) -> int:
    """The V sample at chroma row y."""
    return (5 * y + 2 * (sequence & 0xFF)) & 0xFF


def rgb_value(x: int, y: int, sequence: int) -> tuple[int, int, int]:
    """The (R, G, B) of the synthetic RGB888 pattern at pixel (x, y)."""
    n = sequence & 0xFF
    return (x + n) & 0xFF, (y + n) & 0xFF, (x + y) & 0xFF


def check_pattern(frame: camera.Frame, *, hflip: bool = False, vflip: bool = False) -> None:
    """Check every pixel and every padding byte of a frame against the synthetic pattern, without numpy."""
    info, data = frame.info, frame.data
    sequence = info.sequence
    for y in range(info.height):
        sy = info.height - 1 - y if vflip else y
        row = info.offset0 + y * info.stride0
        for x in range(info.width):
            sx = info.width - 1 - x if hflip else x
            if info.format == camera.PixelFormat.I420:
                assert data[row + x] == y_value(sx, sy, sequence), (x, y)
            else:
                red, green, blue = rgb_value(sx, sy, sequence)
                pixel = bytes(data[row + 3 * x : row + 3 * x + 3])
                expected = (blue, green, red) if info.format == camera.PixelFormat.BGR888 else (red, green, blue)
                assert pixel == bytes(expected), (x, y)
        used = info.width if info.format == camera.PixelFormat.I420 else info.width * 3
        assert set(data[row + used : row + info.stride0]) <= {PADDING}
    if info.format != camera.PixelFormat.I420:
        return
    for y in range(info.height // 2):
        sy = info.height // 2 - 1 - y if vflip else y
        for x in range(info.width // 2):
            sx = info.width // 2 - 1 - x if hflip else x
            assert data[info.offset1 + y * info.stride1 + x] == u_value(sx, sequence)
            assert data[info.offset2 + y * info.stride2 + x] == v_value(sy, sequence)


def free_port(kind: socket.SocketKind = socket.SOCK_STREAM) -> int:
    """A local port that was free a moment ago."""
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(("127.0.0.1", 0))
        port: int = probe.getsockname()[1]
        return port


def open_fds() -> int:
    """The number of open file descriptors of this process."""
    return len(os.listdir("/proc/self/fd"))


def threads() -> int:
    """The number of threads of this process, the library's included."""
    return len(os.listdir("/proc/self/task"))
