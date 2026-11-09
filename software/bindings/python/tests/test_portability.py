# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Portability: import keel works on every platform, and loading fails clearly anywhere but Linux.

CI runs these on Windows and macOS with pytest -m portability. On Linux the platform is injected through the private
layer, so the same check runs everywhere. The package structure is checked here as well, because it needs no
library: every module can be the first one a program imports, and every package exports its generated names. The
camera package keel.camera loads its own library, libkeel-camera, with the same rules.
"""

from __future__ import annotations

import ctypes
import importlib
import pkgutil
import subprocess
import sys

import pytest

import keel
from keel import camera
from keel._internal import _generated, library_loader, native_library
from keel._internal.camera import _generated as camera_generated
from keel._internal.camera import library_loader as camera_loader
from keel._internal.camera import native_library as camera_library
from support.environment import subprocess_env

pytestmark = pytest.mark.portability

PACKAGES = ["keel", "keel.errors", "keel.loop", "keel.state", "keel.peripherals", "keel.sim"]
CAMERA_PACKAGES = [
    "keel.camera",
    "keel.camera.errors",
    "keel.camera.frames",
    "keel.camera.encoding",
    "keel.camera.outputs",
    "keel.camera.stills",
]
MODULES = ["keel", *sorted(module.name for module in pkgutil.walk_packages(keel.__path__, "keel."))]


def test_import_works_without_the_library() -> None:
    assert keel.__version__ == ".".join(str(part) for part in _generated.VERSION)
    assert keel.Status.E_CONFLICT == -15
    assert keel.limits.DUTY_MAX == 1000
    assert keel.abi.VERSION == 0x0001_0000
    assert ctypes.sizeof(_generated.SnapshotStruct) == 144
    assert keel.Status(-99).name == "UNRECOGNIZED_-99"


@pytest.mark.parametrize("value", ["-15", -15.5, True, None])
def test_an_open_enum_refuses_a_value_that_is_not_an_integer(value: object) -> None:
    with pytest.raises(ValueError, match="is not a valid Status"):
        keel.Status(value)


def test_loading_fails_clearly_off_linux(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(native_library, "_current", None)
    platform = sys.platform
    if platform.startswith("linux"):
        platform = "win32"
        monkeypatch.setattr(library_loader, "PLATFORM", platform)
    with pytest.raises(keel.LibraryError, match=f"libkeel requires Linux; found {platform}"):
        keel.load()
    with pytest.raises(keel.LibraryError, match="libkeel requires Linux"):
        keel.open(backend=keel.Backend.SIMULATOR)
    assert native_library._current is None


@pytest.mark.parametrize("module", MODULES)
def test_every_module_can_be_imported_first(module: str) -> None:
    """A fresh interpreter imports the module with no import cycle, and without loading either library."""
    code = (
        f"import sys, {module}, keel._internal.native_library as n, keel._internal.camera.native_library as c; "
        "sys.exit(0 if n._current is None and c._current is None else 1)"
    )
    result = subprocess.run(
        [sys.executable, "-W", "error", "-c", code],
        capture_output=True,
        text=True,
        env=subprocess_env(),
        check=False,
        timeout=60,
    )
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("package", PACKAGES)
def test_every_package_exports_its_generated_names(package: str) -> None:
    """Each package and the keel root export every public name of the package's generated module."""
    module = importlib.import_module(package)
    generated = importlib.import_module(f"{package}._generated")
    assert generated.__all__
    for name in generated.__all__:
        assert name in module.__all__, f"{package}.{name}"
        assert name in keel.__all__, name
        assert getattr(module, name) is getattr(generated, name)
        assert getattr(keel, name) is getattr(generated, name)


def test_every_name_of_keel_resolves() -> None:
    assert len(keel.__all__) == len(set(keel.__all__))
    for name in keel.__all__:
        assert getattr(keel, name) is not None, name


@pytest.mark.parametrize("package", CAMERA_PACKAGES)
def test_every_camera_package_exports_its_generated_names(package: str) -> None:
    """Each camera package and keel.camera export every public name of the package's generated module."""
    module = importlib.import_module(package)
    generated = importlib.import_module(f"{package}._generated")
    assert generated.__all__
    for name in generated.__all__:
        assert name in module.__all__, f"{package}.{name}"
        assert name in camera.__all__, name
        assert getattr(camera, name) is getattr(generated, name)
        assert name not in keel.__all__, name


def test_every_name_of_keel_camera_resolves() -> None:
    assert len(camera.__all__) == len(set(camera.__all__))
    for name in camera.__all__:
        assert getattr(camera, name) is not None, name
    assert keel.camera is camera


def test_the_camera_api_initialises_without_its_library() -> None:
    assert camera.Codec.H264 == 1
    assert camera.PixelFormat.BGR888 == 3
    assert camera.limits.ENCODER_SIZE_MAX == 1920
    assert camera.abi.VERSION == 0x0001_0000
    assert camera_generated.VERSION == (1, 0, 0)
    assert ctypes.sizeof(camera_generated.CameraOptionsStruct) == 72
    info = camera.FrameInfo(camera.PixelFormat.I420, 2, 2, 3, 2, 1, 1, 0, 4, 5, 6, 0, 0)
    assert camera.Frame(info, b"abcdef").to_bytes() == b"abcdef"


def test_camera_loading_fails_clearly_off_linux(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(camera_library, "_current", None)
    platform = sys.platform
    if platform.startswith("linux"):
        platform = "darwin"
        monkeypatch.setattr(camera_loader, "PLATFORM", platform)
    with pytest.raises(camera.CameraLibraryError, match=f"libkeel-camera requires Linux; found {platform}"):
        camera.load()
    with pytest.raises(keel.LibraryError, match="libkeel-camera requires Linux"):
        camera.open()
    assert camera_library._current is None
