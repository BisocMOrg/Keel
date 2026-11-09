# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""list_cameras(): models, capabilities and sensor modes, and how a camera that cannot be used is reported."""

from __future__ import annotations

from collections.abc import Callable

import pytest

from keel import camera
from keel.errors import Status


def test_both_synthetic_cameras_are_listed_with_their_modes() -> None:
    module2, module3 = camera.list_cameras()
    assert (module2.index, module2.model, module2.sensor) == (0, "Camera Module 2", "imx219")
    assert (module3.index, module3.model, module3.sensor) == (1, "Camera Module 3", "imx708")
    assert not module2.autofocus
    assert module3.autofocus
    assert module3.info.flags == camera.CameraInfoFlag.AUTOFOCUS
    assert not module2.info.in_use
    assert (module2.info.pixel_array_width, module2.info.pixel_array_height) == (3280, 2464)
    assert (module3.info.pixel_array_width, module3.info.pixel_array_height) == (4608, 2592)
    assert module2.info.id == "/base/soc/i2c0mux/i2c@1/imx219@10"
    assert len(module2.modes) == module2.info.mode_count == 8
    assert len(module3.modes) == module3.info.mode_count == 3
    first = module2.modes[0]
    assert first == camera.SensorMode(
        width=640,
        height=480,
        bit_depth=8,
        max_framerate_milli=200160,
        crop_x=1000,
        crop_y=752,
        crop_width=1280,
        crop_height=960,
    )
    assert first.max_framerate == pytest.approx(200.16)
    assert [mode.bit_depth for mode in module2.modes] == [8] * 4 + [10] * 4


def test_no_camera_is_an_empty_list(fault: Callable[[str], None]) -> None:
    fault("no-camera")
    assert camera.list_cameras() == []


def test_a_camera_held_elsewhere_is_listed_without_rates(fault: Callable[[str], None]) -> None:
    fault("busy")
    for listed in camera.list_cameras():
        assert listed.info.in_use
        assert all(mode.max_framerate_milli == 0 and mode.crop_width == 0 for mode in listed.modes)


def test_a_camera_this_process_holds_is_listed_in_use() -> None:
    with camera.open(1, width=320, height=240):
        module2, module3 = camera.list_cameras()
    assert module3.info.in_use
    assert not module2.info.in_use


def test_a_device_node_without_access_names_the_video_group(fault: Callable[[str], None]) -> None:
    fault("permission")
    with pytest.raises(camera.CameraPermissionError) as caught:
        camera.list_cameras()
    error = caught.value
    assert isinstance(error, PermissionError)
    assert error.function == "keel_camera_list_open"
    assert error.status == Status.E_PERMISSION
    assert error.code == Status.E_PERMISSION.value
    assert error.report is not None
    assert error.report.status == Status.E_PERMISSION
    assert error.report.resource == camera.CameraResource.DEVICE_NODE
    assert error.report.path == "/dev/media0"
    assert "video group" in error.report.message
    assert "sudo keel-setup --user NAME" in str(error)


def test_a_missing_ipa_module_is_not_found(fault: Callable[[str], None]) -> None:
    fault("ipa-missing")
    with pytest.raises(camera.CameraNotFoundError) as caught:
        camera.list_cameras()
    assert caught.value.report is not None
    assert caught.value.report.resource == camera.CameraResource.IPA
    assert "ipa_rpi_vc4.so" in str(caught.value)
    assert "libcamera-ipa" in str(caught.value)
