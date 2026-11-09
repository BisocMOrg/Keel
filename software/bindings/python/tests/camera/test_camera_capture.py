# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Capture: open options, stream layouts, raw frames and their numpy views, conversion, drops and focus."""

from __future__ import annotations

import dataclasses
import math
import sys
import time
from collections.abc import Callable

import numpy as np
import pytest

from keel import camera
from keel.errors import Status
from support.camera_source import check_pattern


def read_started(cam: camera.Camera) -> camera.Frame:
    cam.start()
    return cam.read_frame(timeout=2)


def read_until_it_fails(cam: camera.Camera) -> None:
    """Read frames until the camera raises; fail if ten frames arrive."""
    for _ in range(10):
        cam.read_frame()
    pytest.fail("the camera did not fail")


class TestOpen:
    def test_defaults(self) -> None:
        with camera.open() as cam:
            assert cam.index == 0
            assert cam.info.sensor == "imx219"
            assert cam.info.model == "Camera Module 2"
            stream = cam.stream_info()
            assert (stream.format, stream.width, stream.height) == (camera.PixelFormat.I420, 640, 480)
            assert stream.framerate == 30.0
            assert (stream.plane_count, stream.stride0, stream.stride1, stream.stride2) == (3, 640, 320, 320)
            assert (stream.offset0, stream.offset1, stream.offset2) == (0, 640 * 480, 640 * 480 + 320 * 240)
            assert stream.frame_bytes == 640 * 480 * 3 // 2
            assert (stream.codec, stream.encoded_bytes_max) == (camera.Codec.NONE, 0)
            stats = cam.stats()
            assert stats.state == camera.CaptureState.STOPPED
            assert stats.frames_captured == 0
            report = cam.last_error()
            assert (report.status, report.message) == (Status.OK, "")

    @pytest.mark.parametrize(
        ("pixel_format", "width", "height", "stride", "size"),
        [
            (camera.PixelFormat.I420, 1280, 720, 1280, 1280 * 720 * 3 // 2),
            (camera.PixelFormat.I420, 1000, 750, 1024, 1024 * 750 + 512 * 375 * 2),
            (camera.PixelFormat.RGB888, 640, 480, 1920, 1920 * 480),
            (camera.PixelFormat.BGR888, 100, 64, 320, 320 * 64),
        ],
    )
    def test_layouts(self, pixel_format: camera.PixelFormat, width: int, height: int, stride: int, size: int) -> None:
        with camera.open(width=width, height=height, framerate=15, pixel_format=pixel_format) as cam:
            stream = cam.stream_info()
            assert (stream.width, stream.stride0, stream.frame_bytes) == (width, stride, size)
            assert stream.framerate_milli == 15000
            assert stream.plane_count == (3 if pixel_format == camera.PixelFormat.I420 else 1)

    def test_a_second_handle_on_the_same_camera_is_busy(self) -> None:
        with camera.open(width=160, height=120):
            with pytest.raises(camera.CameraBusyError, match="in use") as caught:
                camera.open(width=160, height=120)
            assert caught.value.status == Status.E_IN_USE
            with camera.open(1, width=160, height=120) as other:
                assert other.info.sensor == "imx708"
        with camera.open(width=160, height=120):
            pass

    @pytest.mark.parametrize(
        ("fault_name", "error_type", "resource", "words"),
        [
            ("no-camera", camera.CameraNotFoundError, camera.CameraResource.CAMERA, "ribbon cable"),
            ("busy", camera.CameraBusyError, camera.CameraResource.CAMERA, "in use by another process"),
            ("permission", camera.CameraPermissionError, camera.CameraResource.DEVICE_NODE, "keel-setup --user"),
            ("ipa-missing", camera.CameraNotFoundError, camera.CameraResource.IPA, "IPA module"),
        ],
    )
    def test_setup_failures(
        self,
        fault: Callable[[str], None],
        fault_name: str,
        error_type: type[camera.CameraError],
        resource: camera.CameraResource,
        words: str,
    ) -> None:
        fault(fault_name)
        with pytest.raises(error_type, match=words) as caught:
            camera.open()
        assert caught.value.function == "keel_camera_open"
        assert caught.value.report is not None
        assert caught.value.report.resource == resource

    def test_a_camera_index_past_the_last_one(self) -> None:
        with pytest.raises(camera.CameraNotFoundError, match="there is no camera 2"):
            camera.open(2)

    @pytest.mark.parametrize(
        ("options", "words"),
        [
            ({"width": 11}, "width and height"),
            ({"width": 32, "height": 32}, "width and height"),
            ({"framerate": 500}, "framerate"),
            ({"rotation": 90}, "rotation"),
            ({"frame_queue": 9}, "frame_queue"),
            ({"pixel_format": 9}, "format"),
            ({"exposure_us": 2_000_000}, "exposure"),
            ({"gain": 0.5}, "gain"),
            ({"ev": 9}, "ev"),
            ({"red_gain": 1.5}, "gain"),
        ],
    )
    def test_out_of_range_options_are_refused_by_the_library(self, options: dict[str, object], words: str) -> None:
        with pytest.raises(camera.CameraArgumentError, match=words) as caught:
            camera.open(**options)  # type: ignore[arg-type]
        error = caught.value
        assert isinstance(error, ValueError)
        assert error.report is not None
        assert error.report.resource == camera.CameraResource.OPTIONS

    @pytest.mark.parametrize(
        ("options", "error_type", "words"),
        [
            ({"framerate": math.nan}, ValueError, "framerate must be a finite number"),
            ({"gain": math.inf}, ValueError, "gain must be a finite number"),
            ({"framerate": "30"}, TypeError, "framerate must be a number, not str"),
            ({"ev": True}, TypeError, "ev must be a number, not bool"),
            ({"width": 640.0}, TypeError, "width must be an integer"),
            ({"width": -1}, OverflowError, "width = -1 is outside uint32_t"),
            ({"gain": -1.0}, OverflowError, "gain_milli = -1000 is outside uint32_t"),
            ({"index": 1 << 32}, OverflowError, "index"),
        ],
    )
    def test_the_binding_refuses_what_ctypes_would_truncate(
        self, options: dict[str, object], error_type: type[Exception], words: str
    ) -> None:
        with pytest.raises(error_type, match=words) as caught:
            camera.open(**options)  # type: ignore[arg-type]
        assert not isinstance(caught.value, camera.CameraError)


class TestFrames:
    @pytest.mark.parametrize("pixel_format", list(camera.PixelFormat))
    def test_every_format_carries_the_pattern(self, pixel_format: camera.PixelFormat) -> None:
        with camera.open(width=96, height=64, framerate=120, pixel_format=pixel_format) as cam:
            frame = read_started(cam)
            assert frame.pixel_format == pixel_format
            assert (frame.width, frame.height) == (96, 64)
            check_pattern(frame)

    @pytest.mark.parametrize(("hflip", "vflip", "rotation"), [(True, False, 0), (False, True, 0), (True, True, 0)])
    def test_flips(self, hflip: bool, vflip: bool, rotation: int) -> None:
        with camera.open(width=96, height=64, framerate=120, hflip=hflip, vflip=vflip, rotation=rotation) as cam:
            check_pattern(read_started(cam), hflip=hflip, vflip=vflip)

    def test_rotation_180_is_both_flips(self) -> None:
        with camera.open(width=96, height=64, framerate=120, rotation=180) as cam:
            check_pattern(read_started(cam), hflip=True, vflip=True)

    def test_frame_metadata_and_bytes(self, cam: camera.Camera) -> None:
        cam.start()
        first = cam.read_frame()
        second = cam.read_frame()
        assert second.sequence > first.sequence
        assert second.timestamp_ns > first.timestamp_ns
        assert first.info.bytes == len(first.data) == len(bytes(first)) == cam.stream_info().frame_bytes
        assert first.data.readonly
        assert repr(first) == f"<keel.camera.frames.Frame 320x240 I420, sequence {first.sequence}, 115200 bytes>"

    def test_frames_iterates_until_the_camera_is_closed(self) -> None:
        cam = camera.open(width=160, height=120, framerate=120)
        cam.start()
        sequences = []
        for frame in cam.frames():
            sequences.append(frame.sequence)
            if len(sequences) == 5:
                cam.close()
        assert len(sequences) == 5
        assert sequences == sorted(sequences)

    def test_a_slow_reader_gets_the_newest_frame_and_the_drops_are_counted(self, cam: camera.Camera) -> None:
        cam.start()
        first = cam.read_frame()
        time.sleep(0.2)
        newest = cam.read_frame()
        assert newest.sequence - first.sequence > 5
        stats = cam.stats()
        assert stats.frames_dropped > 0
        assert stats.frames_read == 2
        assert stats.frames_captured >= stats.frames_read + stats.frames_dropped

    def test_a_longer_queue_keeps_consecutive_frames(self) -> None:
        with camera.open(width=160, height=120, framerate=120, frame_queue=camera.limits.FRAME_QUEUE_MAX) as cam:
            cam.start()
            time.sleep(0.2)
            sequences = [cam.read_frame().sequence for _ in range(camera.limits.FRAME_QUEUE_MAX)]
        assert sequences == list(range(sequences[0], sequences[0] + camera.limits.FRAME_QUEUE_MAX))

    def test_reading_before_start_is_a_state_error(self, cam: camera.Camera) -> None:
        with pytest.raises(camera.CameraStateError) as caught:
            cam.read_frame()
        assert caught.value.function == "keel_camera_read_frame"
        assert caught.value.report is not None

    def test_a_stalled_camera_times_out(self, fault: Callable[[str], None]) -> None:
        fault("stall")
        with camera.open(width=160, height=120, framerate=120) as cam:
            cam.start()
            for _ in range(3):
                cam.read_frame()
            with pytest.raises(camera.CameraTimeoutError) as caught:
                cam.read_frame(timeout=0.2)
            assert isinstance(caught.value, TimeoutError)
            assert caught.value.status == Status.E_AGAIN

    def test_a_camera_that_fails_while_running(self, fault: Callable[[str], None]) -> None:
        fault("capture-fail")
        with camera.open(width=160, height=120, framerate=120) as cam:
            cam.start()
            with pytest.raises(camera.CameraError) as caught:
                read_until_it_fails(cam)
            error = caught.value
            assert type(error) is camera.CameraError
            assert error.status == Status.E_PERIPHERAL
            assert "test fault" in str(error)
            assert cam.stats().state == camera.CaptureState.FAILED
            assert cam.last_error().status == Status.E_PERIPHERAL
            cam.stop()
            assert cam.stats().state == camera.CaptureState.STOPPED

    @pytest.mark.parametrize("timeout", [-1.0, math.nan])
    def test_a_bad_timeout_is_refused_before_the_call(self, cam: camera.Camera, timeout: float) -> None:
        with pytest.raises(ValueError, match="timeout must"):
            cam.read_frame(timeout)

    def test_a_timeout_above_the_limit_is_refused_by_the_library(self, cam: camera.Camera) -> None:
        cam.start()
        with pytest.raises(camera.CameraArgumentError):
            cam.read_frame(timeout=camera.limits.TIMEOUT_MAX_MS / 1000 + 1)

    def test_a_frame_needs_its_bytes(self) -> None:
        with camera.open(width=160, height=120, framerate=120) as cam:
            frame = read_started(cam)
        with pytest.raises(ValueError, match=f"needs {frame.info.bytes} bytes, but data has 3"):
            camera.Frame(frame.info, b"abc")
        copy = camera.Frame(frame.info, bytes(frame.data))
        assert copy.to_bytes() == frame.to_bytes()


class TestArrays:
    def test_packed_formats_are_zero_copy_views(self) -> None:
        with camera.open(width=96, height=64, framerate=120, pixel_format=camera.PixelFormat.BGR888) as cam:
            frame = read_started(cam)
        image = frame.array()
        assert image.shape == (64, 96, 3)
        assert image.strides == (320, 3, 1)
        n = frame.sequence & 0xFF
        assert tuple(image[5, 7]) == ((7 + 5) & 0xFF, (5 + n) & 0xFF, (7 + n) & 0xFF)
        image[0, 0] = (1, 2, 3)  # a view: writing changes the frame's own bytes
        assert bytes(frame.data[:3]) == b"\x01\x02\x03"

    def test_i420_planes_and_the_opencv_layout(self) -> None:
        with camera.open(width=320, height=240, framerate=120) as cam:
            frame = read_started(cam)
        n = frame.sequence & 0xFF
        luma = frame.plane(0)
        assert luma.shape == (240, 320)
        assert int(luma[10, 20]) == (7 * 20 + 3 * 10 + n) & 0xFF
        assert frame.plane(1).shape == frame.plane(2).shape == (120, 160)
        assert int(frame.plane(1)[0, 3]) == (15 + n) & 0xFF
        assert int(frame.plane(2)[4, 0]) == (20 + 2 * n) & 0xFF
        whole = frame.array()
        assert whole.shape == (360, 320)
        assert np.shares_memory(whole, luma)
        assert whole.tobytes() == frame.to_bytes() == bytes(frame.data)

    def test_padded_i420_rows_give_a_packed_copy(self) -> None:
        with camera.open(width=96, height=64, framerate=120) as cam:
            frame = read_started(cam)
        assert frame.info.stride0 == 128
        assert frame.plane(0).strides == (128, 1)
        whole = frame.array()
        assert whole.shape == (96, 96)
        assert not np.shares_memory(whole, frame.plane(0))
        assert whole.tobytes() == frame.to_bytes()
        assert len(frame.to_bytes()) == 96 * 64 * 3 // 2

    def test_plane_needs_an_i420_frame_and_a_valid_index(self) -> None:
        with camera.open(width=96, height=64, framerate=120, pixel_format=camera.PixelFormat.RGB888) as cam:
            rgb = read_started(cam)
        with pytest.raises(ValueError, match="plane\\(\\) needs an I420 frame"):
            rgb.plane(0)
        assert len(rgb.to_bytes()) == 96 * 64 * 3
        with camera.open(width=96, height=64, framerate=120) as cam:
            i420 = read_started(cam)
        with pytest.raises(ValueError, match="planes 0, 1 and 2, not 3"):
            i420.plane(3)

    def test_without_numpy_the_arrays_name_the_install_command(self, monkeypatch: pytest.MonkeyPatch) -> None:
        with camera.open(width=96, height=64, framerate=120) as cam:
            frame = read_started(cam)
        monkeypatch.setitem(sys.modules, "numpy", None)
        with pytest.raises(ModuleNotFoundError, match=r"Frame.array\(\) needs numpy: sudo apt install python3-numpy"):
            frame.array()
        with pytest.raises(ModuleNotFoundError, match=r"Frame.plane\(\) needs numpy"):
            frame.plane(0)
        assert len(frame.to_bytes()) == 96 * 64 * 3 // 2  # bytes need no numpy


def reference_rgb(frame: camera.Frame) -> np.ndarray:
    """The conversion of camera.h, computed with numpy from the frame's planes."""
    luma = frame.plane(0).astype(np.int32)
    u = frame.plane(1).astype(np.int32).repeat(2, axis=0).repeat(2, axis=1) - 128
    v = frame.plane(2).astype(np.int32).repeat(2, axis=0).repeat(2, axis=1) - 128
    red = luma + ((91881 * v + 32768) >> 16)
    green = luma + ((-22554 * u - 46802 * v + 32768) >> 16)
    blue = luma + ((116130 * u + 32768) >> 16)
    return np.clip(np.stack([red, green, blue], axis=2), 0, 255).astype(np.uint8)


class TestConvert:
    @pytest.mark.parametrize("width", [320, 96])
    def test_i420_to_rgb_and_bgr(self, width: int) -> None:
        with camera.open(width=width, height=64, framerate=120) as cam:
            frame = read_started(cam)
        expected = reference_rgb(frame)
        rgb = frame.convert(camera.PixelFormat.RGB888)
        bgr = frame.convert(camera.PixelFormat.BGR888)
        assert (rgb.pixel_format, bgr.pixel_format) == (camera.PixelFormat.RGB888, camera.PixelFormat.BGR888)
        assert (rgb.sequence, rgb.timestamp_ns) == (frame.sequence, frame.timestamp_ns)
        assert rgb.info.stride0 == width * 3
        assert np.array_equal(rgb.array(), expected)
        assert np.array_equal(bgr.array(), expected[:, :, ::-1])

    def test_a_frame_from_read_only_bytes_converts_too(self) -> None:
        with camera.open(width=96, height=64, framerate=120) as cam:
            frame = read_started(cam)
        for data in (bytes(frame.data), memoryview(bytes(frame.data))):
            copy = camera.Frame(frame.info, data)
            assert (
                copy.convert(camera.PixelFormat.RGB888).to_bytes()
                == frame.convert(camera.PixelFormat.RGB888).to_bytes()
            )

    def test_convert_needs_i420_and_a_packed_target(self) -> None:
        with camera.open(width=96, height=64, framerate=120, pixel_format=camera.PixelFormat.RGB888) as cam:
            rgb = read_started(cam)
        with pytest.raises(ValueError, match="convert\\(\\) needs an I420 frame; this one is RGB888"):
            rgb.convert(camera.PixelFormat.BGR888)
        with camera.open(width=96, height=64, framerate=120) as cam:
            i420 = read_started(cam)
        with pytest.raises(ValueError, match="to RGB888 or BGR888, not I420"):
            i420.convert(camera.PixelFormat.I420)

    def test_a_layout_that_does_not_fit_is_refused_by_the_library(self) -> None:
        with camera.open(width=96, height=64, framerate=120) as cam:
            frame = read_started(cam)
        broken = dataclasses.replace(frame.info, offset2=1 << 20)
        with pytest.raises(camera.CameraArgumentError) as caught:
            camera.Frame(broken, frame.data).convert(camera.PixelFormat.RGB888)
        assert caught.value.function == "keel_camera_convert_i420"
        assert caught.value.report is None


class TestFocus:
    @pytest.mark.parametrize(
        "options",
        [
            {"af_mode": camera.AutofocusMode.CONTINUOUS},
            {"af_range": camera.AutofocusRange.MACRO},
        ],
    )
    def test_camera_module_2_has_a_fixed_focus_lens(self, options: dict[str, object]) -> None:
        with pytest.raises(camera.CameraUnsupportedError, match="fixed-focus") as caught:
            camera.open(**options)  # type: ignore[arg-type]
        assert "Camera Module 2" in str(caught.value)

    def test_trigger_autofocus_needs_an_autofocus_camera(self, cam: camera.Camera) -> None:
        cam.start()
        with pytest.raises(camera.CameraUnsupportedError):
            cam.trigger_autofocus(timeout=0.5)

    def test_camera_module_3_focuses(self) -> None:
        with camera.open(
            1, width=320, height=240, af_mode=camera.AutofocusMode.AUTO, af_range=camera.AutofocusRange.FULL
        ) as cam:
            with pytest.raises(camera.CameraStateError):
                cam.trigger_autofocus(timeout=0.5)
            cam.start()
            with pytest.raises(camera.CameraArgumentError):
                cam.trigger_autofocus(timeout=0)
            assert cam.trigger_autofocus(timeout=1) == camera.AutofocusState.FOCUSED

    def test_manual_focus_holds_the_lens_and_has_no_scan(self) -> None:
        with camera.open(1, width=320, height=240, af_mode=camera.AutofocusMode.MANUAL, lens_position=2.0) as cam:
            cam.start()
            cam.read_frame()
            with pytest.raises(camera.CameraUnsupportedError):
                cam.trigger_autofocus(timeout=0.5)
        with pytest.raises(camera.CameraArgumentError, match="lens_position"):
            camera.open(1, af_mode=camera.AutofocusMode.MANUAL, lens_position=32.001)
