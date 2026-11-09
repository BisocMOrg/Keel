# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""ControlLoop: one keel_cycle per period on one thread, on the device clock.

See the keel.loop package for the model: the cycle, honest health, ownership and faulted mode.
"""

from __future__ import annotations

import ctypes
import gc
import logging
import operator
import sys
import threading
import time
from collections.abc import Callable
from concurrent.futures import Future
from queue import Empty, SimpleQueue
from types import MappingProxyType, TracebackType
from typing import Any, Self, SupportsIndex, TypeVar

from keel import limits
from keel._internal import _generated as g
from keel._internal import native_library
from keel.device import Device
from keel.errors import HandleError, InvalidArgumentError, KeelError, Status, StatusClass
from keel.errors.error_factory import error_for, refused
from keel.state import Fault, Snapshot, State

from ._generated import CycleCommand, CycleFlag, Health
from .cycle import Cycle
from .loop_stats import LoopStats

__all__ = ["ControlLoop"]

T = TypeVar("T")

_log = logging.getLogger("keel")

_US_PER_MS = 1000
_SWITCH_INTERVAL_S = 0.001
_LOG_INTERVAL_S = 5.0

# sys.setswitchinterval is process-wide: the first running loop saves the previous value, the last restores it.
_switch_lock = threading.Lock()
_switch_users = 0
_switch_saved = 0.0

# Guards the one-loop-per-device rule while a loop is being attached.
_attach_lock = threading.Lock()


def _acquire_switch_interval() -> None:
    global _switch_users, _switch_saved
    with _switch_lock:
        if _switch_users == 0:
            _switch_saved = sys.getswitchinterval()
            sys.setswitchinterval(_SWITCH_INTERVAL_S)
        _switch_users += 1


def _release_switch_interval() -> None:
    global _switch_users
    with _switch_lock:
        _switch_users -= 1
        if _switch_users == 0:
            sys.setswitchinterval(_switch_saved)


class _Ended(BaseException):
    """Unwinds the cycle loop when the device was closed: a normal end, not an error."""


class _RateLimit:
    """Allows one log message per key and interval, so a persistent condition cannot flood the log."""

    def __init__(self) -> None:
        self._next: dict[object, float] = {}

    def allow(self, key: object) -> bool:
        now = time.monotonic()
        if now < self._next.get(key, 0.0):
            return False
        self._next[key] = now + _LOG_INTERVAL_S
        return True


class ControlLoop:
    """The control loop: one keel_cycle per period on one dedicated thread, on the device clock.

    Start it on its own daemon thread with start() (or a with block), or run it on the calling thread with run().
    step(cycle) is called once per cycle with a reused Cycle; it decides the motor command with cycle.drive(),
    cycle.brake() or cycle.coast(). Other threads post one-shot intents with set_command(), brake() and coast(), and
    run go calls on the loop thread with request_arm(), clear_fault() and submit(). Stops (stop(), or the device's
    disarm() and emergency_stop()) work from any thread, signal handlers included.

    Parameters:
        device: the open device; a device has at most one loop for its whole life.
        step: the application's control step, or None for a loop that only ticks and applies posted intents.
        period_ms: the cycle period, 1..limits.TICK_PERIOD_MS (10).
        step_budget_ms: the time the application's work of one cycle may take (queued requests, submitted
            functions and the step), 1..limits.STALL_BUDGET_MS (40). An overrun clears CONTROL_OK.
        take_buttons: consume the button edges each cycle, so the snapshots report every press and release once.
        on_fault: called on the loop thread with the fault record each time the device latches a fault, also one
            that latches again in the cycle that cleared the previous one.
        name: the name of the loop's thread.

    """

    def __init__(
        self,
        device: Device,
        step: Callable[[Cycle], None] | None = None,
        *,
        period_ms: int = limits.TICK_PERIOD_MS,
        step_budget_ms: int = limits.STALL_BUDGET_MS,
        take_buttons: bool = True,
        on_fault: Callable[[Fault], None] | None = None,
        name: str = "keel-loop",
    ) -> None:
        """Attach a loop to device; nothing runs until start() or run()."""
        if not isinstance(device, Device):
            raise TypeError(f"ControlLoop needs a keel.Device, not {type(device).__name__}")
        if step is not None and not callable(step):
            raise TypeError("step must be callable or None")
        if on_fault is not None and not callable(on_fault):
            raise TypeError("on_fault must be callable or None")
        period = _parameter("period_ms", period_ms, limits.TICK_PERIOD_MS)
        budget = _parameter("step_budget_ms", step_budget_ms, limits.STALL_BUDGET_MS)
        with _attach_lock:
            if device._loop_attached:
                raise RuntimeError(
                    "this device already has a ControlLoop; one loop serves a device for its whole life "
                    "(close and reopen the device for a new one)"
                )
            snapshot = device.snapshot()
            device._loop_attached = True
        self.device = device
        self._step = step
        self._on_fault = on_fault
        self._name = name
        self._period_us = period * _US_PER_MS
        self._budget_us = budget * _US_PER_MS
        self._flags = CycleFlag.TAKE_BUTTONS if take_buttons else CycleFlag(0)
        self._cycle = Cycle(device, snapshot)
        self._input = g.CycleInputStruct()
        self._output = g.SnapshotStruct()
        self._go: SimpleQueue[tuple[Future[Any], Callable[[Device], Any]]] = SimpleQueue()
        self._intents: SimpleQueue[tuple[int, int, int]] = SimpleQueue()
        self._limit = _RateLimit()
        self._snapshot = snapshot
        self._fault: Fault | None = None
        self._error: BaseException | None = None
        self._fatal: BaseException | None = None
        self._thread: threading.Thread | None = None
        self._ident: int | None = None
        self._running = False
        self._stopping = False
        self._ended = False
        # Fault latches already reported: a fault active when the loop is created is still reported once.
        self._fault_count = max(0, snapshot.fault_count - (1 if snapshot.state == State.FAULTED else 0))
        self._gc_frozen = False
        self._cycles = 0
        self._overruns = 0
        self._max_gap_us = 0
        self._max_step_us = 0
        self._rejected: dict[Status, int] = {}
        self._step_errors = 0
        self._faults = 0

    # ---------------------------------------------------------- properties --

    @property
    def snapshot(self) -> Snapshot:
        """The latest snapshot the loop published; read-only, from any thread."""
        return self._snapshot

    @property
    def fault(self) -> Fault | None:
        """The fault record the loop fetched when the device last latched a fault, or None."""
        return self._fault

    @property
    def error(self) -> BaseException | None:
        """The most recent exception raised by the step, or the error that ended the loop."""
        return self._error

    @property
    def running(self) -> bool:
        """True while the loop is running cycles."""
        return self._running

    def stats(self) -> LoopStats:
        """Return a copy of the loop's counters."""
        return LoopStats(
            cycles=self._cycles,
            overruns=self._overruns,
            max_gap_us=self._max_gap_us,
            max_step_us=self._max_step_us,
            rejected=MappingProxyType(dict(self._rejected)),
            step_errors=self._step_errors,
            faults=self._faults,
        )

    # ------------------------------------------------------- running modes --

    def start(self) -> None:
        """Run the loop on a new daemon thread named after the loop.

        Refused with RuntimeError when the device already has a control owner (the thread start() would create
        cannot be that owner, so its first tick would fail-stop with TICK_THREAD_CHANGED), when the loop already
        runs, or when it has ended. Allowed from any thread, including an event-loop thread.
        """
        if self._ended or self._stopping:
            raise RuntimeError("the loop has ended; close and reopen the device for a new loop")
        if self._thread is not None or self._running:
            raise RuntimeError("the loop is already running")
        owner = self.device.snapshot().owner_thread_id
        if owner:
            raise RuntimeError(
                f"the device already has a control owner (thread {owner}); a loop thread started now would fail-stop "
                "with TICK_THREAD_CHANGED. Start the loop before the first tick, or close and reopen the device"
            )
        thread = threading.Thread(target=self._execute, args=(None, None), name=self._name, daemon=True)
        self._thread = thread
        thread.start()

    def run(self, *, cycles: int | None = None, until: Callable[[Snapshot], bool] | None = None) -> Snapshot:
        """Run the loop on the calling thread and return the latest snapshot.

        It runs until cycles cycles have run, until(snapshot) returns True after a cycle, or the loop is stopped.
        Repeated run() calls on one thread continue the same loop with the same cycle_id sequence; on a
        virtual-clock simulator this drives the device deterministically, one period of device time per cycle.
        Refused with RuntimeError when another thread is the control owner, when the loop runs on its own thread, or
        in an asyncio event-loop thread. Re-raises the error that ended the loop.
        """
        count = None if cycles is None else _count(cycles)
        asyncio = sys.modules.get("asyncio")
        if asyncio is not None:
            try:
                asyncio.get_running_loop()
            except RuntimeError:
                pass
            else:
                raise RuntimeError("ControlLoop.run() must not run on an event-loop thread; use start() instead")
        if self._running or (self._thread is not None and not self._ended):
            raise RuntimeError("the loop is already running on another thread")
        if self._ended or self._stopping or self.device.closed:
            self._ended = True
            self._fail_pending()
        else:
            owner = self.device.snapshot().owner_thread_id
            if owner and owner != threading.get_native_id():
                raise RuntimeError(
                    f"the device's control owner is thread {owner}, not this thread; its first tick here would "
                    "fail-stop with TICK_THREAD_CHANGED. Close and reopen the device"
                )
            self._execute(count, until)
        if self._fatal is not None:
            raise self._fatal
        return self._snapshot

    def stop(self, *, emergency: bool = False) -> None:
        """Stop the robot now and end the loop after the current cycle; never waits.

        It calls the device's disarm() (or emergency_stop() with emergency=True) directly on the calling thread,
        "stop wins", and records the request in a plain attribute: no lock, no queue, no join. It is therefore safe
        from any thread and from a signal handler. A stop that libkeel could not confirm raises (for example
        SafetyFaultError with E_IO); a device that is already closed was stopped by its close.
        """
        self._stopping = True
        try:
            if emergency:
                self.device.emergency_stop()
            else:
                self.device.disarm()
        except HandleError:
            _log.debug("ControlLoop.stop: already stopped by close")

    def join(self, timeout: float | None = None) -> None:
        """Wait for the loop's thread to end, then re-raise the error that ended the loop, if any.

        Raises RuntimeError on the loop's own thread, which cannot wait for itself. With a timeout, it returns when
        the timeout expires and the loop still runs.
        """
        if self._ident == threading.get_ident():
            raise RuntimeError("join() called on the loop's own thread, which cannot wait for itself; use stop()")
        thread = self._thread
        if thread is not None:
            thread.join(timeout)
            if thread.is_alive():
                return
        if self._fatal is not None:
            raise self._fatal

    def __enter__(self) -> Self:
        """Start the loop on its own thread."""
        self.start()
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        """Stop the loop (an emergency stop after a KeyboardInterrupt) and wait for it.

        The error that ended the loop is re-raised only when the block itself ended normally.
        """
        emergency = exc_type is not None and issubclass(exc_type, KeyboardInterrupt)
        try:
            self.stop(emergency=emergency)
        except KeelError as error:
            _log.error("ControlLoop: the stop could not be confirmed: %s", error)
        if self._ident == threading.get_ident():
            return
        try:
            self.join()
        except BaseException:
            if exc_type is None:
                raise
            _log.exception("ControlLoop: the loop had ended with an error")

    # ----------------------------------------------------------- mailboxes --

    def request_arm(self) -> Future[None]:
        """Run Device.request_arm() on the loop thread at the start of the next cycle; any thread may call this.

        The future receives the result or the exception; a request still queued when the loop ends fails with
        HandleError.
        """
        return self.submit(_request_arm)

    def clear_fault(self) -> Future[None]:
        """Run Device.clear_fault() on the loop thread, the control owner, at the start of the next cycle."""
        return self.submit(_clear_fault)

    def submit(self, fn: Callable[[Device], T]) -> Future[T]:
        """Run fn(device) on the loop thread at the start of the next cycle, inside the step budget.

        The future receives the result or the exception; exceptions never reach the loop. Keep fn short: its time
        counts against step_budget_ms, and an overrun fails the cycle's health check.
        """
        future: Future[T] = Future()
        if self._ended or (self._stopping and not self._running):
            future.set_exception(self._ended_error())
            return future
        self._go.put((future, fn))
        if self._has_ended():
            self._fail_pending()
        return future

    def set_command(self, left: SupportsIndex, right: SupportsIndex) -> None:
        """Post a one-shot drive intent that the next cycle applies, unless the step commands that cycle itself.

        The intent is never re-sent: post again within the command timeout (500 ms by default) to keep the robot
        moving, so the deadman still certifies the posting thread. A duty outside -limits.DUTY_MAX..limits.DUTY_MAX
        (or a non-integer) posts a coast instead and raises InvalidArgumentError (or TypeError) on the posting
        thread; the loop itself stays healthy.
        """
        try:
            duties = operator.index(left), operator.index(right)
        except TypeError:
            self._intents.put((CycleCommand.DRIVE, 0, 0))
            raise TypeError("set_command: duties must be integers; a coast was posted instead") from None
        if not all(-limits.DUTY_MAX <= duty <= limits.DUTY_MAX for duty in duties):
            self._intents.put((CycleCommand.DRIVE, 0, 0))
            raise refused(
                InvalidArgumentError,
                "keel_cycle",
                f"set_command{duties} is outside -{limits.DUTY_MAX}..{limits.DUTY_MAX}, so a coast was posted instead",
            )
        self._intents.put((CycleCommand.DRIVE, *duties))

    def brake(self) -> None:
        """Post a one-shot brake intent for the next cycle."""
        self._intents.put((CycleCommand.BRAKE, 0, 0))

    def coast(self) -> None:
        """Post a one-shot coast intent (DRIVE 0, 0) for the next cycle."""
        self._intents.put((CycleCommand.DRIVE, 0, 0))

    # ------------------------------------------------------------ the loop --

    def _execute(self, cycles: int | None, until: Callable[[Snapshot], bool] | None) -> None:
        """Run cycles on the calling thread and handle every way the loop can end."""
        self._running = True
        self._ident = threading.get_ident()
        _acquire_switch_interval()
        try:
            if not self._gc_frozen:
                gc.freeze()
                self._gc_frozen = True
            self._run_cycles(cycles, until)
        except _Ended:
            self._ended = True
        except BaseException as error:  # noqa: BLE001  (every escape is a stop, then stored and re-raised)
            self._end_with(error)
        finally:
            _release_switch_interval()
            if self._stopping:
                self._ended = True
            self._running = False
            self._ident = None
            if self._ended:
                self._fail_pending()

    def _end_with(self, error: BaseException) -> None:
        """Stop the robot for an error that ends the loop, then store the error for join() and run()."""
        emergency = isinstance(error, KeyboardInterrupt)
        try:
            if emergency:
                self.device.emergency_stop()
            else:
                self.device.disarm()
        except HandleError:
            _log.debug("ControlLoop: already stopped by close")
        except KeelError as stop_error:
            _log.error("ControlLoop: the stop could not be confirmed: %s", stop_error)
        if isinstance(error, Exception):
            _log.error("ControlLoop ended: %s", error)
        else:
            _log.info("ControlLoop ended by %s", type(error).__name__)
        self._error = self._fatal = error
        self._ended = True

    def _run_cycles(self, cycles: int | None, until: Callable[[Snapshot], bool] | None) -> None:
        device = self.device
        state = device._state
        natives = device._natives
        token = state.token
        in_ref = ctypes.byref(self._input)
        out_ref = ctypes.byref(self._output)
        clock = ctypes.c_uint64()
        clock_ref = ctypes.byref(clock)
        period = self._period_us
        cycle = self._cycle

        def now() -> int:
            if state.closing:
                raise _Ended
            status = g.keel_now_us(natives, token, clock_ref)
            if status != Status.OK:
                self._call_failed(status, "keel_now_us")
            return clock.value

        if state.closing:
            raise _Ended
        self._publish(device.snapshot())
        cycle_id = self._snapshot.last_cycle_id
        next_us = now()
        last_call_us: int | None = None
        count = 0
        while not self._stopping and (cycles is None or count < cycles):
            next_us += period
            work_start = now()
            step_start = now() if self._run_requests() else work_start
            cycle_id += 1
            cycle._reset(self._snapshot, cycle_id, step_start)
            health = Health.ALL
            if self._step is not None:
                try:
                    self._step(cycle)
                except Exception as error:  # noqa: BLE001  (a step error is reported as health, never raised)
                    health &= ~Health.NO_EXCEPTION
                    self._error = error
                    self._step_errors += 1
                    if self._limit.allow("step"):
                        _log.error("ControlLoop step raised %s: %s", type(error).__name__, error)
            if cycle._refused:
                health &= ~Health.NO_EXCEPTION
            work_end = now()
            self._max_step_us = max(self._max_step_us, work_end - work_start)
            if work_end - work_start > self._budget_us:
                health &= ~Health.CONTROL_OK
            if not cycle.io_ok:
                health &= ~Health.IO_OK
            command, left, right = self._command(cycle)
            if last_call_us is not None:
                self._max_gap_us = max(self._max_gap_us, work_end - last_call_us)
            last_call_us = work_end
            g.fill_cycle_input(
                self._input,
                health=health,
                cycle_id=cycle_id,
                command=command,
                left=left,
                right=right,
                flags=self._flags,
            )
            if state.closing:
                raise _Ended
            status = g.keel_cycle(natives, token, in_ref, out_ref)
            count += 1
            self._cycles += 1
            self._dispatch(status)
            # _dispatch returns only when keel_cycle wrote the snapshot, which carries the device time it was taken.
            current = self._output.time_us
            if current > next_us + period:
                next_us = current
                self._overruns += 1
                if self._limit.allow("overrun"):
                    _log.warning("ControlLoop: a cycle missed its %d us period; deadline resynchronised", period)
            if self._stop_requested():
                break
            if state.closing:
                raise _Ended
            status = g.keel_sleep_until(natives, token, next_us)
            if status != Status.OK:
                self._call_failed(status, "keel_sleep_until")
            if until is not None and until(self._snapshot):
                break

    def _run_requests(self) -> bool:
        """Run the queued go requests and submitted functions; return True if any ran."""
        ran = False
        while True:
            try:
                future, fn = self._go.get_nowait()
            except Empty:
                return ran
            ran = True
            if not future.set_running_or_notify_cancel():
                continue
            try:
                result = fn(self.device)
            except Exception as error:  # noqa: BLE001  (the future receives it; the loop goes on)
                future.set_exception(error)
            except BaseException as error:
                future.set_exception(error)
                raise
            else:
                future.set_result(result)

    def _command(self, cycle: Cycle) -> tuple[int, int, int]:
        """Return this cycle's motor command: the step's own, else the last posted intent, else NONE."""
        intent: tuple[int, int, int] | None = None
        while True:
            try:
                intent = self._intents.get_nowait()
            except Empty:
                break
        if self._snapshot.state == State.FAULTED:
            return CycleCommand.NONE, 0, 0
        if cycle._command is not None:
            return cycle._command, cycle._left, cycle._right
        if intent is not None:
            return intent
        return CycleCommand.NONE, 0, 0

    def _publish(self, snapshot: Snapshot) -> None:
        """Publish a snapshot, and report a fault latch that it shows for the first time.

        A latch is recognised by the rise of fault_count, not by the state, so a fault that latches again in the cycle
        whose clear_fault request ended the previous one is reported as well.
        """
        self._snapshot = snapshot
        if snapshot.fault_count > self._fault_count:
            self._enter_faulted(snapshot.fault_count)

    def _dispatch(self, status: int) -> None:
        """Act on the result of keel_cycle by its class."""
        if status == Status.OK:
            self._publish(g.convert_snapshot(self._output))
            return
        status_class = _status_class(status)
        if status_class == StatusClass.REJECTED:
            if status in (Status.E_INVALID_ARGUMENT, Status.E_UNSUPPORTED):
                # The loop built a malformed input: a binding defect that is fatal for the loop (keel.h).
                written = None if status == Status.E_INVALID_ARGUMENT else g.convert_snapshot(self._output)
                raise error_for(status, "keel_cycle", token=self.device._state.token, snapshot=written)
            key = Status(status)
            self._rejected[key] = self._rejected.get(key, 0) + 1
            if self._limit.allow(key):
                _log.debug("ControlLoop: keel_cycle refused: %s", key.name)
            self._publish(g.convert_snapshot(self._output))
        elif status_class == StatusClass.PERIPHERAL:
            if self._limit.allow("peripheral"):
                _log.warning("ControlLoop: keel_cycle reported a peripheral failure: %s", Status(status).name)
            self._publish(g.convert_snapshot(self._output))
        elif status_class == StatusClass.HANDLE:
            if self.device._state.closing:
                raise _Ended
            raise error_for(status, "keel_cycle", token=self.device._state.token)
        else:
            # FAULT, and any class this binding does not know: the snapshot is written and shows the latch.
            self._publish(g.convert_snapshot(self._output))

    def _call_failed(self, status: int, function: str) -> None:
        """Handle a failed keel_now_us or keel_sleep_until: the loop ends, normally only when the device is closing.

        A FAULT here is the device clock's sticky fault: the latch is reported, and the loop ends because it can no
        longer pace itself and clear_fault() cannot clear the cause.
        """
        if self.device._state.closing:
            raise _Ended
        if _status_class(status) not in (StatusClass.REJECTED, StatusClass.PERIPHERAL, StatusClass.HANDLE):
            self._publish(self.device.snapshot())
        raise error_for(status, function, token=self.device._state.token)

    def _enter_faulted(self, fault_count: int) -> None:
        """Report a new fault latch once: fetch the record, log it and call on_fault; the loop keeps cycling."""
        self._fault_count = fault_count
        self._faults += 1
        try:
            fault = self.device.fault()
        except KeelError:
            fault = None
        self._fault = fault
        if fault is None:
            _log.error("ControlLoop: the device is FAULTED")
            return
        _log.error(
            "ControlLoop: the device is FAULTED: %s, cause %s, detail %d",
            fault.status.name,
            fault.cause.name,
            fault.detail,
        )
        if self._on_fault is not None:
            try:
                self._on_fault(fault)
            except Exception as error:  # noqa: BLE001  (an on_fault error must not end the loop)
                self._error = error
                _log.exception("ControlLoop: on_fault raised")

    def _has_ended(self) -> bool:
        # A method, so the type checker does not narrow a flag that another thread sets.
        return self._ended

    def _stop_requested(self) -> bool:
        return self._stopping

    def _ended_error(self) -> KeelError:
        return refused(HandleError, "ControlLoop", "the control loop has ended")

    def _fail_pending(self) -> None:
        """Fail every request still queued when the loop ends."""
        while True:
            try:
                future, _ = self._go.get_nowait()
            except Empty:
                return
            if future.set_running_or_notify_cancel():
                future.set_exception(self._ended_error())


def _status_class(status: int) -> StatusClass:
    return native_library.get().status_class(status)


def _request_arm(device: Device) -> None:
    device.request_arm()


def _clear_fault(device: Device) -> None:
    device.clear_fault()


def _parameter(name: str, value: int, high: int) -> int:
    number = operator.index(value)
    if not 1 <= number <= high:
        raise ValueError(f"{name} must be 1..{high}, not {number}")
    return number


def _count(value: int) -> int:
    number = operator.index(value)
    if number < 0:
        raise ValueError(f"cycles must not be negative, not {number}")
    return number
