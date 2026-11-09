# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""keel.open() and Device: one open libkeel device and its 37 functions.

Every Device method passes its arguments to libkeel unchanged and raises by status class (see keel.errors); libkeel
is the only safety logic. The binding itself checks only what libkeel cannot observe: integers that ctypes would
truncate (TypeError or OverflowError before any call; out-of-range duties are saturated so that libkeel refuses
them) and calls on a device it has closed or is closing (HandleError, again before any call).

Threads: every method is safe from any thread. libkeel decides who may do what: "stop" calls (disarm,
emergency_stop, set_motors(0, 0), coast, servo_end and close) are accepted from every thread in every state, "go"
calls only from the control owner, the thread of the first successful tick after open or clear_fault. The stop
methods take no binding lock, so they are safe from signal handlers and finalizers.
"""

from __future__ import annotations

import ctypes
import itertools
import logging
import os
import threading
import warnings
import weakref
from collections.abc import Callable, Iterable
from types import TracebackType
from typing import NoReturn, Self, SupportsIndex

from ._generated import Backend, Capability, OpenFlag, OpenReport
from ._internal import _generated as g
from ._internal import native_library
from ._internal.native_library import NativeLibrary
from .errors import HandleError, KeelError, SetupError, Status
from .errors.error_factory import error_for, refused
from .loop._generated import CycleCommand, CycleFlag
from .peripherals import ButtonState, RangeMeasurement
from .sim import SimState
from .state import Fault, SafetyInput, Snapshot, State

__all__ = ["Device", "open"]

_log = logging.getLogger("keel")

# Statuses for which libkeel leaves an output struct untouched (keel.h, struct versioning).
_UNTOUCHED = frozenset({Status.E_INVALID_ARGUMENT, Status.E_INVALID_HANDLE, Status.E_NOT_OWNER})

_CLOSED = "the device is closed or closing"


class _CloseState:
    """What close() and the finalizer share: the token, the closing flag, the close claim and the close result.

    The finalizer holds this object, never the Device, so a dropped device can be collected.
    """

    __slots__ = (
        "claim",
        "closing",
        "done",
        "final_fault",
        "opener_pid",
        "record",
        "returned",
        "status",
        "token",
    )

    def __init__(self, token: int) -> None:
        self.token = token
        self.closing = False
        self.claim: dict[str, object] = {}
        self.returned: list[int] = []
        self.record = g.FaultStruct()
        self.status: Status | None = None
        self.final_fault: Fault | None = None
        self.done = threading.Event()
        self.opener_pid = os.getpid()


# The close states the current thread is inside close() for. A signal handler or finalizer that re-enters close()
# on the closing thread finds its state here and returns at once: a thread cannot wait for itself.
_inside = threading.local()


def _closing_on_this_thread() -> set[_CloseState]:
    states: set[_CloseState] | None = getattr(_inside, "states", None)
    if states is None:
        states = set()
        _inside.states = states
    return states


def _claim(state: _CloseState, ticket: object) -> bool:
    """Claim the close for ticket and return whether ticket holds it; the first claim wins.

    The claim is recorded in the state by one atomic dict operation, not in a return value, so an exception that
    arrives right after it (a KeyboardInterrupt raised by a signal handler) cannot lose it: the caller's cleanup
    finds that it holds the close and completes it.
    """
    state.claim.setdefault("holder", ticket)
    return state.claim["holder"] is ticket


def _close_now(state: _CloseState, natives: g.Natives) -> None:
    """Issue keel_close for the holder of the claim and publish its result; the completion Event is set at the end.

    Calling it again after an exception interrupted it is safe and completes the close: keel_close is issued exactly
    once, and its status is never lost.
    """
    try:
        if not state.returned:
            calls = itertools.starmap(natives["keel_close"], ((state.token, ctypes.byref(state.record)),))
            # list.extend drains the iterator in C, so the foreign call and the store of its status run with no
            # bytecode boundary in between: a signal handler's exception arrives before or after them, never
            # between them. A non-empty returned therefore means exactly that keel_close was issued.
            state.returned.extend(calls)
        _publish_close(state)
    finally:
        state.done.set()
    if state.status is not None and state.status != Status.OK:
        cleanup = state.final_fault.cleanup_status.name if state.final_fault else "unknown"
        _log.warning("keel_close: %s; cleanup status %s", state.status.name, cleanup)


def _publish_close(state: _CloseState) -> None:
    """Store the status keel_close returned and its final fault record (idempotent)."""
    if state.status is None and state.returned:
        status = Status(state.returned[0])
        if status not in _UNTOUCHED:
            state.final_fault = g.convert_fault(state.record)
        state.status = status


def _mark_foreign(state: _CloseState) -> None:
    """Record a close in a forked child: no foreign call, nothing logged (libkeel would return NOT_OWNER)."""
    state.closing = True
    if state.status is None:
        state.status = Status.E_NOT_OWNER


def _finalize(state: _CloseState, natives: g.Natives) -> None:
    """Close a device that was collected, or still open at interpreter exit, without having been closed."""
    try:
        if os.getpid() != state.opener_pid:
            _mark_foreign(state)
            return
        state.closing = True
        if not _claim(state, object()):
            return  # a close() call holds the close and completes it
        _close_now(state, natives)
        status = state.status.name if state.status is not None else "unknown"
        _log.warning("keel.Device was not closed; its finalizer closed it (status %s)", status)
        warnings.warn("keel.Device was not closed", ResourceWarning, stacklevel=2)
    except Exception:  # noqa: BLE001  (a finalizer must never raise, also during interpreter shutdown)
        pass


def _stop_quietly(device: Device, emergency: bool) -> None:
    """Issue a stop on a path that is already unwinding: log a failure instead of raising it."""
    try:
        if emergency:
            device.emergency_stop()
        else:
            device.disarm()
    except HandleError:
        _log.debug("stop: already stopped by close")
    except KeelError as error:
        _log.error("stop could not be confirmed: %s", error)


class Device:
    """One open libkeel device. Open it with keel.open() and close it with close() or a with block.

    A with block closes the device on exit; when a KeyboardInterrupt (Ctrl-C) escapes it, the device first makes an
    emergency stop, so the final fault record shows USER_STOP. A device that is dropped without being closed is
    closed by a finalizer, which also emits a ResourceWarning.

    Properties hold only stored data; every call into libkeel is a method.
    """

    __slots__ = ("__weakref__", "_finalizer", "_loop_attached", "_natives", "_report", "_state")

    _state: _CloseState
    _natives: g.Natives
    _report: OpenReport
    _loop_attached: bool

    def __init__(self, *_: object) -> None:
        """Refuse direct construction: devices come from keel.open()."""
        raise TypeError("use keel.open() to open a device")

    @classmethod
    def _create(cls, library: NativeLibrary, token: int, report: OpenReport) -> Self:
        device = object.__new__(cls)
        device._setup(library.natives, token, report)
        return device

    def _setup(self, natives: g.Natives, token: int, report: OpenReport) -> None:
        self._state = _CloseState(token)
        self._natives = natives
        self._report = report
        self._loop_attached = False
        # The finalizer holds the close state, never the device, so a dropped device can be collected.
        self._finalizer: weakref.finalize[[_CloseState, g.Natives], Device] = weakref.finalize(
            self, _finalize, self._state, natives
        )

    # ------------------------------------------------------------ lifetime --

    def __enter__(self) -> Self:
        """Return the device itself."""
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        """Close the device; after a KeyboardInterrupt, make an emergency stop first."""
        try:
            if exc_type is not None and issubclass(exc_type, KeyboardInterrupt):
                _log.info("KeyboardInterrupt: emergency stop")
                _stop_quietly(self, emergency=True)
        finally:
            self.close()

    def __repr__(self) -> str:
        """Show the token and whether the device is open."""
        return f"<keel.Device token={self._state.token:#x} {'closed' if self._state.closing else 'open'}>"

    def close(self) -> None:
        """Close the device: a Category-0 stop, LEDs off, every resource released. Idempotent; never fails.

        When close() returns, keel_close has returned, so an immediate reopen cannot find the lines still held by
        this device. A concurrent second caller waits for the first (about 100 ms at most). The status is
        diagnostic only: it is stored in close_status, the final fault record in final_fault, and a non-OK status
        is logged at WARNING. From the moment close() starts, every other method raises HandleError without a
        foreign call. A KeyboardInterrupt (Ctrl-C) that arrives while close() runs never skips the close: it
        propagates only after keel_close has returned. In a forked child, close() makes no foreign call and sets
        close_status to E_NOT_OWNER.
        """
        state = self._state
        ticket = object()
        reentered = False
        try:
            state.closing = True
            inside = _closing_on_this_thread()
            if state in inside:
                reentered = True
                return
            inside.add(state)
            if os.getpid() != state.opener_pid:
                _mark_foreign(state)
            elif _claim(state, ticket):
                _close_now(state, self._natives)
            else:
                state.done.wait()
        finally:
            if not reentered:
                try:
                    if os.getpid() == state.opener_pid and state.status is None and _claim(state, ticket):
                        # An exception, typically a KeyboardInterrupt raised by a signal handler, interrupted this
                        # call before the close was complete: complete it before the exception propagates.
                        _close_now(state, self._natives)
                finally:
                    _closing_on_this_thread().discard(state)
                    self._finalizer.detach()

    @property
    def report(self) -> OpenReport:
        """The open report: backend, capabilities, warnings and timing class of this device."""
        return self._report

    @property
    def closed(self) -> bool:
        """True once close() has started (the device is closed or closing)."""
        return self._state.closing

    @property
    def close_status(self) -> Status | None:
        """The status keel_close returned, or None while the device is open or closing."""
        return self._state.status

    @property
    def final_fault(self) -> Fault | None:
        """The final fault record written by keel_close, including the cleanup status of its stop, or None."""
        return self._state.final_fault

    # ------------------------------------------------------------- helpers --

    def _token(self, function: str) -> int:
        """Return the token, or raise HandleError without a foreign call once close() has started."""
        state = self._state
        if state.closing:
            raise refused(HandleError, function, _CLOSED)
        return state.token

    def _fail(self, status: int, function: str, snapshot: Snapshot | None = None) -> NoReturn:
        raise error_for(status, function, token=self._state.token, snapshot=snapshot)

    def _u32(self, function: str, call: Callable[..., int]) -> int:
        token = self._token(function)
        value = ctypes.c_uint32()
        status = call(self._natives, token, ctypes.byref(value))
        if status != Status.OK:
            self._fail(status, function)
        return value.value

    def _go(self, function: str, call: Callable[..., int]) -> None:
        status = call(self._natives, self._token(function))
        if status != Status.OK:
            self._fail(status, function)

    # ------------------------------------------------------- device queries --

    def capabilities(self) -> Capability:
        """Return the features of this device (keel_get_capabilities; no I/O; any thread)."""
        return Capability(self._u32("keel_get_capabilities", g.keel_get_capabilities))

    def now_us(self) -> int:
        """Return the device clock in microseconds (keel_now_us; any thread).

        CLOCK_MONOTONIC on hardware and the real-time simulator; the virtual clock of a virtual-clock simulator.
        """
        token = self._token("keel_now_us")
        value = ctypes.c_uint64()
        status = g.keel_now_us(self._natives, token, ctypes.byref(value))
        if status != Status.OK:
            self._fail(status, "keel_now_us")
        return value.value

    def sleep_until(self, deadline_us: SupportsIndex) -> None:
        """Sleep until an absolute device time, at most limits.SLEEP_AHEAD_MAX_MS ahead (keel_sleep_until).

        Returns at once if the deadline has passed. On a virtual-clock simulator it advances the clock instead of
        sleeping. close() wakes it early with HandleError. The GIL is released while it sleeps.
        """
        status = g.keel_sleep_until(self._natives, self._token("keel_sleep_until"), deadline_us)
        if status != Status.OK:
            self._fail(status, "keel_sleep_until")

    # ------------------------------------------------------- safety and loop --

    def tick(self, cycle_id: SupportsIndex, health: SupportsIndex) -> None:
        """One health-loop step (keel_tick; the control owner only).

        cycle_id must strictly increase over the device's lifetime. health has no default: pass Health.ALL only
        after this cycle's control work and I/O really succeeded; anything less fail-stops on purpose. ControlLoop
        does this for you.
        """
        status = g.keel_tick(self._natives, self._token("keel_tick"), cycle_id, health)
        if status != Status.OK:
            self._fail(status, "keel_tick")

    def cycle(
        self,
        cycle_id: SupportsIndex,
        health: SupportsIndex,
        *,
        command: SupportsIndex = CycleCommand.NONE,
        left: SupportsIndex = 0,
        right: SupportsIndex = 0,
        take_buttons: bool = False,
    ) -> Snapshot:
        """Tick, apply a motor command and read the state in one call (keel_cycle; the control owner only).

        The duties are passed through unvalidated; libkeel refuses an out-of-range duty and, while ARMED, coasts.
        With take_buttons the snapshot's press and release edges are consumed. On an error, the exception's
        snapshot holds the snapshot the call wrote, or None when libkeel left it untouched. ControlLoop is the
        normal user of this method.
        """
        flags = CycleFlag.TAKE_BUTTONS if take_buttons else CycleFlag(0)
        cycle_input = g.fill_cycle_input(
            g.CycleInputStruct(), health=health, cycle_id=cycle_id, command=command, left=left, right=right, flags=flags
        )
        out = g.SnapshotStruct()
        token = self._token("keel_cycle")
        status = g.keel_cycle(self._natives, token, ctypes.byref(cycle_input), ctypes.byref(out))
        if status != Status.OK:
            self._fail(status, "keel_cycle", None if status in _UNTOUCHED else g.convert_snapshot(out))
        return g.convert_snapshot(out)

    def request_arm(self) -> None:
        """Request motor permission: an explicit operator or application decision (keel_request_arm; owner only).

        Arming completes over about 0.6-0.85 s of continuing ticks. Never call it automatically after a fault.
        """
        self._go("keel_request_arm", g.keel_request_arm)

    def disarm(self) -> None:
        """Category-0 stop: remove motor permission now, without latching (keel_disarm; any thread).

        Raises SafetyFaultError (for example E_IO) when the stop could not be confirmed. Takes no binding lock.
        """
        self._go("keel_disarm", g.keel_disarm)

    def emergency_stop(self) -> None:
        """Latching Category-0 stop for stop buttons and Ctrl-C (keel_emergency_stop; any thread, any state).

        Returns normally when the stop succeeded: the device is then FAULTED with cause USER_STOP, and leaving that
        needs clear_fault() from the control owner. Takes no binding lock, so a Python signal handler may call it.
        """
        self._go("keel_emergency_stop", g.keel_emergency_stop)

    def clear_fault(self) -> None:
        """Leave FAULTED without arming: an explicit decision, never a retry loop (keel_clear_fault; owner only).

        Sticky causes cannot be cleared (SafetyFaultError with E_FAULTED); close and reopen the device.
        """
        self._go("keel_clear_fault", g.keel_clear_fault)

    def state(self) -> State:
        """Return the coarse safety state (keel_get_state; cached, no I/O; any thread)."""
        return State(self._u32("keel_get_state", g.keel_get_state))

    def snapshot(self) -> Snapshot:
        """Return the cached device state and diagnostics; consumes nothing (keel_get_snapshot; any thread)."""
        token = self._token("keel_get_snapshot")
        out = g.SnapshotStruct()
        status = g.keel_get_snapshot(self._natives, token, ctypes.byref(out))
        if status != Status.OK:
            self._fail(status, "keel_get_snapshot")
        return g.convert_snapshot(out)

    def fault(self) -> Fault:
        """Return the fault record: status OK and cause NONE when none was recorded (keel_get_fault; any thread)."""
        token = self._token("keel_get_fault")
        out = g.FaultStruct()
        status = g.keel_get_fault(self._natives, token, ctypes.byref(out))
        if status != Status.OK:
            self._fail(status, "keel_get_fault")
        return g.convert_fault(out)

    def read_safety_inputs(self) -> SafetyInput:
        """Read READY and ARMED fresh (keel_read_safety_inputs; one GPIO read; any thread). Diagnostic only."""
        return SafetyInput(self._u32("keel_read_safety_inputs", g.keel_read_safety_inputs))

    # ----------------------------------------------------------------- motion --

    def set_motors(self, left: SupportsIndex, right: SupportsIndex) -> None:
        """Set both motor duties in signed permille, -1000..1000 (keel_set_motors).

        A non-zero command needs ARMED and the control owner; (0, 0) is a stop accepted from any thread. The duties
        are not validated by the binding: libkeel refuses an out-of-range duty and, while ARMED, coasts both motors.
        Values outside the int32 range are saturated so that libkeel still sees them as out of range.
        """
        status = g.keel_set_motors(self._natives, self._token("keel_set_motors"), left, right)
        if status != Status.OK:
            self._fail(status, "keel_set_motors")

    def coast(self) -> None:
        """Coast both motors: set_motors(0, 0), a stop accepted from any thread in every state."""
        self.set_motors(0, 0)

    def brake(self) -> None:
        """Brake both motors while staying armed (keel_brake; the control owner only)."""
        self._go("keel_brake", g.keel_brake)

    # ------------------------------------------------------------------ servo --

    def servo_start(self, pulse_us: SupportsIndex, lease_ms: SupportsIndex) -> None:
        """Start a servo burst (keel_servo_start; the control owner only, while ARMED).

        Raises UnsupportedError on devices without Capability.SERVO, such as the keel_r3 board (J5 not fitted).
        """
        status = g.keel_servo_start(self._natives, self._token("keel_servo_start"), pulse_us, lease_ms)
        if status != Status.OK:
            self._fail(status, "keel_servo_start")

    def servo_set_pulse(self, pulse_us: SupportsIndex) -> None:
        """Change the pulse width of the active burst; never extends it (keel_servo_set_pulse; owner only)."""
        status = g.keel_servo_set_pulse(self._natives, self._token("keel_servo_set_pulse"), pulse_us)
        if status != Status.OK:
            self._fail(status, "keel_servo_set_pulse")

    def servo_end(self) -> None:
        """End the active burst early; a no-op without one (keel_servo_end; any thread)."""
        self._go("keel_servo_end", g.keel_servo_end)

    # ---------------------------------------------------------------- ranging --

    def range_start(self) -> None:
        """Trigger an ultrasonic measurement without blocking (keel_range_start; any thread); the form for loops."""
        self._go("keel_range_start", g.keel_range_start)

    def range_poll(self) -> RangeMeasurement:
        """Return the latest measurement without blocking (keel_range_poll; any thread).

        Only RangeStatus.VALID carries a distance; NO_ECHO means nothing was detected, never that the path is clear.
        """
        return self._range("keel_range_poll", g.keel_range_poll)

    def range_measure(self) -> RangeMeasurement:
        """Trigger and wait for one measurement, at most 35 ms (keel_range_measure).

        Any thread except the control owner while ARMING, ARMED or SERVO, which gets ConflictError.
        """
        return self._range("keel_range_measure", g.keel_range_measure)

    def _range(self, function: str, call: Callable[..., int]) -> RangeMeasurement:
        token = self._token(function)
        out = g.RangeMeasurementStruct()
        status = call(self._natives, token, ctypes.byref(out))
        if status != Status.OK:
            self._fail(status, function)
        return g.convert_range_measurement(out)

    # ------------------------------------------------------------------- LEDs --

    def set_rgb(self, index: SupportsIndex, rgb: SupportsIndex) -> None:
        """Set one RGB LED, index 0..5, to 0x00RRGGBB (keel_set_rgb; any thread, every state).

        Bits 24-31 of rgb are ignored. Raises PeripheralError for an I2C failure and UnsupportedError without
        Capability.RGB.
        """
        status = g.keel_set_rgb(self._natives, self._token("keel_set_rgb"), index, rgb)
        if status != Status.OK:
            self._fail(status, "keel_set_rgb")

    def set_rgb_all(self, rgb: SupportsIndex) -> None:
        """Set all six RGB LEDs to one colour in one I2C frame (keel_set_rgb_all; any thread)."""
        status = g.keel_set_rgb_all(self._natives, self._token("keel_set_rgb_all"), rgb)
        if status != Status.OK:
            self._fail(status, "keel_set_rgb_all")

    def set_rgb_frame(self, colors: Iterable[SupportsIndex]) -> None:
        """Set the first len(colors) RGB LEDs, RGB1 first, in one atomic I2C frame (keel_set_rgb_frame).

        The count is passed through: libkeel refuses an empty or a longer-than-six sequence with
        InvalidArgumentError. LEDs beyond the sequence keep their colour.
        """
        values = [g.check_u32("keel_set_rgb_frame", f"rgb24[{i}]", color) for i, color in enumerate(colors)]
        frame = (ctypes.c_uint32 * len(values))(*values)
        status = g.keel_set_rgb_frame(self._natives, self._token("keel_set_rgb_frame"), frame, len(values))
        if status != Status.OK:
            self._fail(status, "keel_set_rgb_frame")

    def rgb(self, index: SupportsIndex) -> int:
        """Return the colour last written successfully to one RGB LED (keel_get_rgb; cached, no I/O)."""
        token = self._token("keel_get_rgb")
        value = ctypes.c_uint32()
        status = g.keel_get_rgb(self._natives, token, index, ctypes.byref(value))
        if status != Status.OK:
            self._fail(status, "keel_get_rgb")
        return value.value

    def set_button_leds(self, mask: SupportsIndex, values: SupportsIndex) -> None:
        """Switch the button indicator LEDs in mask to the levels in values (keel_set_button_leds; any thread).

        A failure is FAULT class: these lines share the control request with the motor enable and the heartbeat.
        """
        status = g.keel_set_button_leds(self._natives, self._token("keel_set_button_leds"), mask, values)
        if status != Status.OK:
            self._fail(status, "keel_set_button_leds")

    # ---------------------------------------------------------------- buttons --

    def take_buttons(self) -> ButtonState:
        """Read the buttons and consume the edges since the previous take (keel_take_buttons; any thread)."""
        token = self._token("keel_take_buttons")
        out = g.ButtonStateStruct()
        status = g.keel_take_buttons(self._natives, token, ctypes.byref(out))
        if status != Status.OK:
            self._fail(status, "keel_take_buttons")
        return g.convert_button_state(out)

    # -------------------------------------------------------------- simulator --

    def sim_set(self, item: SupportsIndex, value: SupportsIndex) -> None:
        """Apply a simulator stimulus (keel_sim_set; any thread). UnsupportedError on hardware."""
        status = g.keel_sim_set(self._natives, self._token("keel_sim_set"), item, value)
        if status != Status.OK:
            self._fail(status, "keel_sim_set")

    def sim_state(self) -> SimState:
        """Return the simulator's ground truth (keel_sim_get_state; any thread). UnsupportedError on hardware."""
        token = self._token("keel_sim_get_state")
        out = g.SimStateStruct()
        status = g.keel_sim_get_state(self._natives, token, ctypes.byref(out))
        if status != Status.OK:
            self._fail(status, "keel_sim_get_state")
        return g.convert_sim_state(out)


def open(  # noqa: A001  (keel.open, like the C API's keel_open)
    *,
    backend: SupportsIndex = Backend.AUTO,
    flags: SupportsIndex = OpenFlag(0),
    command_timeout_ms: SupportsIndex = 0,
    gpiochip_path: str | bytes = "",
    i2c_path: str | bytes = "",
) -> Device:
    """Open a device (keel_open; up to about 100 ms; any thread).

    Every zero or empty value means "default", as in keel_open_options: with all defaults this equals
    keel_open(NULL, ...). Backend.AUTO selects the backend named by the environment variable KEEL_BACKEND ("r3" or
    "simulator") if set, else the keel_r3 board if the library has it, and never falls back to the simulator
    silently. Pass Backend.SIMULATOR (with OpenFlag.VIRTUAL_CLOCK for deterministic tests) to open the simulator.

    Raises SetupError, carrying the open report with its remedy, for every failed open. On success, a degraded open
    (OpenWarning.NO_RGB, BEST_EFFORT_TIMING) is logged at WARNING with the report's hint and stays visible in
    Device.report. Loads the library first if necessary (LibraryError).
    """
    library = native_library.get()
    options = g.fill_open_options(
        g.OpenOptionsStruct(),
        backend=backend,
        flags=flags,
        command_timeout_ms=command_timeout_ms,
        gpiochip_path=gpiochip_path,
        i2c_path=i2c_path,
    )
    raw = ctypes.c_void_p()
    out = g.OpenReportStruct()
    status = g.keel_open(library.natives, ctypes.byref(options), ctypes.byref(raw), ctypes.byref(out))
    report = g.convert_open_report(out)
    if status != Status.OK:
        raise SetupError(status, "keel_open", report=report)
    token = raw.value or 0
    try:
        device = Device._create(library, token, report)
    except BaseException:
        g.keel_close(library.natives, token, None)
        raise
    if report.warnings:
        _log.warning("keel_open: the device opened degraded (%s): %s", report.warnings.name, report.hint)
    return device
