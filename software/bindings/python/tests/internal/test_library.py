# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Library layer: loading order, the platform and ABI checks, the bound symbols, bounded strings and metadata.

The loader is keel._internal (library_loader, native_library and string_reader); the metadata is keel.library.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import re
import subprocess
import sys

import pytest

import keel
from keel._internal import library_loader, native_library, string_reader
from keel._internal.native_library import NativeLibrary
from support.environment import libkeel_source, subprocess_env

SOURCE = libkeel_source()


def symbols() -> list[str]:
    assert SOURCE is not None
    with (SOURCE / "tests" / "abi" / "symbols.txt").open(encoding="utf-8") as handle:
        return [line.strip() for line in handle if line.strip() and not line.startswith("#")]


@pytest.fixture
def unloaded(monkeypatch: pytest.MonkeyPatch, natives: object) -> None:
    """Pretend that nothing is loaded yet; the session's library is restored afterwards."""
    monkeypatch.setattr(native_library, "_current", None)


class TestResolution:
    def test_explicit_absolute_path_wins(self) -> None:
        assert library_loader.resolve("/opt/keel/libkeel.so.1", "/other/libkeel.so.1") == "/opt/keel/libkeel.so.1"

    def test_keel_library_is_used_when_no_path_is_given(self) -> None:
        assert library_loader.resolve(None, "/build/libkeel.so.1") == "/build/libkeel.so.1"

    def test_empty_keel_library_counts_as_unset(self) -> None:
        assert library_loader.resolve(None, "") == "libkeel.so.1"
        assert library_loader.resolve("", "") == "libkeel.so.1"

    def test_soname_is_the_default(self) -> None:
        assert library_loader.resolve(None, "") == library_loader.SONAME == "libkeel.so.1"

    def test_relative_keel_library_is_refused(self) -> None:
        with pytest.raises(keel.LibraryError, match=r"KEEL_LIBRARY must be an absolute path, not 'build/libkeel"):
            library_loader.resolve(None, "build/libkeel.so.1")

    def test_relative_explicit_path_is_refused(self) -> None:
        with pytest.raises(keel.LibraryError, match=r"keel.load\(path\) must be an absolute path"):
            library_loader.resolve("libkeel.so.1", "")


class TestPlatform:
    @pytest.mark.parametrize("platform", ["win32", "darwin", "cygwin", "freebsd14"])
    def test_anything_but_linux_is_refused(self, platform: str) -> None:
        with pytest.raises(keel.LibraryError, match=f"libkeel requires Linux; found {platform}"):
            library_loader.check_platform(platform)

    def test_linux_is_accepted(self) -> None:
        library_loader.check_platform("linux")

    def test_load_checks_the_platform_before_dlopen(self, unloaded: None, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setattr(library_loader, "PLATFORM", "win32")
        monkeypatch.setattr(native_library, "bind", lambda name: pytest.fail("bind must not run off Linux"))
        with pytest.raises(keel.LibraryError, match="requires Linux; found win32"):
            keel.load()


class TestLoading:
    def test_missing_library(self) -> None:
        with pytest.raises(keel.LibraryError, match=re.escape("cannot load /nonexistent/libkeel.so.1")) as caught:
            native_library.bind("/nonexistent/libkeel.so.1")
        assert isinstance(caught.value.__cause__, OSError)
        assert "KEEL_LIBRARY" in str(caught.value)

    def test_missing_symbol(self) -> None:
        libc = ctypes.util.find_library("c")
        assert libc is not None
        with pytest.raises(keel.LibraryError, match="does not export keel_abi_version"):
            native_library.bind(libc)

    def test_failed_load_is_not_kept(self, unloaded: None, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("KEEL_LIBRARY", "relative/libkeel.so.1")
        with pytest.raises(keel.LibraryError):
            keel.load()
        assert native_library._current is None
        monkeypatch.setenv("KEEL_LIBRARY", "/nonexistent/libkeel.so.1")
        with pytest.raises(keel.LibraryError, match="cannot load"):
            keel.load()
        assert native_library._current is None

    def test_a_corrected_keel_library_loads(self, unloaded: None, monkeypatch: pytest.MonkeyPatch) -> None:
        good = os.environ["KEEL_LIBRARY"]
        monkeypatch.setenv("KEEL_LIBRARY", "relative/libkeel.so.1")
        with pytest.raises(keel.LibraryError):
            keel.load()
        monkeypatch.setenv("KEEL_LIBRARY", good)
        keel.load()
        assert native_library._current is not None
        assert native_library._current.path == good

    def test_load_is_idempotent(self, natives: object) -> None:
        library = native_library.get()
        keel.load()
        keel.load(library.path)
        assert native_library.get() is library

    def test_a_second_library_is_refused(self, natives: object) -> None:
        with pytest.raises(keel.LibraryError, match="already loaded from"):
            keel.load("/opt/other/libkeel.so.1")

    def test_all_37_functions_are_bound_in_header_order(self, library: NativeLibrary) -> None:
        assert list(library.natives) == symbols()
        assert len(library.natives) == 37


class TestAbiCheck:
    @pytest.mark.parametrize("version", [0x0001_0000, 0x0001_0005])
    def test_same_major_and_newer_minor_are_accepted(self, version: int) -> None:
        library_loader.check_abi(version, "/lib/libkeel.so.1")

    @pytest.mark.parametrize("version", [0x0002_0000, 0x0000_0009, 0x8001_0000])
    def test_other_majors_are_refused(self, version: int) -> None:
        with pytest.raises(keel.LibraryError, match=r"/lib/libkeel.so.1 has ABI \d+\.\d+, but keel-robot"):
            library_loader.check_abi(version, "/lib/libkeel.so.1")

    def test_a_minor_below_the_bindings_is_refused(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setattr(keel.abi, "MINOR", 3)
        with pytest.raises(keel.LibraryError) as caught:
            library_loader.check_abi(0x0001_0002, "/usr/lib/libkeel.so.1")
        message = str(caught.value)
        assert "ABI 1.2" in message
        assert "needs ABI 1.3" in message
        assert "dpkg-query -W libkeel1" in message
        assert 'pip install "keel-robot==X.Y.*"' in message

    def test_the_loaded_library_matches_the_header(self) -> None:
        assert keel.abi_version() == (keel.abi.MAJOR, keel.abi.MINOR)
        assert keel.abi.VERSION == (keel.abi.MAJOR << 16) | keel.abi.MINOR


class TestStrings:
    def test_a_string_without_nul_within_the_bound_is_refused(self) -> None:
        buffer = ctypes.create_string_buffer(b"x" * keel.limits.STRING_MAX, keel.limits.STRING_MAX)
        with pytest.raises(keel.LibraryError, match="no NUL within KEEL_STRING_MAX"):
            string_reader.read_string(ctypes.addressof(buffer), "keel_status_name")

    def test_the_longest_allowed_string_is_read(self) -> None:
        text = b"y" * (keel.limits.STRING_MAX - 1)
        buffer = ctypes.create_string_buffer(text, keel.limits.STRING_MAX)
        assert string_reader.read_string(ctypes.addressof(buffer), "keel_status_name") == text.decode()

    def test_reading_stops_at_the_nul(self) -> None:
        buffer = ctypes.create_string_buffer(b"KEEL_OK\0garbage", 32)
        assert string_reader.read_string(ctypes.addressof(buffer), "keel_status_name") == "KEEL_OK"

    def test_null_is_refused(self) -> None:
        with pytest.raises(keel.LibraryError, match="returned NULL"):
            string_reader.read_string(None, "keel_version_string")

    def test_version_string(self, natives: object) -> None:
        version = keel.version_string()
        assert version.startswith(f"{keel.abi.MAJOR}.{keel.abi.MINOR}.")
        assert re.search(r"backends [\w,]*simulator", version)

    def test_strings_are_cached(self, library: NativeLibrary) -> None:
        assert keel.status_name(keel.Status.E_PWM) == "KEEL_E_PWM"
        assert keel.status_name(keel.Status.E_PWM) is library.status_name(keel.Status.E_PWM)


class TestMetadata:
    def test_every_status_name_description_and_class(self, natives: object) -> None:
        for status in keel.Status:
            assert keel.status_name(status) == "KEEL_" + status.name
            description = keel.status_string(status)
            assert description
            assert description.endswith(".")
            assert isinstance(keel.status_class(status), keel.StatusClass)

    def test_every_cause_name(self, natives: object) -> None:
        for cause in keel.FaultCause:
            assert keel.fault_cause_name(cause) == cause.name

    def test_unknown_codes_get_the_librarys_answers(self, natives: object) -> None:
        assert keel.status_name(-99) == "KEEL_E_UNKNOWN"
        assert keel.status_class(-99) == keel.StatusClass.FAULT
        assert keel.status_string(-99)
        assert keel.fault_cause_name(9999) == "UNKNOWN"

    @pytest.mark.parametrize("code", [2**31, -(2**31) - 1])
    def test_status_outside_int32_raises_overflow_error(self, natives: object, code: int) -> None:
        with pytest.raises(OverflowError, match="keel_status_name: status"):
            keel.status_name(code)

    def test_import_does_not_load_the_library(self) -> None:
        code = "import sys, keel, keel._internal.native_library as l; sys.exit(0 if l._current is None else 1)"
        result = subprocess.run([sys.executable, "-c", code], env=subprocess_env(), check=False, timeout=60)
        assert result.returncode == 0
