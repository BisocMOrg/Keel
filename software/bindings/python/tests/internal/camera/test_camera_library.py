# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Loading libkeel-camera: resolution, the ABI check, the failures, and its independence from libkeel.so."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

import keel
from keel import camera
from keel._internal.camera import library_loader, native_library
from support.environment import subprocess_env


@pytest.fixture
def unloaded(monkeypatch: pytest.MonkeyPatch) -> None:
    """Forget the loaded camera library for one test; the original is restored afterwards."""
    monkeypatch.setattr(native_library, "_current", None)


def test_the_test_build_loads_and_reports_its_version() -> None:
    camera.load()
    assert camera.abi_version() == (camera.abi.MAJOR, camera.abi.MINOR) == (1, 0)
    assert camera.abi.VERSION == 0x0001_0000
    version = camera.version_string()
    assert version.startswith("1.0.0 (")
    assert "libcamera" in version
    assert "test-hooks" in version
    assert native_library.get().version_string() is version  # read once


def test_load_accepts_the_same_path_again_and_refuses_another() -> None:
    path = os.environ["KEEL_CAMERA_LIBRARY"]
    camera.load(path)
    camera.load(Path(path))
    with pytest.raises(camera.CameraLibraryError, match="already loaded from"):
        camera.load("/usr/lib/other/libkeel-camera.so.1")


def test_a_relative_path_is_refused(unloaded: None, monkeypatch: pytest.MonkeyPatch) -> None:
    with pytest.raises(camera.CameraLibraryError, match=r"keel.camera.load\(path\) must be an absolute path"):
        camera.load("libkeel-camera.so.1")
    monkeypatch.setenv("KEEL_CAMERA_LIBRARY", "build/libkeel-camera.so.1")
    with pytest.raises(camera.CameraLibraryError, match="KEEL_CAMERA_LIBRARY must be an absolute path"):
        camera.load()
    assert native_library._current is None


def test_a_missing_library_names_the_package(unloaded: None, tmp_path: Path) -> None:
    missing = tmp_path / "libkeel-camera.so.1"
    with pytest.raises(camera.CameraLibraryError, match="install the libkeel-camera1 package") as caught:
        camera.load(missing)
    assert str(missing) in str(caught.value)
    assert isinstance(caught.value, keel.LibraryError)
    assert native_library._current is None


def test_an_empty_variable_counts_as_unset() -> None:
    assert library_loader.resolve(None, "") == library_loader.SONAME == "libkeel-camera.so.1"
    assert library_loader.resolve("", "/opt/libkeel-camera.so.1") == "/opt/libkeel-camera.so.1"


def test_a_library_without_the_camera_functions_is_refused(unloaded: None) -> None:
    libkeel = os.environ.get("KEEL_LIBRARY")
    if not libkeel:
        pytest.skip("needs KEEL_LIBRARY, a shared library that is not libkeel-camera")
    with pytest.raises(camera.CameraLibraryError, match="does not export keel_camera_abi_version"):
        camera.load(libkeel)


def test_the_abi_check() -> None:
    library_loader.check_abi(0x0001_0000, "x")
    library_loader.check_abi(0x0001_0005, "x")
    with pytest.raises(camera.CameraLibraryError, match=r"has camera ABI 2\.0, but this binding .* needs ABI 1\.0"):
        library_loader.check_abi(0x0002_0000, "/opt/libkeel-camera.so.2")


def test_loading_fails_clearly_off_linux(unloaded: None, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(library_loader, "PLATFORM", "win32")
    with pytest.raises(camera.CameraLibraryError, match="libkeel-camera requires Linux; found win32"):
        camera.list_cameras()
    with pytest.raises(camera.CameraLibraryError, match="requires Linux"):
        camera.open()
    assert native_library._current is None


def _child(code: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-W", "error", "-c", code],
        capture_output=True,
        text=True,
        env=subprocess_env(),
        check=False,
        timeout=60,
    )


def test_importing_keel_loads_neither_library() -> None:
    result = _child(
        "import sys, keel, keel.camera, keel._internal.native_library as k, keel._internal.camera.native_library as c\n"
        "maps = open('/proc/self/maps').read()\n"
        "sys.exit(0 if k._current is None and c._current is None and 'libkeel' not in maps else 1)"
    )
    assert result.returncode == 0, result.stderr


def test_a_camera_program_never_loads_libkeel() -> None:
    result = _child(
        "import sys\n"
        "from keel import camera\n"
        "import keel._internal.native_library as board\n"
        "assert len(camera.list_cameras()) == 2\n"
        "with camera.open(width=160, height=120) as cam:\n"
        "    cam.start()\n"
        "    cam.read_frame()\n"
        "maps = open('/proc/self/maps').read()\n"
        "assert 'libkeel-camera.so' in maps, maps\n"
        "assert 'libkeel.so' not in maps\n"
        "sys.exit(0 if board._current is None else 1)"
    )
    assert result.returncode == 0, result.stderr
