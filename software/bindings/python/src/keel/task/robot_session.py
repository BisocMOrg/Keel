# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""An interactive robot session with explicit arming and renewable finite motion leases.

The session owns its hardware loop. It is intended for dashboards and diagnostic
examples; sequential student programs normally use KeelTask instead. No method
exposes the native device or control loop, and drive refresh never arms a robot.
"""

from __future__ import annotations

import operator
import threading
import time
from collections.abc import Callable
from concurrent.futures import Future
from concurrent.futures import TimeoutError as FutureTimeout
from types import TracebackType
from typing import TYPE_CHECKING, TypeVar

from keel import device as device_api
from keel.errors import AgainError
from keel.loop import ControlLoop, Cycle, LoopStats
from keel.peripherals import Button, Led, RangeMeasurement, SensorPower
from keel.sim import SimItem
from keel.state import Fault, Snapshot, State

from .task_options import TaskOptions, _milliseconds
from .task_robot import TaskError, TaskRobot

if TYPE_CHECKING:
    from keel import Capability
    from keel.device import Device

T = TypeVar("T")


class RobotSession:
    """Own a long-lived interactive session without application control-loop code.

    All methods are thread-safe. Arming and fault recovery are always explicit.
    refresh_drive() replaces one finite motor lease without inserting a stop
    between valid refreshes. Expiry, disarm(), emergency_stop(), and close()
    revoke that lease and invalidate older queued operations before stopping.

    The session lasts until close(); TaskOptions.task_timeout_ms applies only to
    KeelTask, not to this explicitly long-lived service. max_motion_ms bounds
    every drive refresh and arm_timeout_ms bounds queued operations and arming.
    """

    def __init__(self, options: TaskOptions | None = None) -> None:
        """Open the configured board and start its controller, initially disarmed.

        Args:
            options: Native board settings and finite action limits. Camera
                settings are unused; interactive cameras have their own owner.

        Raises:
            TaskError: The controller cannot start.
            KeelError: Native board loading or opening fails.
        """
        if options is not None and not isinstance(options, TaskOptions):
            raise TypeError("options must be TaskOptions")
        self._options = options or TaskOptions()
        self._lock = threading.RLock()
        self._ready = threading.Event()
        self._closed = False
        self._generation = 0
        self._authorized = False
        self._left = self._right = 0
        self._until_ns = 0
        self._seen_fault_count = 0
        self._pressed = Button(0)
        self._released = Button(0)
        self._device = device_api.open(**self._options.board_kwargs)
        try:
            self._loop = ControlLoop(self._device, self._step, name="keel-session-loop")
            self._loop.start()
            if not self._ready.wait(self._options.arm_timeout_ms / 1000):
                raise TaskError("LOOP_FAILED", "Interactive robot controller did not start")
        except BaseException as error:
            operations: list[Callable[[], None]] = []
            if hasattr(self, "_loop"):
                operations.extend((self._loop.stop, self._loop.join))
            operations.append(self._device.close)
            for operation in operations:
                try:
                    operation()
                except BaseException as cleanup_error:  # noqa: BLE001 - attempt every startup cleanup.
                    error.add_note(f"Session startup cleanup also failed: {cleanup_error!r}")
            raise

    def __enter__(self) -> RobotSession:
        """Return this open session for a with statement."""
        self._check()
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        """Stop and close on every exit, preserving the original application error."""
        failures: list[BaseException] = []
        if isinstance(exc, (KeyboardInterrupt, SystemExit)):
            try:
                self.emergency_stop()
            except BaseException as error:  # noqa: BLE001 - attempt every cleanup after any failure.
                failures.append(error)
        try:
            self.close()
        except BaseException as error:  # noqa: BLE001 - preserve application failure while closing.
            failures.append(error)
        if failures:
            if exc is not None:
                for failure in failures:
                    exc.add_note(f"Session cleanup also failed: {failure!r}")
            else:
                for failure in failures[1:]:
                    failures[0].add_note(f"Additional session cleanup failure: {failure!r}")
                raise failures[0]

    def _check(self) -> None:
        """Reject operations after closure or an unexpected controller exit."""
        if self._closed:
            raise TaskError("CLOSED", "The interactive robot session is closed")
        if self._ready.is_set() and not self._loop.running:
            raise TaskError("LOOP_FAILED", "The interactive robot controller ended")
        if self._loop.error is not None:
            raise TaskError("LOOP_FAILED", "The interactive robot controller reported an error")

    def _step(self, cycle: Cycle) -> None:
        """Enforce the finite lease on the hardware thread, independently of HTTP."""
        self._ready.set()
        with self._lock:
            snapshot = cycle.snapshot
            if self._loop.error is not None:
                self._invalidate()
                self._device.disarm()
                cycle.coast()
                return
            self._pressed |= snapshot.buttons_pressed
            self._released |= snapshot.buttons_released
            if snapshot.fault_count > self._seen_fault_count:
                self._invalidate()
                self._seen_fault_count = snapshot.fault_count
            if self._closed:
                cycle.coast()
                return
            if self._until_ns and time.monotonic_ns() >= self._until_ns:
                self._invalidate()
                self._device.disarm()
            if self._authorized and self._until_ns and snapshot.state == State.ARMED:
                cycle.drive(self._left, self._right)
            else:
                cycle.coast()

    def _invalidate(self) -> None:
        """Revoke all older queued work and motor authority while holding the lock."""
        self._generation += 1
        self._authorized = False
        self._left = self._right = self._until_ns = 0

    def _submit(self, operation: Callable[[Device], T], guard: Callable[[], None] | None = None) -> Future[T]:
        """Queue a bounded native operation, fenced by session and application intent."""
        with self._lock:
            self._check()
            generation = self._generation
            deadline_ns = time.monotonic_ns() + self._options.arm_timeout_ms * 1_000_000

        def execute(device: Device) -> T:
            """Check application ownership before taking the session lock to avoid lock inversion."""
            if guard is not None:
                guard()
            with self._lock:
                self._check()
                if generation != self._generation:
                    raise TaskError("CANCELLED", "A newer stop cancelled this queued operation")
                if time.monotonic_ns() >= deadline_ns:
                    raise TaskError("CANCELLED", "The queued operation expired before execution")
                return operation(device)

        return self._loop.submit(execute)

    def _wait(self, future: Future[T]) -> T:
        """Wait a finite time and cancel queued work if the caller cannot continue."""
        try:
            return future.result(timeout=self._options.arm_timeout_ms / 1000)
        except (FutureTimeout, KeyboardInterrupt, SystemExit):
            future.cancel()
            self.disarm()
            raise

    def request_arm(self, guard: Callable[[], None] | None = None) -> Future[None]:
        """Queue an explicit arming request; its future completes before the handshake finishes.

        Args:
            guard: Optional fast authorization check run immediately before
                native execution. Raise to reject expired application intent.

        Returns:
            A future for request acceptance, not for reaching ARMED. Use arm()
            when a synchronous action must wait for the complete handshake.
        """

        def request(device: Device) -> None:
            """Grant lease eligibility only after the native request was accepted."""
            device.request_arm()
            self._authorized = True

        return self._submit(request, guard)

    def arm(self, guard: Callable[[], None] | None = None) -> None:
        """Request arming and wait for confirmed ARMED within arm_timeout_ms.

        No fault is cleared automatically. A concurrent stop cancels the wait,
        invalidates the queued request, and cannot be undone by a late result.
        """
        with self._lock:
            self._check()
            generation = self._generation
        deadline = time.monotonic() + self._options.arm_timeout_ms / 1000
        self._wait(self.request_arm(guard))
        while time.monotonic() < deadline:
            with self._lock:
                self._check()
                if generation != self._generation:
                    raise TaskError("CANCELLED", "Arming was cancelled by a stop or fault")
                if self._loop.snapshot.state == State.ARMED:
                    return
                if self._loop.snapshot.state == State.FAULTED:
                    raise TaskError("FAULT", "The robot faulted during arming")
            time.sleep(0.005)
        self.disarm()
        raise TaskError("ARM_TIMEOUT", "The robot did not arm before arm_timeout_ms")

    def refresh_drive(self, left_percent: float, right_percent: float, lease_ms: int) -> None:
        """Replace the motor lease using signed percentages and a finite duration.

        Args:
            left_percent: Left duty in -100..100 percent.
            right_percent: Right duty in -100..100 percent.
            lease_ms: Positive integer milliseconds, at most max_motion_ms.
                Applications must pass remaining intent lifetime when dispatch
                was delayed, rather than extending an old command on execution.

        Arming must already be confirmed. A lease expiry disarms, so later drive
        packets cannot restart the robot until a fresh explicit arm request.
        """
        duration = _milliseconds("lease_ms", lease_ms)
        if duration > self._options.max_motion_ms:
            raise ValueError("lease_ms exceeds max_motion_ms")
        self.refresh_drive_until(left_percent, right_percent, time.monotonic_ns() + duration * 1_000_000)

    def refresh_drive_until(self, left_percent: float, right_percent: float, deadline_ns: int) -> None:
        """Refresh drive up to an original monotonic_ns deadline, without extending queued intent.

        deadline_ns must be in the future and within max_motion_ms. This form
        is for network controllers whose lease starts at request receipt.
        """
        left = TaskRobot._duty("left_percent", left_percent)
        right = TaskRobot._duty("right_percent", right_percent)
        if isinstance(deadline_ns, bool):
            raise TypeError("deadline_ns must be integer monotonic nanoseconds")
        deadline_ns = operator.index(deadline_ns)
        now = time.monotonic_ns()
        if deadline_ns <= now:
            raise TaskError("CANCELLED", "The drive deadline expired before execution")
        if deadline_ns > now + self._options.max_motion_ms * 1_000_000:
            raise ValueError("deadline_ns must be within max_motion_ms")
        with self._lock:
            self._check()
            now = time.monotonic_ns()
            if self._until_ns and now >= self._until_ns:
                self._invalidate()
                self._device.disarm()
            if deadline_ns <= now:
                raise TaskError("CANCELLED", "The drive deadline expired before execution")
            if not self._authorized or self._loop.snapshot.state != State.ARMED:
                raise TaskError("FAULT", "Explicitly arm the robot before refreshing drive")
            if not (left or right):
                self._left = self._right = self._until_ns = 0
                self._device.coast()
                return
            self._left, self._right = left, right
            self._until_ns = deadline_ns

    def disarm(self) -> None:
        """Invalidate old work and synchronously remove motor permission from any thread."""
        with self._lock:
            if self._closed:
                return
            self._invalidate()
            self._device.disarm()

    def coast(self) -> None:
        """Clear the current drive lease while retaining explicit arming permission."""
        with self._lock:
            self._check()
            if self._until_ns and time.monotonic_ns() >= self._until_ns:
                self._invalidate()
                self._device.disarm()
                return
            self._left = self._right = self._until_ns = 0
            self._device.coast()

    def emergency_stop(self) -> None:
        """Invalidate old work and latch a native emergency stop from any thread."""
        with self._lock:
            if self._closed:
                return
            self._invalidate()
            self._device.emergency_stop()
            self._seen_fault_count = self._device.snapshot().fault_count

    def clear_fault(self, guard: Callable[[], None] | None = None) -> None:
        """Explicitly clear a recoverable native fault, leaving the robot disarmed."""
        self.disarm()
        self._wait(self._submit(lambda device: device.clear_fault(), guard))

    def snapshot(self) -> Snapshot:
        """Return the latest immutable board observation without exposing the control loop."""
        self._check()
        return self._loop.snapshot

    def stats(self) -> LoopStats:
        """Return timing counters from the session's owned controller."""
        self._check()
        return self._loop.stats()

    def capabilities(self) -> Capability:
        """Return the board's capability flags; this does not arm or move it."""
        self._check()
        return self._device.capabilities()

    def fault(self) -> Fault:
        """Return the native fault record for diagnostic display."""
        self._check()
        return self._device.fault()

    def buttons_pressed(self) -> Button:
        """Consume accumulated button press edges, including edges between slow reads."""
        with self._lock:
            self._check()
            result, self._pressed = self._pressed, Button(0)
            return result

    def buttons_released(self) -> Button:
        """Consume accumulated button release edges without losing intervening events."""
        with self._lock:
            self._check()
            result, self._released = self._released, Button(0)
            return result

    def set_rgb(self, led: Led | int, color: int, guard: Callable[[], None] | None = None) -> None:
        """Set one RGB LED to packed 0xRRGGBB, with native capability validation."""
        self._wait(self._submit(lambda device: device.set_rgb(led, color), guard))

    def set_rgb_all(self, color: int, guard: Callable[[], None] | None = None) -> None:
        """Set every RGB LED to packed 0xRRGGBB without changing motor permission."""
        self._wait(self._submit(lambda device: device.set_rgb_all(color), guard))

    def set_rgb_frame(self, colors: list[int], guard: Callable[[], None] | None = None) -> None:
        """Set all six RGB LEDs from six packed 0xRRGGBB values."""
        self._wait(self._submit(lambda device: device.set_rgb_frame(colors), guard))

    def rgb(self, led: Led | int) -> int:
        """Read the last requested packed colour for one RGB LED."""
        self._check()
        return self._device.rgb(led)

    def set_button_leds(
        self, mask: Button | int, values: Button | int, guard: Callable[[], None] | None = None
    ) -> None:
        """Update the selected button LEDs; bits use the Button flags."""
        self._wait(self._submit(lambda device: device.set_button_leds(mask, values), guard))

    def measure_distance(self, timeout_ms: int = 1000, guard: Callable[[], None] | None = None) -> RangeMeasurement:
        """Wait for sensor power and measurement spacing, then return a ranging result.

        Only a VALID result carries distance_mm. NO_ECHO does not mean a clear
        path. The controller keeps ticking while this calling thread waits.
        """
        timeout = _milliseconds("timeout_ms", timeout_ms)
        deadline = time.monotonic() + timeout / 1000
        with self._lock:
            generation = self._generation
        while True:
            if guard is not None:
                guard()
            self._check()
            with self._lock:
                if generation != self._generation:
                    raise TaskError("CANCELLED", "Range request was cancelled by a stop")
            if self.snapshot().sensor_power in (SensorPower.PERMITTED, SensorPower.CONFIRMED):
                try:
                    result = self._device.range_measure()
                    if guard is not None:
                        guard()
                    with self._lock:
                        if generation != self._generation:
                            raise TaskError("CANCELLED", "Range result was cancelled by a stop")
                    return result
                except AgainError:
                    pass
            if time.monotonic() >= deadline:
                raise TimeoutError("The range sensor was not ready before timeout_ms")
            time.sleep(0.01)

    def sim_set(self, item: SimItem, value: int) -> None:
        """Set an explicit simulator stimulus; native code refuses this on a real board."""
        self._check()
        self._device.sim_set(item, value)

    def close(self) -> None:
        """Disarm, stop the controller and close the board, once, from any thread."""
        with self._lock:
            if self._closed:
                return
            self._invalidate()
            self._closed = True
        failures: list[BaseException] = []
        for operation in (self._loop.stop, self._loop.join, self._device.close):
            try:
                operation()
            except BaseException as error:  # noqa: BLE001 - attempt every cleanup after any failure.
                failures.append(error)
        if failures:
            for failure in failures[1:]:
                failures[0].add_note(f"Additional session cleanup failure: {failure!r}")
            raise failures[0]
