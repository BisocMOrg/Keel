# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Building camera exceptions from statuses, with the error record libkeel-camera wrote for the failure."""

from __future__ import annotations

import ctypes

from keel._internal.camera import _generated as g
from keel.errors._generated import Status

from ._generated import ErrorReport
from .exceptions import (
    CameraArgumentError,
    CameraBusyError,
    CameraClosedError,
    CameraError,
    CameraNotFoundError,
    CameraPermissionError,
    CameraStateError,
    CameraThreadError,
    CameraTimeoutError,
    CameraUnsupportedError,
)

__all__ = ["camera_error", "check", "error_from_record", "handle_error"]

_TYPES: dict[int, type[CameraError]] = {
    Status.E_INVALID_ARGUMENT: CameraArgumentError,
    Status.E_UNSUPPORTED: CameraUnsupportedError,
    Status.E_STATE: CameraStateError,
    Status.E_AGAIN: CameraTimeoutError,
    Status.E_CONFLICT: CameraThreadError,
    Status.E_NOT_FOUND: CameraNotFoundError,
    Status.E_IN_USE: CameraBusyError,
    Status.E_PERMISSION: CameraPermissionError,
    Status.E_INVALID_HANDLE: CameraClosedError,
    Status.E_NOT_OWNER: CameraClosedError,
}

# Statuses after which keel_camera_last_error cannot be read: the handle is not usable by this caller.
_NO_RECORD = frozenset({Status.E_CONFLICT, Status.E_INVALID_HANDLE, Status.E_NOT_OWNER})


def camera_error(code: int, function: str, report: ErrorReport | None = None) -> CameraError:
    """Return the exception for a non-OK status: the subclass of its status, else CameraError."""
    return _TYPES.get(code, CameraError)(code, function, report)


def error_from_record(code: int, function: str, record: g.ErrorReportStruct) -> CameraError:
    """Return the exception for a failed keel_camera_open or keel_camera_list_open, from the record it wrote."""
    report = g.convert_error_report(record)
    return camera_error(code, function, report if report.status == code else None)


def handle_error(natives: g.Natives, token: int, code: int, function: str) -> CameraError:
    """Return the exception for a failed call on a handle, with the handle's latest error record when it has one."""
    report = None
    if code not in _NO_RECORD:
        record = g.ErrorReportStruct()
        if g.keel_camera_last_error(natives, token, ctypes.byref(record)) == Status.OK and record.status == code:
            report = g.convert_error_report(record)
    return camera_error(code, function, report)


def check(natives: g.Natives, token: int, code: int, function: str) -> None:
    """Raise the exception for code unless it is KEEL_OK."""
    if code != Status.OK:
        raise handle_error(natives, token, code, function)
