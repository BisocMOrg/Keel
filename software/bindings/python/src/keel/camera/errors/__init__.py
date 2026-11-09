# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The camera's exceptions and the error record they carry.

Every camera call returns a keel.h status, and every failure records an ErrorReport: the resource involved, the OS
error, the path and a one-line message that names the remedy. Each status that needs its own handling maps to one
exception type; the others raise CameraError itself:

| Exception | Status | Usual reason |
|---|---|---|
| CameraArgumentError (also ValueError) | KEEL_E_INVALID_ARGUMENT | an option out of range |
| CameraUnsupportedError | KEEL_E_UNSUPPORTED | focus on a fixed-focus camera; an output this build lacks |
| CameraStateError | KEEL_E_STATE | reading before start(); a still while capturing |
| CameraTimeoutError (also TimeoutError) | KEEL_E_AGAIN | no frame or chunk within the timeout |
| CameraThreadError | KEEL_E_CONFLICT | a call from another thread than the one that opened the camera |
| CameraNotFoundError | KEEL_E_NOT_FOUND | no camera; a receiver that does not answer |
| CameraBusyError | KEEL_E_IN_USE | another program has the camera; the port is taken |
| CameraPermissionError (also PermissionError) | KEEL_E_PERMISSION | the user is not in the video group |
| CameraClosedError | KEEL_E_INVALID_HANDLE, KEEL_E_NOT_OWNER | the camera is closed |
| CameraError | any other | the camera or the encoder failed |

CameraLibraryError (a keel.LibraryError) means that libkeel-camera.so.1 could not be loaded. A TypeError, ValueError
or OverflowError raised before any call means that the binding refused an argument and no foreign call was made.
"""

from ._generated import CameraResource, ErrorReport
from .exceptions import (
    CameraArgumentError,
    CameraBusyError,
    CameraClosedError,
    CameraError,
    CameraLibraryError,
    CameraNotFoundError,
    CameraPermissionError,
    CameraStateError,
    CameraThreadError,
    CameraTimeoutError,
    CameraUnsupportedError,
)

__all__ = [
    "CameraArgumentError",
    "CameraBusyError",
    "CameraClosedError",
    "CameraError",
    "CameraLibraryError",
    "CameraNotFoundError",
    "CameraPermissionError",
    "CameraResource",
    "CameraStateError",
    "CameraThreadError",
    "CameraTimeoutError",
    "CameraUnsupportedError",
    "ErrorReport",
]
