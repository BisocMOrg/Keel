# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Building exceptions from libkeel status codes, for Device, ControlLoop and Cycle.

error_for() chooses the exception type by the class libkeel reports for a status; refused() builds an error the
binding raises itself, before any foreign call.
"""

from __future__ import annotations

import ctypes
import os
from typing import TYPE_CHECKING

from keel._generated import OpenResource
from keel._internal import native_library
from keel._internal._generated import FaultStruct, convert_fault, keel_get_fault

from ._generated import Status, StatusClass
from .exceptions import (
    AgainError,
    ConflictError,
    HandleError,
    InvalidArgumentError,
    KeelError,
    PeripheralError,
    RejectedError,
    SafetyFaultError,
    StateError,
    UnsupportedError,
)

if TYPE_CHECKING:
    from keel._generated import OpenReport
    from keel.state import Snapshot

__all__ = ["error_for", "refused"]

_REJECTED: dict[int, type[RejectedError]] = {
    Status.E_INVALID_ARGUMENT: InvalidArgumentError,
    Status.E_STATE: StateError,
    Status.E_AGAIN: AgainError,
    Status.E_UNSUPPORTED: UnsupportedError,
    Status.E_CONFLICT: ConflictError,
}


def error_for(code: int, function: str, *, token: int | None = None, snapshot: Snapshot | None = None) -> KeelError:
    """Return the exception for a non-OK status, chosen by the status class libkeel reports.

    For the FAULT class, and for any class this binding does not know, the fault record of the device identified by
    token is read first. keel_open() failures never come here: they always raise SetupError with the report.
    """
    library = native_library.get()
    status_class = library.status_class(code)
    if status_class == StatusClass.REJECTED:
        return _REJECTED.get(code, RejectedError)(code, function, snapshot=snapshot)
    if status_class == StatusClass.PERIPHERAL:
        return PeripheralError(code, function, snapshot=snapshot)
    if status_class == StatusClass.HANDLE:
        return HandleError(code, function, snapshot=snapshot)
    # FAULT, an OK or SETUP class that keel.h rules out here, or a class a newer library added: read as FAULT, the
    # conservative interpretation libkeel itself applies to undefined codes.
    fault = None
    if token is not None:
        record = FaultStruct()
        if keel_get_fault(library.natives, token, ctypes.byref(record)) == Status.OK:
            fault = convert_fault(record)
    return SafetyFaultError(code, function, fault=fault, snapshot=snapshot)


def refused(error_type: type[KeelError], function: str, reason: str) -> KeelError:
    """Return an error the binding raises itself, with the status libkeel would have returned.

    The message says that the binding refused the call and that no foreign call was made.
    """
    code = Status.E_INVALID_ARGUMENT if issubclass(error_type, InvalidArgumentError) else Status.E_INVALID_HANDLE
    return error_type(code, function, detail=f"; the binding refused the call: {reason}; no foreign call was made")


def _describe_report(report: OpenReport) -> str:
    """Return the details of an open report for the message of SetupError, each part after "; "."""
    parts = []
    if report.resource != OpenResource.NONE:
        parts.append(f"resource {report.resource.name}")
    if report.line >= 0 and report.consumer:
        parts.append(f"BCM{report.line} is used by {report.consumer}")
    elif report.line >= 0:
        parts.append(f"BCM{report.line}")
    elif report.consumer:
        parts.append(f"used by {report.consumer}")
    if report.os_error:
        parts.append(f"OS error {report.os_error} ({os.strerror(report.os_error)})")
    if report.path:
        parts.append(f"path {report.path}")
    if report.hint:
        parts.append(f"remedy: {report.hint}")
    return "".join("; " + part for part in parts)
