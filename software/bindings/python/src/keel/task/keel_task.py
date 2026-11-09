# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""A single-use task class and the entry point that owns its robot resources."""

from __future__ import annotations

import threading
from abc import ABC, abstractmethod

from keel import device as device_api

from .task_options import TaskOptions
from .task_robot import TaskError, TaskRobot

_binding_lock = threading.Lock()


class KeelTask(ABC):
    """Subclass this and implement run() as ordinary sequential Python.

    self.keel exists only while keel.run(task) executes. Each task instance is
    single-use; construct a new instance for another run. Application methods
    such as approach_box() remain student code. The framework provides only
    bounded hardware primitives, not vision or navigation algorithms.
    """

    _robot: TaskRobot | None = None
    _used: bool = False

    @property
    def keel(self) -> TaskRobot:
        """Return this task's robot while running; raise TaskError outside that scope."""
        if self._robot is None:
            raise TaskError("CLOSED", "self.keel is available only while this task is running")
        return self._robot

    @abstractmethod
    def run(self) -> None:
        """Perform the student's sequence; return to stop and close the robot.

        Ordinary sleep() and image processing run on this thread while the
        background controller ticks. Between bounded actions, motor drive is
        zero. Any exception ends the task and triggers cleanup; hardware faults
        remain terminal even if student code catches their TaskError.
        """


def run(task: KeelTask, options: TaskOptions | None = None) -> None:
    """Execute a single-use task on this thread and always clean up its hardware.

    Args:
        task: A new KeelTask instance whose run() method contains student code.
        options: Optional finite time limits and native board/camera settings.

    The board opens immediately; the camera opens only on take_image(). Motor
    actions arm automatically. Normal return, cancellation, KeyboardInterrupt
    and exceptions all stop the controller and close opened resources.

    Raises:
        TypeError: task or options has the wrong type.
        RuntimeError: The task instance was already used.
        TaskError: A task deadline, hardware fault, camera failure or stop occurs.
        BaseException: The student's original error or a native cleanup failure.

    No motion is performed merely by constructing the task. Running a task that
    calls a movement primitive is the explicit decision to permit that motion.
    """
    if not isinstance(task, KeelTask):
        raise TypeError("task must be a KeelTask instance")
    if options is None:
        options = TaskOptions()
    if not isinstance(options, TaskOptions):
        raise TypeError("options must be TaskOptions")
    with _binding_lock:
        if task._used:
            raise RuntimeError("A KeelTask instance is single-use; construct a new task")
        task._used = True
    device = device_api.open(**options.board_kwargs)
    try:
        robot = TaskRobot(device, options)
    except BaseException as construction_error:
        try:
            device.close()
        except BaseException as close_error:  # noqa: BLE001 - preserve the original construction failure.
            construction_error.add_note(f"Board cleanup also failed: {close_error!r}")
        raise
    task._robot = robot
    failure: BaseException | None = None
    try:
        robot._start()
        robot._check()
        task.run()
        robot._check()
    except BaseException as error:
        failure = error
        raise
    finally:
        task._robot = None
        try:
            robot._close()
        except BaseException as cleanup_error:
            if failure is None:
                raise
            failure.add_note(f"Task cleanup also failed: {cleanup_error!r}")
