# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The generated camera declarations against camera.h: struct layouts, prototypes, enumerators and the symbols."""

from __future__ import annotations

import ctypes
import enum
import re
from typing import Any

import pytest

from keel import camera
from keel._internal.camera import _generated
from support.environment import libkeel_source

SOURCE = libkeel_source()
pytestmark = pytest.mark.skipif(SOURCE is None, reason="needs the libkeel source tree (set KEEL_SOURCE_DIR)")

ENUM_TYPES: dict[type[enum.Enum], str] = {
    camera.PixelFormat: "KEEL_CAMERA_FORMAT_",
    camera.CameraFlag: "KEEL_CAMERA_",
    camera.AutofocusMode: "KEEL_CAMERA_AF_",
    camera.AutofocusRange: "KEEL_CAMERA_AF_RANGE_",
    camera.AutofocusState: "KEEL_CAMERA_AF_STATE_",
    camera.WhiteBalance: "KEEL_CAMERA_AWB_",
    camera.CameraInfoFlag: "KEEL_CAMERA_INFO_",
    camera.Codec: "KEEL_CAMERA_CODEC_",
    camera.H264Profile: "KEEL_CAMERA_H264_PROFILE_",
    camera.EncoderFlag: "KEEL_CAMERA_",
    camera.ChunkFlag: "KEEL_CAMERA_ENCODED_",
    camera.StillFlag: "KEEL_CAMERA_STILL_",
    camera.CaptureState: "KEEL_CAMERA_STATE_",
    camera.OutputState: "KEEL_CAMERA_OUTPUT_",
    camera.CameraResource: "KEEL_CAMERA_RESOURCE_",
}

# SPEC section 26.2: the frozen struct sizes.
SIZES = {
    "keel_camera_error": 528,
    "keel_camera_info": 248,
    "keel_camera_mode": 40,
    "keel_camera_options": 72,
    "keel_camera_stream_info": 64,
    "keel_camera_frame_info": 64,
    "keel_camera_encoder_options": 32,
    "keel_camera_encoded_info": 32,
    "keel_camera_output_options": 24,
    "keel_camera_output_stats": 304,
    "keel_camera_stats": 72,
    "keel_camera_still_options": 24,
    "keel_camera_still_info": 32,
}


def golden() -> dict[str, int]:
    """{enumerator: value} of tests/abi/camera_constants.golden, the frozen values of camera ABI 1.x.

    The minor version and the packed version are not frozen: they grow with each minor release.
    """
    assert SOURCE is not None
    values = {}
    for line in (SOURCE / "tests" / "abi" / "camera_constants.golden").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            name, value = line.split("=")
            values[name] = int(value)
    return values


def layout_pins() -> dict[str, tuple[dict[str, int], int]]:
    """{struct: ({field: offset}, size)} from the FIRST/NEXT/LAST lines of tests/abi/camera_layout.c."""
    assert SOURCE is not None
    pins: dict[str, tuple[dict[str, int], int]] = {}
    for line in (SOURCE / "tests" / "abi" / "camera_layout.c").read_text(encoding="utf-8").splitlines():
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


def test_struct_sizes_and_offsets_equal_the_layout_pins() -> None:
    pins = layout_pins()
    assert set(_generated.STRUCTS) == set(pins) == set(SIZES)
    for name, struct in _generated.STRUCTS.items():
        offsets, size = pins[name]
        assert ctypes.sizeof(struct) == size == SIZES[name], name
        assert [field[0] for field in struct._fields_] == list(offsets), name
        for field, offset in offsets.items():
            assert getattr(struct, field).offset == offset, f"{name}.{field}"
        instance = struct()
        assert instance.struct_size == size
        assert bytes(instance)[4:] == bytes(size - 4)


def test_every_enumerator_equals_the_golden_file() -> None:
    values = golden()
    assert len(values) == 87
    generated = {}
    for cls, prefix in ENUM_TYPES.items():
        for member in cls:
            generated[prefix + member.name] = member.value
    for module, prefix in ((camera.abi, "KEEL_CAMERA_ABI_"), (camera.limits, "KEEL_CAMERA_")):
        for name in module.__all__:
            generated[prefix + name] = getattr(module, name)
    assert len(generated) == 89
    assert generated.pop("KEEL_CAMERA_ABI_MINOR") == 0
    assert generated.pop("KEEL_CAMERA_ABI_VERSION") == 0x0001_0000
    assert generated == values


def test_the_open_enums_keep_unknown_values_and_the_flags_keep_unknown_bits() -> None:
    assert camera.Codec(9).name == "UNRECOGNIZED_9"
    assert camera.ChunkFlag(0x80).value == 0x80
    for cls in ENUM_TYPES:
        assert issubclass(cls, (enum.IntEnum, enum.IntFlag))


def test_the_bound_functions_equal_camera_symbols_txt() -> None:
    assert SOURCE is not None
    text = (SOURCE / "tests" / "abi" / "camera_symbols.txt").read_text(encoding="utf-8")
    symbols = [line.strip() for line in text.splitlines() if line.strip() and not line.startswith("#")]
    assert list(_generated.PROTOTYPES) == symbols
    assert len(symbols) == 25


def c_type(declaration: str) -> Any:
    """The ctypes type a correct binding uses for one C parameter or return type of camera.h."""
    text = re.sub(r"\bconst\b", "", declaration).strip()
    pointers = text.count("*")
    base = text.replace("*", " ").split()[0]
    scalars = {"uint32_t": ctypes.c_uint32, "int32_t": ctypes.c_int32, "uint64_t": ctypes.c_uint64}
    if base in ("keel_camera", "keel_camera_list"):
        return ctypes.c_void_p if pointers == 1 else ctypes.POINTER(ctypes.c_void_p)
    if base in ("char", "uint8_t") and pointers == 1:
        return ctypes.c_void_p
    target = scalars.get(base) or _generated.STRUCTS[base]
    return ctypes.POINTER(target) if pointers == 1 else target


def test_prototypes_equal_the_header_declarations() -> None:
    assert SOURCE is not None
    text = (SOURCE / "include" / "keel" / "camera.h").read_text(encoding="utf-8")
    declared = re.findall(r"KEEL_API\s+([\w\s\*]+?)\b(keel_camera_\w+)\s*\(([^)]*)\)\s*;", text)
    assert [item[1] for item in declared] == list(_generated.PROTOTYPES)
    for result, name, params in declared:
        restype, argtypes = _generated.PROTOTYPES[name]
        assert restype is c_type(result), name
        flat = " ".join(params.split())
        wanted = [] if flat == "void" else [c_type(re.sub(r"\w+$", "", part.strip())) for part in flat.split(",")]
        assert argtypes == wanted, name
