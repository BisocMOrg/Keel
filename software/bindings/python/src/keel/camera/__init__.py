# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""keel.camera: the Raspberry Pi camera on the robot, through libkeel-camera.

The camera connects to the Raspberry Pi's camera connector, not to the Keel board, so it has a library of its own,
libkeel-camera.so.1 (camera.h). This package binds it the way keel binds libkeel: thin wrappers, every status raised
as an exception, the constants and structs generated from the header. It needs neither libkeel.so nor a keel.Device.
Camera Module 2 (IMX219, fixed focus) and Camera Module 3 (IMX708, autofocus; also Wide and NoIR) are supported; any
other camera libcamera drives works for capture.

    from keel import camera

    with camera.open(width=640, height=480, pixel_format=camera.PixelFormat.BGR888) as cam:
        cam.start()
        for frame in cam.frames():
            image = frame.array()        # a numpy view (480, 640, 3), BGR: the order OpenCV expects
            ...

The camera library is loaded on the first camera call (libkeel-camera.so.1, or the absolute path in
KEEL_CAMERA_LIBRARY); importing keel.camera never touches it. On the robot it comes with the libkeel-camera1 package.
numpy is optional: Frame.array() and Frame.plane() need it, Frame.data and Frame.to_bytes() do not.

keel.camera itself holds open(), list_cameras(), Camera and ListedCamera, the camera's CameraInfo, SensorMode and
CameraStats, the focus, white-balance and flip options, load() with the library metadata, and the constant modules
abi and limits. The other types are grouped by what they describe, and keel.camera re-exports them all:

| Package | Contents |
|---|---|
| keel.camera.frames | Frame, FrameInfo, PixelFormat and StreamInfo |
| keel.camera.encoding | Codec, H264Profile, EncodedChunk, ChunkInfo and their flags |
| keel.camera.outputs | OutputState and OutputStats of the files and network streams the library writes itself |
| keel.camera.stills | StillImage and StillInfo |
| keel.camera.errors | CameraError and its subclasses, CameraLibraryError, ErrorReport and CameraResource |
"""

# isort: off
# This is the import order of the camera package. errors comes first: every other module raises its exceptions.
# encoding comes before frames, whose StreamInfo names a Codec; the hand-written modules come last.
from . import errors
from .errors import (
    CameraArgumentError,
    CameraBusyError,
    CameraClosedError,
    CameraError,
    CameraLibraryError,
    CameraNotFoundError,
    CameraPermissionError,
    CameraResource,
    CameraStateError,
    CameraThreadError,
    CameraTimeoutError,
    CameraUnsupportedError,
    ErrorReport,
)
from ._generated import (
    AutofocusMode,
    AutofocusRange,
    AutofocusState,
    CameraFlag,
    CameraInfo,
    CameraInfoFlag,
    CameraStats,
    CaptureState,
    SensorMode,
    WhiteBalance,
)
from . import encoding, frames, outputs, stills
from .encoding import ChunkFlag, ChunkInfo, Codec, EncodedChunk, EncoderFlag, H264Profile
from .frames import Frame, FrameInfo, PixelFormat, StreamInfo
from .outputs import OutputState, OutputStats
from .stills import StillFlag, StillImage, StillInfo
from . import abi, limits
from .library import abi_version, load, version_string
from .listed_camera import ListedCamera
from .camera import Camera, list_cameras, open  # noqa: A004  (keel.camera.open, after keel_camera_open)

# isort: on

__all__ = [
    "AutofocusMode",
    "AutofocusRange",
    "AutofocusState",
    "Camera",
    "CameraArgumentError",
    "CameraBusyError",
    "CameraClosedError",
    "CameraError",
    "CameraFlag",
    "CameraInfo",
    "CameraInfoFlag",
    "CameraLibraryError",
    "CameraNotFoundError",
    "CameraPermissionError",
    "CameraResource",
    "CameraStateError",
    "CameraStats",
    "CameraThreadError",
    "CameraTimeoutError",
    "CameraUnsupportedError",
    "CaptureState",
    "ChunkFlag",
    "ChunkInfo",
    "Codec",
    "EncodedChunk",
    "EncoderFlag",
    "ErrorReport",
    "Frame",
    "FrameInfo",
    "H264Profile",
    "ListedCamera",
    "OutputState",
    "OutputStats",
    "PixelFormat",
    "SensorMode",
    "StillFlag",
    "StillImage",
    "StillInfo",
    "StreamInfo",
    "WhiteBalance",
    "abi",
    "abi_version",
    "encoding",
    "errors",
    "frames",
    "limits",
    "list_cameras",
    "load",
    "open",
    "outputs",
    "stills",
    "version_string",
]
