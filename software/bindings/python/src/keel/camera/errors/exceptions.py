# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The camera's exception hierarchy: CameraError, a subclass per status that needs its own handling, CameraLibraryError.

At module level this imports only generated modules and keel.errors.exceptions, so the loader can import it. Nothing
here calls into a library: the status names come from the Status enum generated from keel.h.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from keel.errors._generated import Status
from keel.errors.exceptions import LibraryError

if TYPE_CHECKING:
    from ._generated import ErrorReport

__all__ = [
    "CameraArgumentError",
    "CameraBusyError",
    "CameraClosedError",
    "CameraError",
    "CameraLibraryError",
    "CameraNotFoundError",
    "CameraPermissionError",
    "CameraStateError",
    "CameraThreadError",
    "CameraTimeoutError",
    "CameraUnsupportedError",
]

# What a status means when libkeel-camera recorded no message for it, as camera.h describes the codes.
_DESCRIPTIONS = {
    Status.E_INVALID_ARGUMENT: "a bad option or argument",
    Status.E_UNSUPPORTED: "a feature this camera or build lacks",
    Status.E_STATE: "not allowed in the camera's current state",
    Status.E_AGAIN: "nothing arrived within the timeout",
    Status.E_CONFLICT: "only the thread that opened the camera may call this; close() works from any thread",
    Status.E_NOT_FOUND: "no such camera, encoder or destination",
    Status.E_IN_USE: "the camera or the port is in use",
    Status.E_PERMISSION: "a device node or file is not accessible",
    Status.E_INVALID_HANDLE: "the camera is closed",
    Status.E_NOT_OWNER: "the camera belongs to the process that opened it, not to this forked child",
    Status.E_SETUP_IO: "the camera, the encoder or an output failed",
    Status.E_PERIPHERAL: "the camera, the encoder or an output failed while running",
    Status.E_NO_MEMORY: "an allocation failed",
}


class CameraLibraryError(LibraryError):
    """libkeel-camera.so.1 could not be loaded, lacks a symbol or has an incompatible ABI.

    It is a keel.LibraryError, so one except clause covers both libraries. The message names the library and the
    remedy. Importing keel or keel.camera never raises it: the camera library is loaded on the first camera call.
    """


def status_name(code: int) -> str:
    """Return the C name of a status code, such as "KEEL_E_IN_USE", from the Status enum generated from keel.h."""
    status = Status(code)
    return f"status {code}" if status.name.startswith("UNRECOGNIZED") else f"KEEL_{status.name}"


def _rebuild(cls: type[CameraError], args: tuple[Any, ...], state: dict[str, Any]) -> CameraError:
    """Recreate a pickled CameraError."""
    error = cls.__new__(cls)
    Exception.__init__(error, *args)
    error.__dict__.update(state)
    return error


class CameraError(Exception):
    """A camera call returned a status other than OK; the base of every camera status error.

    It is raised itself for the statuses without a subclass: KEEL_E_SETUP_IO (the camera or the encoder failed to
    start), KEEL_E_PERIPHERAL (it failed while running; stop() and start() again), KEEL_E_NO_MEMORY and any code this
    binding does not know.

    Attributes:
        status: the status as a keel.errors.Status member.
        code: the raw status code.
        function: the C function that returned it, for example "keel_camera_open".
        report: the error record libkeel-camera wrote for the failure (resource, OS error, path and message), or None
            when there is none (a closed camera, or a call from another thread).
        description: the record's message, which names the remedy, or a short description of the status.

    """

    status: Status
    code: int
    function: str
    report: ErrorReport | None
    description: str

    def __init__(self, code: int, function: str, report: ErrorReport | None = None) -> None:
        """Describe status code returned by function, with the error record if there is one."""
        self.code = code
        self.status = Status(code)
        self.function = function
        self.report = report
        if report is not None and report.message:
            self.description = report.message
        else:
            self.description = _DESCRIPTIONS.get(self.status, "no further details")
        super().__init__(f"{function}: {status_name(code)}: {self.description}")

    def __reduce__(self) -> tuple[Any, ...]:
        """Pickle the error with its attributes."""
        return (_rebuild, (type(self), self.args, dict(self.__dict__)))


class CameraArgumentError(CameraError, ValueError):
    """KEEL_E_INVALID_ARGUMENT: an option or argument out of range, or a buffer too small; nothing happened.

    It is also a ValueError, the exception Python uses for a bad argument.
    """


class CameraUnsupportedError(CameraError):
    """KEEL_E_UNSUPPORTED: a feature this camera or build lacks.

    Examples: a focus option on a fixed-focus camera (Camera Module 2), an .mp4 or rtsp:// output in a build without
    FFmpeg's libavformat, an encoder for a stream larger than 1920x1920.
    """


class CameraStateError(CameraError):
    """KEEL_E_STATE: not allowed in the camera's state, such as a read before start() or a still while capturing."""


class CameraTimeoutError(CameraError, TimeoutError):
    """KEEL_E_AGAIN: nothing arrived within the timeout (frames, chunks, stills, autofocus).

    Also raised by start_output() when all KEEL_CAMERA_OUTPUTS_MAX outputs exist. It is a TimeoutError as well.
    """


class CameraThreadError(CameraError):
    """KEEL_E_CONFLICT: called from a thread other than the one that opened the camera.

    The thread that opens a camera owns it; only close() works from any thread.
    """


class CameraNotFoundError(CameraError):
    """KEEL_E_NOT_FOUND: no camera, the IPA module or the hardware encoder missing, or an unreachable destination.

    A camera index past the last camera, and a receiver that refused the connection or did not answer, are not
    found as well.
    """


class CameraBusyError(CameraError):
    """KEEL_E_IN_USE: another process or camera handle has the camera, or a listening port is taken."""


class CameraPermissionError(CameraError, PermissionError):
    """KEEL_E_PERMISSION: a camera, encoder or frame-buffer device node is not accessible, or a file cannot be made.

    The message names the remedy: the user joins the video group with sudo keel-setup --user NAME, then logs in
    again. It is a PermissionError as well.
    """


class CameraClosedError(CameraError):
    """KEEL_E_INVALID_HANDLE or KEEL_E_NOT_OWNER: the camera is closed, or belongs to the parent of a forked process.

    A read that another thread's close() woke up raises it too.
    """
