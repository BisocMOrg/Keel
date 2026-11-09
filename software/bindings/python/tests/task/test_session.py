# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Interactive session contracts against the guarded native real-time simulator."""

from __future__ import annotations

import threading
import time
from collections.abc import Callable

import pytest

import keel
from keel.task import robot_session


def options() -> keel.TaskOptions:
    return keel.TaskOptions(board_kwargs={"backend": keel.Backend.SIMULATOR})


def wait_for(condition: Callable[[], bool], timeout: float = 3) -> None:
    deadline = time.monotonic() + timeout
    while not condition():
        if time.monotonic() >= deadline:
            pytest.fail("session state did not reach the expected condition")
        time.sleep(0.005)


def test_session_requires_explicit_arm_and_expires_original_lease() -> None:
    with keel.RobotSession(options()) as robot:
        with pytest.raises(keel.TaskError, match="Explicitly arm"):
            robot.refresh_drive(20, 20, 100)
        robot.arm()
        deadline = time.monotonic_ns() + 180_000_000
        robot.refresh_drive_until(20, 20, deadline)
        wait_for(lambda: robot.snapshot().output_left == 200)
        time.sleep(0.06)
        robot.refresh_drive_until(20, 20, deadline)
        wait_for(lambda: robot.snapshot().state == keel.State.DISARMED)
        assert time.monotonic_ns() < deadline + 200_000_000
        assert robot.snapshot().output_left == 0
        with pytest.raises(keel.TaskError):
            robot.refresh_drive(20, 20, 100)


def test_continuous_refresh_has_no_forced_stop_and_zero_retains_arming() -> None:
    with keel.RobotSession(options()) as robot:
        robot.arm()
        robot.refresh_drive(20, 20, 200)
        wait_for(lambda: robot.snapshot().output_left == 200)
        for _ in range(4):
            time.sleep(0.03)
            assert robot.snapshot().output_left == 200
            robot.refresh_drive(20, 20, 200)
        robot.refresh_drive(0, 0, 50)
        wait_for(lambda: robot.snapshot().output_left == 0)
        time.sleep(0.08)
        assert robot.snapshot().state == keel.State.ARMED


def test_expired_lease_cannot_be_renewed_before_controller_observes_it() -> None:
    with keel.RobotSession(options()) as robot:
        robot.arm()
        with robot._lock:
            robot.refresh_drive(20, 20, 1)
            time.sleep(0.003)
            with pytest.raises(keel.TaskError, match="Explicitly arm"):
                robot.refresh_drive(20, 20, 100)
        wait_for(lambda: robot.snapshot().state == keel.State.DISARMED)


def test_coast_cannot_erase_an_expired_lease() -> None:
    with keel.RobotSession(options()) as robot:
        robot.arm()
        with robot._lock:
            robot.refresh_drive(20, 20, 1)
            time.sleep(0.003)
            robot.coast()
            with pytest.raises(keel.TaskError, match="Explicitly arm"):
                robot.refresh_drive(20, 20, 100)
        wait_for(lambda: robot.snapshot().state == keel.State.DISARMED)


def test_queued_arm_cannot_undo_newer_stop() -> None:
    with keel.RobotSession(options()) as robot:
        entered, release = threading.Event(), threading.Event()

        def guard() -> None:
            entered.set()
            assert release.wait(1)

        request = robot.request_arm(guard)
        assert entered.wait(1)
        robot.disarm()
        release.set()
        with pytest.raises(keel.TaskError, match="cancelled"):
            request.result(timeout=2)
        assert robot.snapshot().state != keel.State.ARMED


def test_application_guard_refuses_arm_and_fault_recovery_never_rearms() -> None:
    with keel.RobotSession(options()) as robot:

        def expired() -> None:
            raise PermissionError("controller lease expired")

        with pytest.raises(PermissionError):
            robot.request_arm(expired).result(timeout=2)
        assert robot.snapshot().state == keel.State.DISARMED
        robot.emergency_stop()
        assert robot.fault().cause == keel.FaultCause.USER_STOP
        robot.clear_fault()
        wait_for(lambda: robot.snapshot().state == keel.State.DISARMED)
        with pytest.raises(keel.TaskError):
            robot.refresh_drive(20, 20, 100)


def test_peripherals_work_without_arming_and_session_closes() -> None:
    robot = keel.RobotSession(options())
    with robot:
        robot.set_rgb_all(0x123456)
        assert robot.rgb(keel.Led.RGB1) == 0x123456
        robot.set_rgb_frame([1, 2, 3, 4, 5, 6])
        assert robot.rgb(keel.Led.RGB6) == 6
        robot.set_button_leds(keel.Button.ALL, keel.Button.A)
        robot.sim_set(keel.SimItem.RANGE_MM, 500)
        result = robot.measure_distance()
        assert result.valid
        assert result.distance_mm == 500
        assert robot.snapshot().state == keel.State.DISARMED
        assert robot.stats().cycles > 0
    with pytest.raises(keel.TaskError, match="closed"):
        robot.snapshot()


def test_exit_still_closes_when_emergency_stop_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    robot = object.__new__(keel.RobotSession)
    calls = []

    def failed_stop() -> None:
        calls.append("stop")
        raise RuntimeError("native stop failed")

    monkeypatch.setattr(robot, "emergency_stop", failed_stop)
    monkeypatch.setattr(robot, "close", lambda: calls.append("close"))
    interruption = KeyboardInterrupt()
    robot.__exit__(KeyboardInterrupt, interruption, None)
    assert calls == ["stop", "close"]
    assert "native stop failed" in interruption.__notes__[0]


def test_constructor_failure_stops_joins_and_closes(monkeypatch: pytest.MonkeyPatch) -> None:
    calls = []

    class Device:
        def close(self) -> None:
            calls.append("close")

    class BrokenLoop:
        def __init__(self, *args, **kwargs):
            pass

        def start(self):
            raise RuntimeError("start failed")

        def stop(self):
            calls.append("stop")

        def join(self):
            calls.append("join")

    monkeypatch.setattr(robot_session.device_api, "open", lambda **kwargs: Device())
    monkeypatch.setattr(robot_session, "ControlLoop", BrokenLoop)
    with pytest.raises(RuntimeError, match="start failed"):
        keel.RobotSession()
    assert calls == ["stop", "join", "close"]


def test_close_attempts_every_resource_after_stop_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    robot = keel.RobotSession(options())
    original_stop = robot._loop.stop
    calls = []

    def failed_stop() -> None:
        original_stop()
        calls.append("stop")
        raise RuntimeError("stop failed")

    monkeypatch.setattr(robot._loop, "stop", failed_stop)
    try:
        with pytest.raises(RuntimeError, match="stop failed"):
            robot.close()
    finally:
        original_stop()
        robot._device.close()
    assert calls == ["stop"]
    assert not robot._loop.running
    assert robot._device.closed


def test_task_peripheral_facade_and_terminal_emergency_stop() -> None:
    class Peripherals(keel.KeelTask):
        def run(self) -> None:
            assert keel.Capability.SIMULATED in self.keel.capabilities()
            assert self.keel.snapshot().state == keel.State.DISARMED
            self.keel.set_rgb(keel.Led.RGB1, 0x123456)
            self.keel.set_rgb_all(0x234567)
            self.keel.set_rgb_frame([1, 2, 3, 4, 5, 6])
            self.keel.set_button_leds(keel.Button.ALL, keel.Button.A)
            self.keel.sim_set(keel.SimItem.RANGE_MM, 300)
            result = self.keel.measure_distance()
            assert result.valid
            assert result.distance_mm == 300
            self.keel.emergency_stop()

    with pytest.raises(keel.TaskError, match="emergency-stopped"):
        keel.run(Peripherals(), options())


def test_session_guards_queued_peripherals_and_validates_bounds() -> None:
    def denied() -> None:
        raise PermissionError("new controller owns the robot")

    with keel.RobotSession(options()) as robot:
        with pytest.raises(PermissionError):
            robot.set_rgb(keel.Led.RGB1, 1, denied)
        with pytest.raises(PermissionError):
            robot.set_rgb_all(1, denied)
        with pytest.raises(PermissionError):
            robot.set_rgb_frame([1] * 6, denied)
        with pytest.raises(PermissionError):
            robot.set_button_leds(15, 15, denied)
        with pytest.raises(PermissionError):
            robot.measure_distance(guard=denied)
        with pytest.raises(ValueError, match="max_motion_ms"):
            robot.refresh_drive(10, 10, 30_001)
        with pytest.raises(keel.TaskError, match="expired"):
            robot.refresh_drive_until(10, 10, 0)
        with pytest.raises(TypeError):
            robot.refresh_drive_until(10, 10, True)
        assert robot.buttons_pressed() == 0
        assert robot.buttons_released() == 0
        robot.coast()


def test_explicit_stop_does_not_cancel_subsequent_fault_indication() -> None:
    with keel.RobotSession(options()) as robot:
        robot.emergency_stop()
        robot.set_rgb_all(0xFF0000)
        assert robot.rgb(keel.Led.RGB1) == 0xFF0000
        robot.clear_fault()


def test_task_remains_cancelled_when_native_emergency_stop_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    def failed_stop(device: keel.Device) -> None:
        raise RuntimeError("native emergency stop failed")

    monkeypatch.setattr(keel.Device, "emergency_stop", failed_stop)

    class StopTask(keel.KeelTask):
        def run(self) -> None:
            with pytest.raises(RuntimeError, match="native emergency stop"):
                self.keel.emergency_stop()
            with pytest.raises(keel.TaskError, match="emergency-stopped"):
                self.keel.drive_for(10, 10, 100)

    with pytest.raises(keel.TaskError, match="emergency-stopped"):
        keel.run(StopTask(), options())
