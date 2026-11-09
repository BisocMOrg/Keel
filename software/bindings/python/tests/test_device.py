# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Device layer: every one of the 37 functions through the public API, against the virtual-clock simulator."""

from __future__ import annotations

import logging
from typing import Any

import pytest

import keel
from support.native_calls import NativeCallController
from support.simulator import CycleDriver, open_sim

ARMED_INPUTS = keel.SafetyInput.READY | keel.SafetyInput.ARMED


class TestOpen:
    def test_report_and_capabilities(self, sim: keel.Device) -> None:
        report = sim.report
        assert report.status == keel.Status.OK
        assert report.backend == keel.Backend.SIMULATOR
        assert report.warnings == keel.OpenWarning(0)
        assert report.timing_class == keel.TimingClass.NONE
        assert report.resource == keel.OpenResource.NONE
        expected = (
            keel.Capability.MOTORS
            | keel.Capability.BRAKE
            | keel.Capability.RGB
            | keel.Capability.BUTTONS
            | keel.Capability.BUTTON_LEDS
            | keel.Capability.SERVO
            | keel.Capability.RANGE
            | keel.Capability.SIMULATED
        )
        assert sim.capabilities() == expected
        assert report.capabilities == expected
        assert sim.state() == keel.State.DISARMED
        assert not sim.closed
        assert sim.close_status is None
        assert sim.final_fault is None
        assert "open" in repr(sim)

    def test_open_flags_remove_capabilities(self) -> None:
        with open_sim(keel.OpenFlag.NO_SERVO | keel.OpenFlag.NO_RANGE) as device:
            capabilities = device.capabilities()
            assert keel.Capability.SERVO not in capabilities
            assert keel.Capability.RANGE not in capabilities

    def test_command_timeout_option(self) -> None:
        with open_sim(command_timeout_ms=keel.limits.COMMAND_TIMEOUT_MIN_MS) as device:
            driver = CycleDriver(device)
            driver.arm()
            device.set_motors(300, 300)
            assert 0 < device.snapshot().command_remaining_ms <= keel.limits.COMMAND_TIMEOUT_MIN_MS

    def test_devices_cannot_be_constructed_directly(self) -> None:
        with pytest.raises(TypeError, match=r"keel.open\(\)"):
            keel.Device()

    def test_real_time_simulator(self) -> None:
        with open_sim(virtual=False) as device:
            before = device.now_us()
            device.sleep_until(before + 2000)
            assert device.now_us() >= before + 2000


class TestClock:
    def test_virtual_time_moves_only_inside_sleep_until(self, sim: keel.Device) -> None:
        start = sim.now_us()
        assert sim.now_us() == start
        sim.sleep_until(start + 25_000)
        assert sim.now_us() == start + 25_000

    def test_a_deadline_in_the_past_returns_at_once(self, sim: keel.Device) -> None:
        start = sim.now_us()
        sim.sleep_until(0)
        assert sim.now_us() == start

    def test_a_deadline_too_far_ahead_is_refused(self, sim: keel.Device) -> None:
        with pytest.raises(keel.InvalidArgumentError) as caught:
            sim.sleep_until(sim.now_us() + keel.limits.SLEEP_AHEAD_MAX_MS * 1000 + 1_000_000)
        assert caught.value.function == "keel_sleep_until"


class TestSafety:
    def test_tick_cycle_arm_and_disarm(self, sim: keel.Device) -> None:
        sim.tick(1, keel.Health.ALL)
        assert sim.snapshot().last_cycle_id == 1
        driver = CycleDriver(sim)
        snapshot = driver.arm()
        assert snapshot.state == keel.State.ARMED
        assert sim.read_safety_inputs() == ARMED_INPUTS
        assert keel.SnapshotFlag.HEARTBEAT_RUNNING in snapshot.flags
        assert keel.SnapshotFlag.SIMULATED in snapshot.flags
        sim.request_arm()  # already ARMED: a no-op
        sim.disarm()
        assert sim.state() == keel.State.DISARMED

    def test_emergency_stop_latches_user_stop(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        sim.emergency_stop()
        assert sim.state() == keel.State.FAULTED
        fault = sim.fault()
        assert fault.recorded
        assert (fault.status, fault.cause) == (keel.Status.E_FAULTED, keel.FaultCause.USER_STOP)
        assert fault.state == keel.State.ARMED
        assert not fault.sticky
        assert not fault.cleared
        sim.clear_fault()
        assert sim.state() == keel.State.DISARMED
        assert sim.fault().cleared

    def test_no_fault_recorded(self, sim: keel.Device) -> None:
        fault = sim.fault()
        assert not fault.recorded
        assert fault.cause == keel.FaultCause.NONE

    def test_cycle_returns_the_snapshot_and_takes_buttons(self, sim: keel.Device) -> None:
        sim.sim_set(keel.SimItem.BUTTONS, keel.Button.X)
        driver = CycleDriver(sim)
        driver.cycles(4)
        snapshot = driver.cycle(take_buttons=True)
        assert snapshot.buttons_held == keel.Button.X
        assert snapshot.buttons_pressed == keel.Button.X
        assert sim.snapshot().buttons_pressed == keel.Button(0)


class TestMotion:
    def test_drive_coast_and_bridge(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        sim.set_motors(400, -400)
        snapshot = driver.cycle()
        assert (snapshot.command_left, snapshot.command_right) == (400, -400)
        assert (snapshot.output_left, snapshot.output_right) == (400, -400)
        state = sim.sim_state()
        assert (state.bridge_left, state.bridge_right) == (400, -400)
        assert keel.SimFlag.MOTOR_PERMIT in state.flags
        sim.coast()
        snapshot = driver.cycle()
        assert (snapshot.output_left, snapshot.output_right) == (0, 0)
        assert snapshot.state == keel.State.ARMED

    def test_reversal_is_deferred(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        driver.cycles(2, keel.CycleCommand.DRIVE, 300, 300)
        sim.set_motors(-300, -300)
        snapshot = sim.snapshot()
        assert keel.SnapshotFlag.REVERSAL_PENDING in snapshot.flags
        assert snapshot.output_left == 0
        snapshot = driver.cycles(3, keel.CycleCommand.DRIVE, -300, -300)
        assert snapshot.output_left == -300

    def test_command_expires(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        sim.set_motors(250, 250)
        snapshot = driver.wait_ms(keel.limits.COMMAND_TIMEOUT_DEFAULT_MS + 50)
        assert keel.SnapshotFlag.COMMAND_EXPIRED in snapshot.flags
        assert (snapshot.output_left, snapshot.command_left) == (0, 0)
        assert snapshot.state == keel.State.ARMED

    def test_brake(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        sim.brake()
        snapshot = driver.cycle()
        assert keel.SnapshotFlag.BRAKING in snapshot.flags
        flags = sim.sim_state().flags
        assert keel.SimFlag.LEFT_BRAKE in flags
        assert keel.SimFlag.RIGHT_BRAKE in flags

    def test_motion_while_disarmed_is_refused(self, sim: keel.Device) -> None:
        with pytest.raises(keel.StateError):
            sim.set_motors(100, 100)
        with pytest.raises(keel.StateError):
            sim.brake()
        assert sim.snapshot().last_rejection == keel.Status.E_STATE


class TestServo:
    def test_burst_start_pulse_and_end(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.wait_ms(keel.limits.SERVO_COOLDOWN_MS + 50)
        driver.arm()
        driver.wait_ms(keel.limits.SERVO_IDLE_MS + 10)
        sim.servo_start(1500, 300)
        assert sim.state() == keel.State.SERVO
        sim.servo_set_pulse(1200)
        snapshot = driver.cycles(3)
        assert keel.SnapshotFlag.SERVO_GRANTED in snapshot.flags
        assert snapshot.servo_pulse_us == 1200
        assert sim.sim_state().servo_pulse_us == 1200
        sim.servo_end()
        assert sim.state() == keel.State.DISARMED
        sim.servo_end()  # no burst: a no-op

    def test_cooldown_after_open_is_again(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        driver.wait_ms(keel.limits.SERVO_IDLE_MS + 10)
        with pytest.raises(keel.AgainError):
            sim.servo_start(1500, 100)

    def test_servo_without_the_capability_is_unsupported(self) -> None:
        with open_sim(keel.OpenFlag.NO_SERVO) as device:
            with pytest.raises(keel.UnsupportedError):
                device.servo_start(1500, 100)
            with pytest.raises(keel.UnsupportedError):
                device.servo_set_pulse(1500)
            device.servo_end()


class TestRanging:
    def test_measure_start_and_poll(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        assert sim.range_poll().status == keel.RangeStatus.IDLE
        driver.cycles(30)  # two heartbeat falls qualify sensor power
        sim.sim_set(keel.SimItem.RANGE_MM, 750)
        measured = sim.range_measure()
        assert measured.valid
        assert (measured.status, measured.distance_mm) == (keel.RangeStatus.VALID, 750)
        assert sim.snapshot().sensor_power == keel.SensorPower.CONFIRMED
        driver.wait_ms(keel.limits.RANGE_INTERVAL_MS + 10)
        sim.range_start()
        with pytest.raises(keel.AgainError):
            sim.range_start()
        driver.cycles(4)
        polled = sim.range_poll()
        assert (polled.status, polled.distance_mm) == (keel.RangeStatus.VALID, 750)
        assert polled.sequence == measured.sequence + 1

    def test_nothing_in_range_is_no_echo(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.cycles(30)
        sim.sim_set(keel.SimItem.RANGE_MM, -1)
        result = sim.range_measure()
        assert result.status == keel.RangeStatus.NO_ECHO
        assert not result.valid
        assert result.distance_mm == 0

    def test_an_unpowered_sensor_is_unavailable(self, sim: keel.Device) -> None:
        assert sim.range_measure().status == keel.RangeStatus.UNAVAILABLE

    def test_measure_from_the_owner_while_armed_conflicts(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        with pytest.raises(keel.ConflictError):
            sim.range_measure()

    def test_ranging_without_the_capability_is_unsupported(self) -> None:
        with open_sim(keel.OpenFlag.NO_RANGE) as device:
            with pytest.raises(keel.UnsupportedError):
                device.range_start()
            with pytest.raises(keel.UnsupportedError):
                device.range_poll()
            with pytest.raises(keel.UnsupportedError):
                device.range_measure()


class TestLeds:
    def test_rgb_and_the_frame_cache(self, sim: keel.Device) -> None:
        sim.set_rgb(keel.Led.RGB1, 0x102030)
        assert sim.rgb(keel.Led.RGB1) == 0x102030
        sim.set_rgb_all(0x0000FF)
        assert [sim.rgb(index) for index in keel.Led] == [0xFF] * keel.limits.LED_COUNT
        sim.set_rgb_frame([1, 2, 3])
        assert [sim.rgb(index) for index in keel.Led] == [1, 2, 3, 0xFF, 0xFF, 0xFF]
        sim.set_rgb(keel.Led.RGB6, 0xFF123456)  # bits 24-31 are ignored
        assert sim.rgb(keel.Led.RGB6) == 0x123456

    def test_frame_count_is_passed_through(self, sim: keel.Device) -> None:
        with pytest.raises(keel.InvalidArgumentError):
            sim.set_rgb_frame([])
        with pytest.raises(keel.InvalidArgumentError):
            sim.set_rgb_frame(range(7))

    def test_index_out_of_range(self, sim: keel.Device) -> None:
        with pytest.raises(keel.InvalidArgumentError):
            sim.set_rgb(keel.limits.LED_COUNT, 0)
        with pytest.raises(keel.InvalidArgumentError):
            sim.rgb(keel.limits.LED_COUNT)

    def test_leds_work_while_faulted(self, sim: keel.Device) -> None:
        sim.emergency_stop()
        sim.set_rgb_all(0xFF0000)
        assert sim.rgb(keel.Led.RGB3) == 0xFF0000

    def test_button_leds(self, sim: keel.Device) -> None:
        sim.set_button_leds(keel.Button.A | keel.Button.B, keel.Button.A)
        assert sim.snapshot().button_leds == keel.Button.A
        sim.set_button_leds(keel.Button.ALL, keel.Button.ALL)
        assert sim.snapshot().button_leds == keel.Button.ALL
        with pytest.raises(keel.InvalidArgumentError):
            sim.set_button_leds(0x10, 0)


class TestButtons:
    def test_take_buttons_consumes_edges(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        sim.sim_set(keel.SimItem.BUTTONS, keel.Button.A | keel.Button.Y)
        driver.cycles(4)
        taken = sim.take_buttons()
        assert taken.held == keel.Button.A | keel.Button.Y
        assert taken.pressed == keel.Button.A | keel.Button.Y
        assert sim.take_buttons().pressed == keel.Button(0)
        sim.sim_set(keel.SimItem.BUTTONS, 0)
        driver.cycles(4)
        assert sim.take_buttons().released == keel.Button.A | keel.Button.Y


class TestSimulator:
    def test_stimulus_and_ground_truth(self, sim: keel.Device) -> None:
        assert sim.sim_state().now_us == sim.now_us()
        driver = CycleDriver(sim)
        driver.cycles(30)  # the MCU model raises READY once the heartbeat qualifies
        state = sim.sim_state()
        assert keel.SimFlag.READY in state.flags
        assert state.heartbeat_falls >= 2
        assert state.now_us == sim.now_us()
        with pytest.raises(keel.InvalidArgumentError):
            sim.sim_set(0, 1)


QUERIES: list[tuple[str, str, tuple[object, ...]]] = [
    ("capabilities", "keel_get_capabilities", ()),
    ("now_us", "keel_now_us", ()),
    ("state", "keel_get_state", ()),
    ("snapshot", "keel_get_snapshot", ()),
    ("fault", "keel_get_fault", ()),
    ("read_safety_inputs", "keel_read_safety_inputs", ()),
    ("range_poll", "keel_range_poll", ()),
    ("rgb", "keel_get_rgb", (0,)),
    ("take_buttons", "keel_take_buttons", ()),
    ("sim_state", "keel_sim_get_state", ()),
    ("cycle", "keel_cycle", (1, keel.Health.ALL)),
]


class TestFailurePaths:
    @pytest.mark.parametrize(("method", "function", "arguments"), QUERIES)
    def test_a_status_from_libkeel_raises_by_class(
        self, sim: keel.Device, natives: NativeCallController, method: str, function: str, arguments: tuple[object, ...]
    ) -> None:
        natives.overrides[function] = lambda *args: keel.Status.E_NOT_OWNER
        with pytest.raises(keel.HandleError) as caught:
            getattr(sim, method)(*arguments)
        assert caught.value.function == function
        assert caught.value.status == keel.Status.E_NOT_OWNER
        assert "binding refused" not in str(caught.value)

    def test_a_degraded_open_is_logged(self, natives: NativeCallController, caplog: pytest.LogCaptureFixture) -> None:
        original = natives.original["keel_open"]

        def degraded(options: Any, out_device: Any, report: Any) -> int:
            status = int(original(options, out_device, report))
            report._obj.warnings = keel.OpenWarning.NO_RGB
            report._obj.hint = b"connect the LED driver"
            return status

        natives.overrides["keel_open"] = degraded
        with caplog.at_level(logging.WARNING, logger="keel"), open_sim() as device:
            assert device.report.warnings == keel.OpenWarning.NO_RGB
        assert "opened degraded (NO_RGB): connect the LED driver" in caplog.text

    def test_a_device_that_cannot_be_wrapped_is_closed(
        self, natives: NativeCallController, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        def broken(*args: object) -> keel.Device:
            raise MemoryError

        monkeypatch.setattr(keel.Device, "_create", broken)
        with natives.record() as calls, pytest.raises(MemoryError):
            open_sim()
        assert calls == ["keel_open", "keel_close"]
