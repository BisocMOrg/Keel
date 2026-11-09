# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""ABI layer: the generated declarations against keel.h, the layout pins of layout.c and the loaded library.

These tests are the binding's drift gate: struct sizes and offsets equal
the pins that tests/abi/check_headers.sh proves with the compiler on every target, the prototypes equal the
preprocessed header's declarations, every generated constant equals the value the header evaluates to, and the
generated names equal the library's own strings.
"""

from __future__ import annotations

import ctypes
import enum
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from types import ModuleType
from typing import Any

import pytest

import keel
from keel._internal import _generated
from support.environment import libkeel_source

SOURCE = libkeel_source()
pytestmark = pytest.mark.skipif(SOURCE is None, reason="needs the libkeel source tree (set KEEL_SOURCE_DIR)")

ENUM_TYPES = {
    "keel_status": keel.Status,
    "keel_status_class": keel.StatusClass,
    "keel_state": keel.State,
    "keel_ack_phase": keel.AckPhase,
    "keel_health": keel.Health,
    "keel_button": keel.Button,
    "keel_led": keel.Led,
    "keel_input": keel.SafetyInput,
    "keel_capability": keel.Capability,
    "keel_backend": keel.Backend,
    "keel_open_flag": keel.OpenFlag,
    "keel_open_warning": keel.OpenWarning,
    "keel_resource": keel.OpenResource,
    "keel_fault_cause": keel.FaultCause,
    "keel_fault_flag": keel.FaultFlag,
    "keel_snapshot_flag": keel.SnapshotFlag,
    "keel_sensor_power": keel.SensorPower,
    "keel_timing_class": keel.TimingClass,
    "keel_range_status": keel.RangeStatus,
    "keel_cycle_command": keel.CycleCommand,
    "keel_cycle_flag": keel.CycleFlag,
    "keel_sim_item": keel.SimItem,
    "keel_sim_flag": keel.SimFlag,
}
CONSTANT_GROUPS = {"keel_abi": keel.abi, "keel_limit": keel.limits}


def abi_tools() -> ModuleType:
    """Put tests/abi and tests/docs of the libkeel tree on the path, without writing bytecode there.

    Returns tests/abi/check_constants.py; tests/docs/check_header_docs.py is importable afterwards.
    """
    assert SOURCE is not None
    sys.dont_write_bytecode = True
    for path in (str(SOURCE / "tests" / "abi"), str(SOURCE / "tests" / "docs")):
        if path not in sys.path:
            sys.path.insert(0, path)
    import check_constants

    return check_constants


@pytest.fixture(scope="module")
def header_text() -> str:
    """keel.h preprocessed as binding generators read it: cc -E -P -DKEEL_API= with plain <stdint.h>."""
    check_constants = abi_tools()
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        pytest.skip("needs a C compiler for the preprocessor")
    with tempfile.TemporaryDirectory() as fake:
        Path(fake, "stdint.h").write_text(check_constants.FAKE_STDINT, encoding="utf-8")
        Path(fake, "stddef.h").write_text(check_constants.FAKE_STDDEF, encoding="utf-8")
        assert SOURCE is not None
        text: str = check_constants.preprocess(compiler, str(SOURCE / "include" / "keel" / "keel.h"), fake)
    return text


@pytest.fixture(scope="module")
def header_enums(header_text: str) -> dict[str, list[tuple[str, int]]]:
    """{enum: [(enumerator, value)]} evaluated from the preprocessed header by check_constants."""
    check_constants = abi_tools()
    known: dict[str, int] = {}
    enums = {}
    for name, body in re.findall(r"\benum\s+(\w+)\s*\{(.*?)\}\s*;", header_text, re.S):
        enums[name] = check_constants.parse_enums(f"enum {name} {{{body}}};", known)
    return enums


def layout_pins() -> dict[str, tuple[dict[str, int], int]]:
    """{struct: ({field: offset}, size)} from the FIRST/NEXT/LAST lines of tests/abi/layout.c."""
    assert SOURCE is not None
    pins: dict[str, tuple[dict[str, int], int]] = {}
    for line in (SOURCE / "tests" / "abi" / "layout.c").read_text(encoding="utf-8").splitlines():
        first = re.match(r"FIRST\((keel_\w+), (\w+)\)", line)
        following = re.match(r"NEXT\((keel_\w+), \w+, (\w+), (\d+)\)", line)
        last = re.match(r"LAST\((keel_\w+), \w+, (\d+)\)", line)
        if first:
            pins[first.group(1)] = ({first.group(2): 0}, 0)
        elif following:
            pins[following.group(1)][0][following.group(2)] = int(following.group(3))
        elif last:
            pins[last.group(1)] = (pins[last.group(1)][0], int(last.group(2)))
    return pins


def c_type(declaration: str) -> Any:
    """The ctypes type a correct binding uses for one C parameter or return type."""
    text = re.sub(r"\bconst\b", "", declaration).strip()
    pointers = text.count("*")
    base = text.replace("*", " ").split()[0]
    scalars = {"uint32_t": ctypes.c_uint32, "int32_t": ctypes.c_int32, "uint64_t": ctypes.c_uint64}
    if base == "keel_device":
        return ctypes.c_void_p if pointers == 1 else ctypes.POINTER(ctypes.c_void_p)
    if base == "char" and pointers == 1:
        return ctypes.c_void_p
    target = scalars.get(base) or _generated.STRUCTS[base]
    return ctypes.POINTER(target) if pointers == 1 else target


class TestLayout:
    def test_struct_sizes_and_offsets_equal_the_layout_pins(self) -> None:
        pins = layout_pins()
        assert set(_generated.STRUCTS) <= set(pins)
        for name, struct in _generated.STRUCTS.items():
            offsets, size = pins[name]
            assert ctypes.sizeof(struct) == size, name
            assert [field[0] for field in struct._fields_] == list(offsets), name
            for field, offset in offsets.items():
                assert getattr(struct, field).offset == offset, f"{name}.{field}"

    def test_a_new_struct_carries_its_own_size(self) -> None:
        for struct in _generated.STRUCTS.values():
            instance = struct()
            assert instance.struct_size == ctypes.sizeof(struct)
            assert bytes(instance)[4:] == bytes(ctypes.sizeof(struct) - 4)
            assert ctypes.sizeof(struct) <= keel.limits.STRUCT_SIZE_MAX


class TestPrototypes:
    def test_prototypes_equal_the_header_declarations(self, header_text: str) -> None:
        declared = re.findall(r"([\w\s\*]+?)\b(keel_\w+)\s*\(([^)]*)\)\s*;", header_text)
        assert [item[1] for item in declared] == list(_generated.PROTOTYPES)
        for result, name, params in declared:
            restype, argtypes = _generated.PROTOTYPES[name]
            assert restype is c_type(result), name
            wanted = (
                []
                if params.strip() == "void"
                else [c_type(re.sub(r"\w+$", "", part.strip())) for part in params.split(",")]
            )
            assert argtypes == wanted, name

    def test_the_bound_function_list_equals_symbols_txt(self) -> None:
        assert SOURCE is not None
        text = (SOURCE / "tests" / "abi" / "symbols.txt").read_text(encoding="utf-8")
        symbols = [line.strip() for line in text.splitlines() if line.strip() and not line.startswith("#")]
        assert list(_generated.PROTOTYPES) == symbols


class TestConstants:
    def test_every_enum_equals_the_header(self, header_enums: dict[str, list[tuple[str, int]]]) -> None:
        assert len(header_enums) == 25
        assert sum(len(items) for items in header_enums.values()) == 195
        assert set(header_enums) == set(ENUM_TYPES) | set(CONSTANT_GROUPS)
        for name, cls in ENUM_TYPES.items():
            prefix = _prefix(cls)
            expected = {enumerator[len(prefix) :]: value for enumerator, value in header_enums[name]}
            assert {key: member.value for key, member in cls.__members__.items()} == expected, name

    def test_member_names_follow_the_naming_rule(self, header_enums: dict[str, list[tuple[str, int]]]) -> None:
        prefixes = {cls: _prefix(cls) for cls in ENUM_TYPES.values()}
        for name, cls in ENUM_TYPES.items():
            names = [enumerator[len(prefixes[cls]) :] for enumerator, _ in header_enums[name]]
            assert names == list(cls.__members__), name

    def test_constant_groups_equal_the_header(self, header_enums: dict[str, list[tuple[str, int]]]) -> None:
        for name, module in CONSTANT_GROUPS.items():
            prefix = "KEEL_ABI_" if name == "keel_abi" else "KEEL_"
            expected = {enumerator[len(prefix) :]: value for enumerator, value in header_enums[name]}
            assert sorted(module.__all__) == sorted(expected), name
            for member, value in expected.items():
                assert getattr(module, member) == value, member

    def test_no_member_is_named_like_the_sentinel(self) -> None:
        for cls in ENUM_TYPES.values():
            for member in cls.__members__:
                assert member != "UNRECOGNIZED"
                assert not member.startswith("UNRECOGNIZED_")

    def test_kinds(self) -> None:
        for cls in ENUM_TYPES.values():
            assert issubclass(cls, enum.IntEnum) or issubclass(cls, enum.IntFlag)
            if issubclass(cls, enum.IntFlag):
                assert cls._boundary_ is enum.KEEP


class TestAgainstTheLibrary:
    def test_status_names_equal_the_librarys(self, natives: object) -> None:
        for status in keel.Status:
            assert keel.status_name(status) == "KEEL_" + status.name

    def test_cause_names_equal_the_librarys(self, natives: object) -> None:
        for cause in keel.FaultCause:
            assert keel.fault_cause_name(cause) == cause.name

    def test_status_classes_equal_the_header_tags(self, natives: object) -> None:
        abi_tools()
        import check_header_docs

        assert SOURCE is not None
        tags = check_header_docs.status_classes(check_header_docs.Header(str(SOURCE / "include" / "keel" / "keel.h")))
        assert len(tags) == len(keel.Status)
        for name, tag in tags.items():
            status = keel.Status[name[len("KEEL_") :]]
            assert keel.status_class(status) == keel.StatusClass[tag], name

    def test_abi_version_equals_the_generated_constant(self, library: Any) -> None:
        assert library.abi_version() == keel.abi.VERSION


def test_keelgen_check_passes() -> None:
    """keelgen.py --check regenerates every Python file in memory and finds no difference."""
    assert SOURCE is not None
    keelgen = SOURCE.parent / "bindings" / "tools" / "keelgen.py"
    if not keelgen.is_file():
        pytest.skip("keelgen.py is not part of this tree")
    pytest.importorskip("pycparser")
    result = subprocess.run(
        [sys.executable, str(keelgen), "--check", "--lang", "python"],
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "keelgen: OK" in result.stdout


def _prefix(cls: type[enum.Enum]) -> str:
    prefixes = {
        keel.Status: "KEEL_",
        keel.StatusClass: "KEEL_CLASS_",
        keel.State: "KEEL_STATE_",
        keel.AckPhase: "KEEL_ACK_",
        keel.Health: "KEEL_HEALTH_",
        keel.Button: "KEEL_BUTTON_",
        keel.Led: "KEEL_LED_",
        keel.SafetyInput: "KEEL_INPUT_",
        keel.Capability: "KEEL_CAP_",
        keel.Backend: "KEEL_BACKEND_",
        keel.OpenFlag: "KEEL_OPEN_",
        keel.OpenWarning: "KEEL_OPEN_WARN_",
        keel.OpenResource: "KEEL_RESOURCE_",
        keel.FaultCause: "KEEL_CAUSE_",
        keel.FaultFlag: "KEEL_FAULT_",
        keel.SnapshotFlag: "KEEL_SNAP_",
        keel.SensorPower: "KEEL_SENSOR_",
        keel.TimingClass: "KEEL_TIMING_",
        keel.RangeStatus: "KEEL_RANGE_",
        keel.CycleCommand: "KEEL_CYCLE_",
        keel.CycleFlag: "KEEL_CYCLE_",
        keel.SimItem: "KEEL_SIM_",
        keel.SimFlag: "KEEL_SIM_",
    }
    return prefixes[cls]
