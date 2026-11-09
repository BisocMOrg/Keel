# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The exception hierarchy: one type per status class, the REJECTED subclasses, and LibraryError.

At module level this imports nothing from keel outside keel.errors, so the loader can import it; the library is
reached only when an error is created.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from ._generated import Status, StatusClass

if TYPE_CHECKING:
    from keel._generated import OpenReport
    from keel.state import Fault, Snapshot

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
    "UnsupportedError",
]


class LibraryError(Exception):
    """libkeel could not be loaded, lacks a symbol, has an incompatible ABI, or broke the string contract.

    It carries no status code. The message names the library, both versions where they matter, and the remedy.
    """


def _rebuild(cls: type[KeelError], args: tuple[Any, ...], state: dict[str, Any]) -> KeelError:
    """Recreate a pickled KeelError without calling into the library."""
    error = cls.__new__(cls)
    Exception.__init__(error, *args)
    error.__dict__.update(state)
    return error


class KeelError(Exception):
    """Base of every status error; the binding never raises it directly.

    Attributes:
        status: the status as a Status member (an UNRECOGNIZED_<code> pseudo-member for a code this binding does not
            know).
        code: the raw status code.
        name: keel_status_name(code), for example "KEEL_E_CONFLICT".
        description: keel_status_string(code), one sentence that names the remedy.
        status_class: keel_status_class(code), the class that chose the exception type.
        function: the C function that returned the status, for example "keel_set_motors".
        snapshot: the snapshot keel_cycle() wrote alongside the status, or None.

    """

    status: Status
    code: int
    name: str
    description: str
    status_class: StatusClass
    function: str
    snapshot: Snapshot | None

    def __init__(self, code: int, function: str, *, snapshot: Snapshot | None = None, detail: str = "") -> None:
        """Describe status code returned by function; detail is appended to the message."""
        from keel._internal import native_library  # imported here: the loader imports this module

        library = native_library.get()
        self.code = code
        self.status = Status(code)
        self.name = library.status_name(code)
        self.description = library.status_string(code)
        self.status_class = library.status_class(code)
        self.function = function
        self.snapshot = snapshot
        text = self.description.rstrip(".") if detail else self.description
        super().__init__(f"{function}: {self.name}: {text}{detail}")

    def __reduce__(self) -> tuple[Any, ...]:
        """Pickle the error with its attributes, without calling into the library when it is restored."""
        return (_rebuild, (type(self), self.args, dict(self.__dict__)))


class RejectedError(KeelError):
    """REJECTED class: refused with no effect on the device.

    The one documented exception: a motion request refused for its arguments or capability while ARMED coasts both
    motors and drops the stored command. No fault is latched and the heartbeat continues. A REJECTED code this
    binding does not know raises RejectedError itself.
    """


class InvalidArgumentError(RejectedError, ValueError):
    """KEEL_E_INVALID_ARGUMENT: a bad struct_size, an unknown enumerator or flag, or a value out of range.

    Also raised by the control loop, without a foreign call, for a duty outside -DUTY_MAX..DUTY_MAX.
    """


class StateError(RejectedError):
    """KEEL_E_STATE: not allowed in the current state, including the re-arm hold-off after a fault."""


class AgainError(RejectedError):
    """KEEL_E_AGAIN: temporarily refused; retry later. Never a safety refusal."""


class UnsupportedError(RejectedError):
    """KEEL_E_UNSUPPORTED: a capability this device lacks, a simulator call on hardware, or an oversize input."""


class ConflictError(RejectedError):
    """KEEL_E_CONFLICT: a go call from a thread other than the control owner, or a clash with an active instruction."""


class SafetyFaultError(KeelError):
    """FAULT class: the device is latched FAULTED; a Category-0 stop was done and the heartbeat stopped.

    Also raised for status codes libkeel does not define (keel_status_class reads them as FAULT) and for a class value
    this binding does not know. fault holds the record keel_get_fault() returned before the error was raised, or None
    when that call failed (for example because the device was closed concurrently). Only clear_fault() leaves FAULTED.
    """

    fault: Fault | None

    def __init__(
        self, code: int, function: str, *, fault: Fault | None, snapshot: Snapshot | None = None, detail: str = ""
    ) -> None:
        """Describe status code returned by function, with the fault record if one could be read."""
        self.fault = fault
        if fault is not None:
            detail = f"; cause {fault.cause.name}, detail {fault.detail}{detail}"
        super().__init__(code, function, snapshot=snapshot, detail=detail)


class PeripheralError(KeelError):
    """PERIPHERAL class: a non-safety peripheral (RGB LED driver, ultrasonic sensor) failed; safety state unchanged."""


class SetupError(KeelError):
    """keel_open() created no device, whatever the class of its status.

    report is the open report, which libkeel writes for every status; its resource, line, consumer, os_error, path
    and hint describe the failure and its remedy.
    """

    report: OpenReport

    def __init__(self, code: int, function: str, *, report: OpenReport) -> None:
        """Describe the failed open, with the details of its report."""
        from .error_factory import _describe_report  # imported here: error_factory imports this module

        self.report = report
        super().__init__(code, function, detail=_describe_report(report))


class HandleError(KeelError):
    """HANDLE class: the handle is not usable by this caller (closed, stale, or inherited through fork()).

    Also raised, with KEEL_E_INVALID_HANDLE and without a foreign call, for a call on a device the binding has closed
    or is closing. Control loops end when they see it.
    """
