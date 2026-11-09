# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The camera's exceptions: one type per status that needs its own handling, their messages and pickling."""

from __future__ import annotations

import pickle

import pytest

import keel
from keel import camera
from keel.camera.errors.error_factory import camera_error
from keel.camera.errors.exceptions import status_name
from keel.errors import Status

TYPES = [
    (Status.E_INVALID_ARGUMENT, camera.CameraArgumentError),
    (Status.E_UNSUPPORTED, camera.CameraUnsupportedError),
    (Status.E_STATE, camera.CameraStateError),
    (Status.E_AGAIN, camera.CameraTimeoutError),
    (Status.E_CONFLICT, camera.CameraThreadError),
    (Status.E_NOT_FOUND, camera.CameraNotFoundError),
    (Status.E_IN_USE, camera.CameraBusyError),
    (Status.E_PERMISSION, camera.CameraPermissionError),
    (Status.E_INVALID_HANDLE, camera.CameraClosedError),
    (Status.E_NOT_OWNER, camera.CameraClosedError),
    (Status.E_SETUP_IO, camera.CameraError),
    (Status.E_PERIPHERAL, camera.CameraError),
    (Status.E_NO_MEMORY, camera.CameraError),
]


@pytest.mark.parametrize(("status", "error_type"), TYPES)
def test_each_status_has_its_type(status: Status, error_type: type[camera.CameraError]) -> None:
    error = camera_error(status, "keel_camera_start")
    assert type(error) is error_type
    assert error.status == status
    assert error.code == status.value
    assert error.function == "keel_camera_start"
    assert error.report is None
    assert str(error) == f"keel_camera_start: KEEL_{status.name}: {error.description}"
    assert error.description


def test_the_python_exception_types_they_also_are() -> None:
    assert issubclass(camera.CameraArgumentError, ValueError)
    assert issubclass(camera.CameraTimeoutError, TimeoutError)
    assert issubclass(camera.CameraPermissionError, PermissionError)
    assert issubclass(camera.CameraLibraryError, keel.LibraryError)
    assert not issubclass(camera.CameraError, keel.KeelError)
    assert str(camera_error(Status.E_AGAIN, "f")) == "f: KEEL_E_AGAIN: nothing arrived within the timeout"


def test_the_report_message_names_the_remedy() -> None:
    report = camera.ErrorReport(
        status=Status.E_PERMISSION,
        resource=camera.CameraResource.DEVICE_NODE,
        os_error=13,
        path="/dev/media0",
        message="permission denied on /dev/media0: add the user to the video group",
    )
    error = camera_error(Status.E_PERMISSION, "keel_camera_open", report)
    assert error.report is report
    assert str(error) == (
        "keel_camera_open: KEEL_E_PERMISSION: permission denied on /dev/media0: add the user to the video group"
    )


def test_an_unknown_status_is_a_plain_camera_error() -> None:
    error = camera_error(-99, "keel_camera_start")
    assert type(error) is camera.CameraError
    assert error.status.name == "UNRECOGNIZED_-99"
    assert str(error) == "keel_camera_start: status -99: no further details"
    assert status_name(-1) == "KEEL_E_INVALID_ARGUMENT"


@pytest.mark.parametrize(("status", "error_type"), TYPES)
def test_errors_survive_pickling(status: Status, error_type: type[camera.CameraError]) -> None:
    report = camera.ErrorReport(status, camera.CameraResource.CAMERA, 5, "", "the camera failed")
    error = camera_error(status, "keel_camera_read_frame", report)
    copy = pickle.loads(pickle.dumps(error))
    assert type(copy) is error_type
    assert str(copy) == str(error)
    assert (copy.status, copy.code, copy.function, copy.report, copy.description) == (
        error.status,
        error.code,
        error.function,
        error.report,
        error.description,
    )


def test_a_library_error_is_raised_with_its_message() -> None:
    error = camera.CameraLibraryError("cannot load x")
    assert str(error) == "cannot load x"
    assert isinstance(error, keel.LibraryError)
