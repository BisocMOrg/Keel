# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Lifetime: close, the finalizer, use after close, signals, fork and with blocks."""

from __future__ import annotations

import gc
import json
import logging
import os
import signal
import threading
import time
import warnings
from typing import Any

import pytest

import keel
from keel._internal import _generated as g
from support.native_calls import NativeCallController
from support.simulator import CycleDriver, open_sim

# SIGHUP and SIGQUIT do not exist on Windows, where this module is still collected for the portability tests.
POSIX_ONLY_SIGNALS = [getattr(signal, name) for name in ("SIGHUP", "SIGQUIT") if hasattr(signal, name)]

METHODS: list[tuple[str, tuple[Any, ...]]] = [
    ("capabilities", ()),
    ("now_us", ()),
    ("sleep_until", (0,)),
    ("tick", (1, keel.Health.ALL)),
    ("cycle", (1, keel.Health.ALL)),
    ("request_arm", ()),
    ("disarm", ()),
    ("emergency_stop", ()),
    ("clear_fault", ()),
    ("state", ()),
    ("snapshot", ()),
    ("fault", ()),
    ("read_safety_inputs", ()),
    ("set_motors", (0, 0)),
    ("coast", ()),
    ("brake", ()),
    ("servo_start", (1500, 100)),
    ("servo_set_pulse", (1500,)),
    ("servo_end", ()),
    ("range_start", ()),
    ("range_poll", ()),
    ("range_measure", ()),
    ("set_rgb", (0, 0)),
    ("set_rgb_all", (0,)),
    ("set_rgb_frame", ([0],)),
    ("rgb", (0,)),
    ("set_button_leds", (1, 1)),
    ("take_buttons", ()),
    ("sim_set", (keel.SimItem.BUTTONS, 0)),
    ("sim_state", ()),
]


class TestClose:
    def test_close_is_idempotent_and_stores_its_result(self, natives: NativeCallController) -> None:
        device = open_sim()
        with natives.record() as calls:
            device.close()
            device.close()
        assert calls == ["keel_close"]
        assert device.closed
        assert device.close_status == keel.Status.OK
        assert device.final_fault is not None
        assert not device.final_fault.recorded
        assert device.final_fault.cleanup_status == keel.Status.OK
        assert "closed" in repr(device)

    def test_a_concurrent_close_waits_for_keel_close(self, natives: NativeCallController) -> None:
        device = open_sim()
        original = natives.original["keel_close"]
        entered = threading.Event()
        finished: list[float] = []

        def slow_close(token: int, fault: Any) -> int:
            entered.set()
            time.sleep(0.1)
            status = int(original(token, fault))
            finished.append(time.monotonic())
            return status

        natives.overrides["keel_close"] = slow_close
        first = threading.Thread(target=device.close)
        first.start()
        entered.wait()
        device.close()
        returned = time.monotonic()
        first.join()
        assert finished
        assert returned >= finished[0]
        assert device.close_status == keel.Status.OK

    def test_the_final_fault_record(self) -> None:
        device = open_sim()
        device.emergency_stop()
        device.close()
        assert device.final_fault is not None
        assert device.final_fault.cause == keel.FaultCause.USER_STOP

    def test_a_failed_close_is_logged_never_raised(
        self, natives: NativeCallController, caplog: pytest.LogCaptureFixture
    ) -> None:
        device = open_sim()
        original = natives.original["keel_close"]

        def failing(token: int, fault: Any) -> int:
            original(token, fault)
            return keel.Status.E_IO

        natives.overrides["keel_close"] = failing
        with caplog.at_level(logging.WARNING, logger="keel"):
            device.close()
        assert device.close_status == keel.Status.E_IO
        assert "keel_close: E_IO; cleanup status OK" in caplog.text

    @pytest.mark.parametrize(("name", "arguments"), METHODS)
    def test_use_after_close_makes_no_foreign_call(
        self, natives: NativeCallController, name: str, arguments: tuple[Any]
    ) -> None:
        device = open_sim()
        device.close()
        with natives.record() as calls, pytest.raises(keel.HandleError) as caught:
            getattr(device, name)(*arguments)
        assert calls == []
        assert caught.value.status == keel.Status.E_INVALID_HANDLE
        assert "the binding refused the call: the device is closed or closing; no foreign call was made" in str(
            caught.value
        )

    def test_use_during_close_makes_no_foreign_call(self, natives: NativeCallController) -> None:
        device = open_sim()
        original = natives.original["keel_close"]
        entered = threading.Event()
        release = threading.Event()

        def held_close(token: int, fault: Any) -> int:
            entered.set()
            release.wait(5)
            return int(original(token, fault))

        natives.overrides["keel_close"] = held_close
        closer = threading.Thread(target=device.close)
        closer.start()
        entered.wait()
        try:
            with natives.record() as calls, pytest.raises(keel.HandleError):
                device.disarm()
            assert calls == []
        finally:
            release.set()
            closer.join()

    def test_close_reentered_from_a_signal_handler_returns_at_once(self, natives: NativeCallController) -> None:
        device = open_sim()
        reentered: list[float] = []

        def handler(signum: int, frame: object) -> None:
            start = time.monotonic()
            device.close()
            reentered.append(time.monotonic() - start)

        original = natives.original["keel_close"]

        def close_with_signal(token: int, fault: Any) -> int:
            signal.raise_signal(signal.SIGUSR1)  # the handler runs here, on the closing thread
            return int(original(token, fault))

        natives.overrides["keel_close"] = close_with_signal
        previous = signal.signal(signal.SIGUSR1, handler)
        try:
            device.close()
        finally:
            signal.signal(signal.SIGUSR1, previous)
        assert len(reentered) == 1
        assert reentered[0] < 0.05
        assert device.close_status == keel.Status.OK

    @pytest.mark.parametrize("point", ["before the claim", "after the claim", "before keel_close", "after keel_close"])
    def test_an_interrupt_during_close_never_skips_keel_close(
        self, natives: NativeCallController, monkeypatch: pytest.MonkeyPatch, point: str
    ) -> None:
        # A KeyboardInterrupt from a signal handler arrives at the next bytecode boundary of the main thread; each
        # stand-in below raises one at a boundary of close(), once.
        device = open_sim()
        token = device._state.token
        fired: list[str] = []

        def interrupt() -> None:
            if not fired:
                fired.append(point)
                raise KeyboardInterrupt

        claim, close_now, publish = keel.device._claim, keel.device._close_now, keel.device._publish_close

        def claim_then(state: Any, ticket: object) -> bool:
            if point == "before the claim":
                interrupt()
            held = claim(state, ticket)
            if point == "after the claim":
                interrupt()
            return held

        def close_now_then(state: Any, functions: Any) -> None:
            if point == "before keel_close":
                interrupt()
            close_now(state, functions)

        def publish_then(state: Any) -> None:
            if point == "after keel_close":
                interrupt()
            publish(state)

        monkeypatch.setattr(keel.device, "_claim", claim_then)
        monkeypatch.setattr(keel.device, "_close_now", close_now_then)
        monkeypatch.setattr(keel.device, "_publish_close", publish_then)
        with natives.record() as calls, pytest.raises(KeyboardInterrupt):
            device.close()
        assert fired == [point]
        assert calls == ["keel_close"]
        assert device.closed
        assert device.close_status == keel.Status.OK
        assert device.final_fault is not None
        assert not device._finalizer.alive
        assert g.keel_close(natives.library.natives, token, None) == keel.Status.E_INVALID_HANDLE
        with natives.record() as again:
            device.close()  # idempotent afterwards, with no second foreign call
        assert again == []


class TestFinalizer:
    def test_a_dropped_device_is_closed_and_warns(
        self, natives: NativeCallController, caplog: pytest.LogCaptureFixture
    ) -> None:
        device = open_sim()
        token = device._state.token
        with warnings.catch_warnings(record=True) as caught, caplog.at_level(logging.WARNING, logger="keel"):
            warnings.simplefilter("always")
            del device
            gc.collect()
        assert any(issubclass(item.category, ResourceWarning) for item in caught)
        assert "keel.Device was not closed" in str(caught[0].message)
        assert "its finalizer closed it (status OK)" in caplog.text
        assert g.keel_close(natives.library.natives, token, None) == keel.Status.E_INVALID_HANDLE

    def test_the_finalizer_never_raises(self, natives: NativeCallController) -> None:
        device = open_sim()
        token = device._state.token
        with warnings.catch_warnings():
            warnings.simplefilter("error")  # the ResourceWarning becomes an exception inside the finalizer
            del device
            gc.collect()
        assert g.keel_close(natives.library.natives, token, None) == keel.Status.E_INVALID_HANDLE


class TestSignals:
    def test_sigterm_stops_every_target_even_when_one_fails(
        self, natives: NativeCallController, caplog: pytest.LogCaptureFixture
    ) -> None:
        failing, stopped, closed = open_sim(), open_sim(), open_sim()
        closed.close()
        loop = keel.ControlLoop(stopped)
        original = natives.original["keel_emergency_stop"]
        failing_token = failing._state.token

        def emergency_stop(token: int) -> int:
            return keel.Status.E_IO if token == failing_token else int(original(token))

        natives.overrides["keel_emergency_stop"] = emergency_stop
        previous = signal.getsignal(signal.SIGTERM)
        try:
            with (
                caplog.at_level(logging.ERROR, logger="keel"),
                pytest.raises(SystemExit) as caught,
                keel.stop_on_signals(failing, loop, closed),
            ):
                signal.raise_signal(signal.SIGTERM)
            assert caught.value.code == 128 + signal.SIGTERM
            assert stopped.fault().cause == keel.FaultCause.USER_STOP
            assert "the emergency stop could not be confirmed" in caplog.text
            assert signal.getsignal(signal.SIGTERM) == previous
        finally:
            failing.close()
            stopped.close()

    def test_sigint_raises_keyboard_interrupt(self) -> None:
        with open_sim() as device:
            with pytest.raises(KeyboardInterrupt), keel.stop_on_signals(device, signals=(signal.SIGINT,)):
                signal.raise_signal(signal.SIGINT)
            assert device.fault().cause == keel.FaultCause.USER_STOP

    def test_the_default_signals_are_those_of_the_libkeel_tools(self) -> None:
        with open_sim() as device, keel.stop_on_signals(device):
            for number in (signal.SIGINT, signal.SIGTERM, *POSIX_ONLY_SIGNALS):
                handler = signal.getsignal(number)
                assert callable(handler)
                assert getattr(handler, "__qualname__", "").startswith("stop_on_signals")

    @pytest.mark.parametrize("signum", POSIX_ONLY_SIGNALS)
    def test_a_hangup_or_quit_stops_and_exits_with_128_plus_the_signal(self, signum: int) -> None:
        with open_sim() as device:
            with pytest.raises(SystemExit) as caught, keel.stop_on_signals(device):
                signal.raise_signal(signum)
            assert caught.value.code == 128 + signum
            assert device.fault().cause == keel.FaultCause.USER_STOP

    def test_a_failed_install_restores_the_handlers_installed_before_it(self) -> None:
        before = signal.getsignal(signal.SIGINT)
        with (
            open_sim() as device,
            pytest.raises(OSError, match="Invalid argument"),
            keel.stop_on_signals(device, signals=(signal.SIGINT, signal.SIGKILL)),
        ):
            pass  # pragma: no cover  (never entered: installing SIGKILL fails)
        assert signal.getsignal(signal.SIGINT) is before

    def test_targets_are_checked(self) -> None:
        with pytest.raises(TypeError, match="Device or ControlLoop"), keel.stop_on_signals(object()):  # type: ignore[arg-type]
            pass


def raise_inside(error: BaseException, devices: list[keel.Device], *, arm: bool = True) -> None:
    """Open a simulator device in a with block, arm it unless told otherwise, then raise error inside the block."""
    with open_sim() as device:
        devices.append(device)
        if arm:
            CycleDriver(device).arm()
        raise error


class TestWithBlocks:
    def test_keyboard_interrupt_is_an_emergency_stop(self) -> None:
        devices: list[keel.Device] = []
        with pytest.raises(KeyboardInterrupt):
            raise_inside(KeyboardInterrupt(), devices)
        device = devices[0]
        assert device.closed
        assert device.final_fault is not None
        assert device.final_fault.cause == keel.FaultCause.USER_STOP

    def test_system_exit_just_closes(self) -> None:
        devices: list[keel.Device] = []
        with pytest.raises(SystemExit):
            raise_inside(SystemExit(0), devices)
        device = devices[0]
        assert device.closed
        assert device.final_fault is not None
        assert not device.final_fault.recorded

    def test_a_failed_emergency_stop_is_logged_and_close_still_runs(
        self, natives: NativeCallController, caplog: pytest.LogCaptureFixture
    ) -> None:
        natives.overrides["keel_emergency_stop"] = lambda token: keel.Status.E_IO
        devices: list[keel.Device] = []
        with caplog.at_level(logging.ERROR, logger="keel"), pytest.raises(KeyboardInterrupt):
            raise_inside(KeyboardInterrupt(), devices, arm=False)
        assert devices[0].closed
        assert "stop could not be confirmed" in caplog.text


@pytest.mark.skipif(not hasattr(os, "fork"), reason="needs os.fork")
def test_a_forked_child_gets_handle_errors_and_stays_silent(natives: NativeCallController) -> None:
    device = open_sim()
    dropped = open_sim()
    reader, writer = os.pipe()
    # The child calls the binding only and leaves with os._exit, so forking a process with threads is harmless here.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)
        pid = os.fork()
    if pid == 0:  # pragma: no cover  (runs in the child; reported through the pipe)
        result: dict[str, Any] = {}
        try:
            try:
                device.state()
            except keel.HandleError as error:
                result["state"] = error.status.name
            with warnings.catch_warnings(record=True) as caught, natives.record() as calls:
                warnings.simplefilter("always")
                device.close()
                device.close()
                del dropped  # its finalizer runs in the child and must stay silent
                gc.collect()
            result["close_status"] = device.close_status.name if device.close_status is not None else None
            result["calls"] = calls
            result["warnings"] = [str(item.message) for item in caught]
        finally:
            os.write(writer, json.dumps(result).encode())
            os._exit(0)
    os.close(writer)
    del dropped  # the parent's copy closes through its finalizer below
    with warnings.catch_warnings(record=True):
        warnings.simplefilter("always")
        gc.collect()
    data = b""
    while chunk := os.read(reader, 4096):
        data += chunk
    os.close(reader)
    os.waitpid(pid, 0)
    result = json.loads(data)
    assert result == {"state": "E_NOT_OWNER", "close_status": "E_NOT_OWNER", "calls": [], "warnings": []}
    assert device.state() == keel.State.DISARMED
    device.close()
    assert device.close_status == keel.Status.OK
