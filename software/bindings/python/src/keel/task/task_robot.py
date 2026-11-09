# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The task-scoped robot and its control-thread-enforced motion deadlines."""

from __future__ import annotations

import math
import numbers
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING

from keel import camera as camera_api
from keel.camera import Camera, Frame
from keel.loop import ControlLoop, Cycle
from keel.state import Fault, State

from .task_options import TaskOptions, _milliseconds

if TYPE_CHECKING:
    from keel.device import Device


class TaskError(RuntimeError):
    """Report a terminal task failure with a stable reason string.

    reason is CANCELLED, TASK_TIMEOUT, ARM_TIMEOUT, FAULT, LOOP_FAILED,
    CAMERA_FAILED, or CLOSED. The first failure is retained: catching it does
    not restore hardware authority or automatically clear a board fault.
    """

    def __init__(self, reason: str, message: str) -> None:
        """Store reason, a stable failure code, and message, its readable explanation.

        Args:
            reason: One of the uppercase task failure codes documented above.
            message: The detail shown when the exception is printed.
        """
        super().__init__(message)
        self.reason = reason

    reason: str
    """Stable uppercase failure category; catching it does not resume the task."""


@dataclass
class _Motion:
    """One finite motor intent, completed only after a zero-output snapshot."""

    left: int
    right: int
    duration_ns: int
    deadline_ns: int | None = None
    stop_cycle: int | None = None
    done: bool = False


class TaskRobot:
    """Provide blocking, bounded robot actions while a KeelTask is running.

    All methods except stop() belong to the thread executing the task. Percent
    duties are signed -100..100; durations are milliseconds. A drive or turn
    lazily arms the robot, then returns only after its command and applied
    output have reached zero. This confirms zero electrical drive, not physical
    standstill or travelled distance. No action promises an angular rotation.

    Constructed by keel.run(), not by student code. Retaining this object after
    the task ends does not retain access to hardware.
    """

    def __init__(self, device: Device, options: TaskOptions) -> None:
        """Prepare a runtime around an already-open board; internal entry point."""
        self._device = device
        self._options = options
        self._owner = threading.get_ident()
        self._condition = threading.Condition()
        self._ready = threading.Event()
        self._deadline_ns = time.monotonic_ns() + options.task_timeout_ms * 1_000_000
        self._error: TaskError | None = None
        self._closed = False
        self._started = False
        self._armed_once = False
        self._motion: _Motion | None = None
        self._camera: Camera | None = None
        self._loop = ControlLoop(device, self._step, on_fault=self._on_fault, name="keel-task-loop")

    def _start(self) -> None:
        """Wait for the control thread's first callback, leaving permission disarmed."""
        self._loop.start()
        deadline = min(self._deadline_ns, time.monotonic_ns() + self._options.arm_timeout_ms * 1_000_000)
        while not self._ready.wait(0.005):
            if self._loop.error is not None or time.monotonic_ns() >= deadline:
                raise self._abort(TaskError("LOOP_FAILED", "The robot control loop did not start"))
        self._started = True

    def _check(self) -> None:
        """Reject use outside the task thread, after abort, or after a deadline."""
        if threading.get_ident() != self._owner:
            raise RuntimeError("TaskRobot actions must run on the task thread; only stop() is cross-thread")
        with self._condition:
            error = self._error
            closed = self._closed
        if error is not None:
            raise error
        if closed:
            raise TaskError("CLOSED", "The task has finished; its robot is closed")
        if time.monotonic_ns() >= self._deadline_ns:
            raise self._abort(TaskError("TASK_TIMEOUT", "The task exceeded task_timeout_ms"))
        if self._loop.error is not None or (self._started and not self._loop.running):
            raise self._abort(TaskError("LOOP_FAILED", "The robot control loop failed or stopped"))
        if self._loop.snapshot.state == State.FAULTED:
            raise self._abort(TaskError("FAULT", "The robot reported a fault; the task cannot resume"))

    def _abort(self, error: TaskError) -> TaskError:
        """Revoke the intent before synchronously disarming; first failure wins."""
        with self._condition:
            first_failure = self._error is None
            if first_failure:
                self._error = error
            retained = self._error or error
            self._motion = None
            closed = self._closed
        try:
            if not closed and first_failure:
                self._device.disarm()
        finally:
            with self._condition:
                self._condition.notify_all()
        return retained

    def _on_fault(self, fault: Fault) -> None:
        """Latch a hardware fault on the control thread without clearing it."""
        self._abort(TaskError("FAULT", f"The robot reported a fault: {fault}"))

    def _step(self, cycle: Cycle) -> None:
        """Enforce wall-clock leases independently of sleeping task code."""
        self._ready.set()
        now = time.monotonic_ns()
        if now >= self._deadline_ns:
            self._abort(TaskError("TASK_TIMEOUT", "The task exceeded task_timeout_ms"))
        if self._loop.error is not None:
            self._abort(TaskError("LOOP_FAILED", "The robot control loop reported an error"))
        if cycle.snapshot.state == State.FAULTED:
            self._abort(TaskError("FAULT", "The robot reported a fault; the task cannot resume"))
        if self._armed_once and not self._closed and cycle.snapshot.state != State.ARMED:
            self._abort(TaskError("FAULT", "Motor permission was lost; the task cannot resume"))
        left = right = 0
        with self._condition:
            motion = self._motion
            if self._error is None and not self._closed and motion is not None:
                if motion.deadline_ns is None:
                    motion.deadline_ns = now + motion.duration_ns
                if motion.stop_cycle is not None:
                    snapshot = cycle.snapshot
                    if snapshot.last_cycle_id >= motion.stop_cycle and not (
                        snapshot.command_left or snapshot.command_right or snapshot.output_left or snapshot.output_right
                    ):
                        motion.done = True
                        self._motion = None
                        self._condition.notify_all()
                elif now >= motion.deadline_ns:
                    motion.stop_cycle = cycle.cycle_id
                else:
                    left, right = motion.left, motion.right
        if left or right:
            if cycle.snapshot.state != State.ARMED:
                self._abort(TaskError("FAULT", "Motor permission was lost during the action"))
                cycle.coast()
            else:
                cycle.drive(left, right)
        else:
            cycle.coast()

    @staticmethod
    def _duty(name: str, value: object) -> int:
        """Convert finite signed percent to permille, matching the Java facade."""
        if isinstance(value, bool) or not isinstance(value, numbers.Real):
            raise TypeError(f"{name} must be a finite percentage, not {type(value).__name__}")
        number = float(value)
        if not math.isfinite(number) or not -100 <= number <= 100:
            raise ValueError(f"{name} must be a finite percentage in -100..100")
        return math.floor(number * 10 + 0.5)

    def _arm(self) -> None:
        """Wait for actual ARMED; never clear a fault or restore lost permission."""
        self._check()
        if self._armed_once:
            if self._loop.snapshot.state != State.ARMED:
                raise self._abort(TaskError("FAULT", "Motor permission was lost; create a new task after recovery"))
            return
        deadline = min(self._deadline_ns, time.monotonic_ns() + self._options.arm_timeout_ms * 1_000_000)

        def request_if_active(device: Device) -> None:
            """Serialize native arming against cancellation before touching permission."""
            with self._condition:
                if self._error is not None:
                    raise self._error
                if self._closed or time.monotonic_ns() >= self._deadline_ns:
                    raise TaskError("TASK_TIMEOUT", "The task ended before arming")
                device.request_arm()

        request = self._loop.submit(request_if_active)
        while True:
            self._check()
            if request.done():
                try:
                    request.result()
                except Exception as error:
                    raise self._abort(TaskError("FAULT", "The robot refused automatic arming")) from error
                if self._loop.snapshot.state == State.ARMED:
                    self._armed_once = True
                    return
            if time.monotonic_ns() >= deadline:
                raise self._abort(TaskError("ARM_TIMEOUT", "The robot did not arm before arm_timeout_ms"))
            with self._condition:
                self._condition.wait(0.005)

    def drive_for(self, left_percent: float, right_percent: float, duration_ms: int) -> None:
        """Apply signed motor duty for a finite duration, then confirm zero drive.

        Args:
            left_percent: Left motor duty in -100..100 percent, not wheel speed.
            right_percent: Right motor duty in -100..100 percent.
            duration_ms: Integer milliseconds, 0..options.max_motion_ms. Zero
                validates the call but neither arms nor moves the robot.

        The duration starts on the first control cycle applying the action,
        after automatic arming. It blocks this task thread; the control thread
        enforces the deadline even if the task thread is delayed. Zero duties
        wait for the requested duration without arming. The method returns only
        after a later snapshot confirms zero command and zero applied output.

        Raises:
            TypeError: A duty or duration has the wrong type.
            ValueError: A duty or duration is outside its finite limit.
            TaskError: Arming, cancellation, the task deadline or hardware fails.
            RuntimeError: Called outside the task thread.
        """
        self._check()
        left, right = self._duty("left_percent", left_percent), self._duty("right_percent", right_percent)
        duration = _milliseconds("duration_ms", duration_ms, allow_zero=True)
        if duration > self._options.max_motion_ms:
            raise ValueError("duration_ms exceeds max_motion_ms")
        if duration == 0:
            return
        try:
            if left or right:
                self._arm()
            self._check()
            motion = _Motion(left, right, duration * 1_000_000)
            with self._condition:
                if self._error is not None:
                    raise self._error
                self._motion = motion
            while not motion.done:
                self._check()
                with self._condition:
                    self._condition.wait(0.005)
            self._check()
        except BaseException:
            # User interruption must revoke the active lease before unwinding.
            self._abort(TaskError("CANCELLED", "The motor action was interrupted"))
            raise

    def turn_left_for(self, duration_ms: int) -> None:
        """Turn with left -20% and right +20% for duration_ms, then stop.

        Blocking, automatically armed, and bounded exactly like drive_for().
        Duration is integer milliseconds, not degrees; the physical angle
        depends on the robot, surface and load. The same exceptions apply.
        """
        self.drive_for(-20, 20, duration_ms)

    def turn_right_for(self, duration_ms: int) -> None:
        """Turn with left +20% and right -20% for duration_ms, then stop.

        Blocking, automatically armed, and bounded exactly like drive_for().
        Duration is integer milliseconds, not degrees; the same exceptions
        apply. No camera, compass or box tracking is implied.
        """
        self.drive_for(20, -20, duration_ms)

    def take_image(self) -> Frame:
        """Read one BGR888 video frame while no motor action is active.

        The camera is opened lazily on this task thread and reused until task
        cleanup. A frame owns its bytes and remains valid after subsequent
        reads and camera closure. Use frame.array() for optional NumPy/OpenCV
        access. The read waits at most camera_timeout_ms or the remaining task
        lifetime, whichever is shorter; native camera opening can also block.

        Returns:
            A BGR888 Frame containing the newest queued camera image.

        Raises:
            TaskError: The camera fails, the task ends, or its deadline expires.
            RuntimeError: Called outside the task thread.
        """
        self._check()
        try:
            if self._camera is None:
                self._camera = camera_api.open(**self._options.camera_kwargs)
                self._check()
                self._camera.start()
            remaining = (self._deadline_ns - time.monotonic_ns()) / 1_000_000_000
            self._check()
            frame = self._camera.read_frame(timeout=min(self._options.camera_timeout_ms / 1000, remaining))
            self._check()
            return frame
        except TaskError:
            raise
        except BaseException as error:
            # Camera/native failures and task interruptions must not allow later motion.
            aborted = self._abort(TaskError("CAMERA_FAILED", "The task camera could not provide an image"))
            if isinstance(error, Exception):
                raise aborted from error
            raise

    def stop(self) -> None:
        """Cancel this task and synchronously disarm, from any thread.

        This is terminal: pending actions fail with TaskError(CANCELLED), and
        catching that error does not permit later movement. Callers need no
        manual ARMED checks. Normal bounded actions already stop before return.
        A native stop failure is propagated, rather than reporting false success.
        """
        self._abort(TaskError("CANCELLED", "The task was stopped"))

    def _close(self) -> None:
        """Disarm, stop the loop, close lazy camera and board, attempting all steps."""
        with self._condition:
            self._motion = None
            self._closed = True
            self._condition.notify_all()
        failures: list[BaseException] = []
        operations: list[Callable[[], None]] = [self._loop.stop, self._loop.join]
        if self._camera is not None:
            operations.append(self._camera.close)
        operations.append(self._device.close)
        for operation in operations:
            try:
                operation()
            except BaseException as error:  # noqa: BLE001 - all resources must be closed after any cleanup failure.
                failures.append(error)
        if failures:
            for additional in failures[1:]:
                failures[0].add_note(f"Additional cleanup failure: {additional!r}")
            raise failures[0]
