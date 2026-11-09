# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The exception hierarchy and the status codes and classes it maps from.

Every status belongs to one class (keel_status_class), and the class tells what state the device is in after the
call. Each class maps to one exception type; the five REJECTED subclasses refine RejectedError without changing it,
and InvalidArgumentError is also a ValueError, the exception Python uses for a bad argument:

| Exception | Class | Device after the call |
|---|---|---|
| RejectedError and its subclasses | REJECTED | unchanged, except that a refused motion request while ARMED coasts |
| SafetyFaultError | FAULT (also undefined codes and unknown classes) | latched FAULTED; Category-0 stop done |
| PeripheralError | PERIPHERAL | safety state unchanged |
| SetupError | any failed keel_open | no device |
| HandleError | HANDLE, or a device the binding has closed | unusable by this caller |
| LibraryError | loading, ABI or symbol failure | no status |

A TypeError or OverflowError from a device method means that the binding refused an argument and no foreign call
was made. The types are defined in keel.errors.exceptions; keel.errors.error_factory builds them from statuses.
"""

from ._generated import Status, StatusClass
from .exceptions import (
    AgainError,
    ConflictError,
    HandleError,
    InvalidArgumentError,
    KeelError,
    LibraryError,
    PeripheralError,
    RejectedError,
    SafetyFaultError,
    SetupError,
    StateError,
    UnsupportedError,
)

__all__ = [
    "AgainError",
    "ConflictError",
    "HandleError",
    "InvalidArgumentError",
    "KeelError",
    "LibraryError",
    "PeripheralError",
    "RejectedError",
    "SafetyFaultError",
    "SetupError",
    "StateError",
    "Status",
    "StatusClass",
    "UnsupportedError",
]
