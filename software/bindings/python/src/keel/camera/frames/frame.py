# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Frame: one raw frame, its layout and its pixels, as bytes or as a numpy array.

numpy is optional. Frame.data and Frame.to_bytes() need nothing; Frame.array() and Frame.plane() import numpy when
they are called and return views of the frame's own buffer, with no copy, wherever the layout allows.
"""

from __future__ import annotations

import ctypes
from typing import TYPE_CHECKING, Any

from keel._internal.camera import _generated as g
from keel._internal.camera import native_library
from keel.camera.errors.error_factory import camera_error
from keel.errors._generated import Status

from ._generated import FrameInfo, PixelFormat

if TYPE_CHECKING:
    import numpy as np
    from numpy.typing import NDArray

__all__ = ["Frame"]

_PACKED = (PixelFormat.RGB888, PixelFormat.BGR888)


def _numpy(caller: str) -> Any:
    """Import numpy for caller, or raise ModuleNotFoundError with the install command."""
    try:
        import numpy
    except ModuleNotFoundError as error:
        raise ModuleNotFoundError(
            f"Frame.{caller}() needs numpy: sudo apt install python3-numpy, or pip install numpy"
        ) from error
    return numpy


class Frame:
    """One raw frame: its FrameInfo (format, size, strides, offsets, sequence number and timestamp) and its bytes.

    Rows may be padded: plane i starts at info.offset<i> and its rows are info.stride<i> bytes apart. array() and
    plane() give numpy views that follow the layout; to_bytes() gives the rows without padding. The colour space is
    full-range BT.601 (sYCC), the JPEG colour space.

    A frame owns its buffer: it stays valid after the camera is closed, and later frames never overwrite it. Frames
    from Camera.read_frame() are writable, so a program may draw on array() in place.
    """

    __slots__ = ("_buffer", "info")

    info: FrameInfo
    """The layout, sequence number and timestamp keel_camera_read_frame wrote."""

    def __init__(self, info: FrameInfo, data: bytes | bytearray | memoryview) -> None:
        """Wrap data, laid out as info describes; data must hold at least info.bytes bytes."""
        size = memoryview(data).nbytes
        if size < info.bytes:
            raise ValueError(f"the frame needs {info.bytes} bytes, but data has {size}")
        self.info = info
        self._buffer = data

    def __repr__(self) -> str:
        """Describe the frame without its pixels."""
        info = self.info
        return (
            f"<keel.camera.frames.Frame {info.width}x{info.height} {info.format.name}, sequence {info.sequence}, "
            f"{info.bytes} bytes>"
        )

    @property
    def width(self) -> int:
        """Width in pixels."""
        return self.info.width

    @property
    def height(self) -> int:
        """Height in lines."""
        return self.info.height

    @property
    def pixel_format(self) -> PixelFormat:
        """The pixel format: I420, RGB888 or BGR888."""
        return self.info.format

    @property
    def sequence(self) -> int:
        """The sensor's frame sequence number; a gap is a frame the camera dropped."""
        return self.info.sequence

    @property
    def timestamp_ns(self) -> int:
        """Start of exposure, CLOCK_MONOTONIC nanoseconds (the clock of the robot's control loop)."""
        return self.info.timestamp_ns

    @property
    def data(self) -> memoryview:
        """The frame's info.bytes bytes as libkeel-camera wrote them, padding included (a read-only view)."""
        return memoryview(self._buffer)[: self.info.bytes].toreadonly()

    def __bytes__(self) -> bytes:
        """Return the frame's bytes, padding included."""
        return bytes(self.data)

    def _pointer(self) -> Any:
        """Return the buffer as a foreign-call argument: a ctypes array over it, or bytes as they are."""
        if isinstance(self._buffer, bytes):
            return self._buffer
        size = memoryview(self._buffer).nbytes
        try:
            return (ctypes.c_ubyte * size).from_buffer(self._buffer)
        except TypeError:  # a read-only memoryview
            return bytes(self._buffer)

    def _planes(self) -> list[tuple[int, int, int, int]]:
        """(offset, stride, row bytes, rows) of each plane."""
        info = self.info
        if info.format == PixelFormat.I420:
            half_width, half_height = (info.width + 1) // 2, (info.height + 1) // 2
            return [
                (info.offset0, info.stride0, info.width, info.height),
                (info.offset1, info.stride1, half_width, half_height),
                (info.offset2, info.stride2, half_width, half_height),
            ]
        if info.format in _PACKED:
            return [(info.offset0, info.stride0, info.width * 3, info.height)]
        raise ValueError(f"the pixel format {info.format.name} is not one this binding knows")

    def to_bytes(self) -> bytes:
        """Return the pixels without row padding: the planes one after the other, each row as wide as the image.

        This is the layout of keel-camera frames and of FFmpeg's rawvideo yuv420p, rgb24 and bgr24.
        """
        view = memoryview(self._buffer)
        rows = []
        for offset, stride, width, height in self._planes():
            if stride == width:
                rows.append(view[offset : offset + width * height])
            else:
                rows.extend(view[offset + row * stride : offset + row * stride + width] for row in range(height))
        return b"".join(rows)

    def plane(self, index: int) -> NDArray[np.uint8]:
        """One plane of an I420 frame as a 2-D numpy view (rows, columns), with no copy: 0 is Y, 1 is U, 2 is V.

        The Y plane is the frame's brightness, a greyscale image. Raises ValueError for other formats or indices and
        ModuleNotFoundError without numpy.
        """
        if self.info.format != PixelFormat.I420:
            raise ValueError(f"plane() needs an I420 frame; this one is {self.info.format.name} (use array())")
        planes = self._planes()
        if not 0 <= index < len(planes):
            raise ValueError(f"an I420 frame has planes 0, 1 and 2, not {index}")
        numpy = _numpy("plane")
        offset, stride, width, height = planes[index]
        result: NDArray[np.uint8] = numpy.ndarray(
            (height, width), dtype=numpy.uint8, buffer=self._buffer, offset=offset, strides=(stride, 1)
        )
        return result

    def array(self) -> NDArray[np.uint8]:
        """Return the pixels as a numpy array of uint8.

        RGB888 and BGR888 frames give an array of shape (height, width, 3), a view of the frame's buffer with no copy
        (BGR888 is the order OpenCV expects). I420 frames give shape (height * 3 // 2, width), the layout of OpenCV's
        COLOR_YUV2BGR_I420: a view when the rows have no padding, else a packed copy. Raises ModuleNotFoundError
        without numpy.
        """
        numpy = _numpy("array")
        info = self.info
        if info.format in _PACKED:
            packed: NDArray[np.uint8] = numpy.ndarray(
                (info.height, info.width, 3),
                dtype=numpy.uint8,
                buffer=self._buffer,
                offset=info.offset0,
                strides=(info.stride0, 3, 1),
            )
            return packed
        planes = self._planes()
        contiguous = all(stride == width for _, stride, width, _ in planes) and all(
            planes[number + 1][0] == planes[number][0] + planes[number][2] * planes[number][3] for number in range(2)
        )
        if contiguous and info.width % 2 == 0 and info.height % 2 == 0:
            view: NDArray[np.uint8] = numpy.ndarray(
                (info.height * 3 // 2, info.width), dtype=numpy.uint8, buffer=self._buffer, offset=info.offset0
            )
            return view
        copy: NDArray[np.uint8] = numpy.frombuffer(self.to_bytes(), dtype=numpy.uint8)
        return copy.reshape(info.height * 3 // 2, info.width)

    def convert(self, pixel_format: PixelFormat) -> Frame:
        """Convert an I420 frame to RGB888 or BGR888 on the CPU (keel_camera_convert_i420) and return the new frame.

        Opening the camera with pixel_format RGB888 or BGR888 instead lets the image signal processor convert at no
        CPU cost; this is for a program that needs both. The new frame has the same size, sequence number and
        timestamp, and rows without padding. Raises ValueError unless this frame is I420 and pixel_format is RGB888
        or BGR888.
        """
        info = self.info
        if info.format != PixelFormat.I420:
            raise ValueError(f"convert() needs an I420 frame; this one is {info.format.name}")
        target = PixelFormat(pixel_format)
        if target not in _PACKED:
            raise ValueError(f"convert() converts to RGB888 or BGR888, not {target.name}")
        layout = g.FrameInfoStruct()
        for name in (
            "format",
            "width",
            "height",
            "plane_count",
            "stride0",
            "stride1",
            "stride2",
            "offset0",
            "offset1",
            "offset2",
            "bytes",
            "sequence",
            "timestamp_ns",
        ):
            setattr(layout, name, getattr(info, name))
        stride = info.width * 3
        size = stride * info.height
        output = bytearray(size)
        code = g.keel_camera_convert_i420(
            native_library.get().natives,
            self._pointer(),
            ctypes.byref(layout),
            (ctypes.c_ubyte * size).from_buffer(output),
            size,
            target,
            stride,
        )
        if code != Status.OK:
            raise camera_error(code, "keel_camera_convert_i420")
        converted = FrameInfo(
            format=target,
            width=info.width,
            height=info.height,
            plane_count=1,
            stride0=stride,
            stride1=0,
            stride2=0,
            offset0=0,
            offset1=0,
            offset2=0,
            bytes=size,
            sequence=info.sequence,
            timestamp_ns=info.timestamp_ns,
        )
        return Frame(converted, output)
