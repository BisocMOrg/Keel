# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""ControlLoop: health, duties, the mailbox and deadman, faulted mode, ownership, ending and statistics.

Most cases drive a virtual-clock simulator synchronously with run(), so arm, drive, fault and recovery sequences are
deterministic and take milliseconds; the cases about the loop's own thread use the real-time simulator.
"""

from __future__ import annotations

import asyncio
import gc
import logging
import threading
import time
from typing import Any

import pytest

import keel
from support.native_calls import NativeCallController
from support.simulator import open_sim


def armed(snapshot: keel.Snapshot) -> bool:
    return snapshot.state == keel.State.ARMED


def faulted(snapshot: keel.Snapshot) -> bool:
    return snapshot.state == keel.State.FAULTED


def arm(loop: keel.ControlLoop) -> keel.Snapshot:
    future = loop.request_arm()
    snapshot = loop.run(until=armed, cycles=300)
    assert future.result() is None
    assert armed(snapshot), snapshot.state
    return snapshot


def raise_when(condition: Any, error: BaseException) -> None:
    """Wait until condition() is true, then raise error."""
    wait_for(condition)
    raise error


def wait_for(condition: Any, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while not condition():
        if time.monotonic() > deadline:
            raise AssertionError("condition not met in time")
        time.sleep(0.005)


class TestArmAndDrive:
    def test_arm_through_the_future(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim)
        future = loop.request_arm()
        assert not future.done()
        loop.run(cycles=1)
        assert future.result() is None
        assert loop.run(until=armed, cycles=300).state == keel.State.ARMED

    def test_the_step_drives(self, sim: keel.Device) -> None:
        def step(cycle: keel.Cycle) -> None:
            assert cycle.device is sim
            if armed(cycle.snapshot):
                cycle.drive(200, -200)

        loop = keel.ControlLoop(sim, step)
        arm(loop)
        snapshot = loop.run(cycles=3)
        assert (snapshot.output_left, snapshot.output_right) == (200, -200)
        assert loop.snapshot is snapshot
        assert sim.sim_state().bridge_left == 200
        assert gc.get_freeze_count() > 0

    def test_brake_and_coast_from_the_step(self, sim: keel.Device) -> None:
        commands: list[str] = []

        def step(cycle: keel.Cycle) -> None:
            if commands:
                getattr(cycle, commands[-1])()

        loop = keel.ControlLoop(sim, step)
        arm(loop)
        commands.append("brake")
        assert keel.SnapshotFlag.BRAKING in loop.run(cycles=2).flags
        commands.append("coast")
        snapshot = loop.run(cycles=2)
        assert keel.SnapshotFlag.BRAKING not in snapshot.flags
        assert snapshot.output_left == 0

    def test_cycle_fields(self, sim: keel.Device) -> None:
        seen: list[tuple[int, int]] = []
        loop = keel.ControlLoop(sim, lambda cycle: seen.append((cycle.cycle_id, cycle.now_us)))
        start = sim.now_us()
        loop.run(cycles=3)
        assert [cycle_id for cycle_id, _ in seen] == [1, 2, 3]
        assert [now - start for _, now in seen] == [0, 10_000, 20_000]


class TestHealth:
    def test_a_step_exception_fail_stops_with_health_flags(self, sim: keel.Device) -> None:
        faults: list[keel.Fault] = []
        calls: list[int] = []

        def step(cycle: keel.Cycle) -> None:
            calls.append(cycle.cycle_id)
            raise RuntimeError("sensor glitch")

        loop = keel.ControlLoop(sim, step, on_fault=faults.append)
        snapshot = loop.run(cycles=5)
        assert faulted(snapshot)
        assert loop.fault is not None
        assert (loop.fault.cause, loop.fault.detail) == (keel.FaultCause.HEALTH_FLAGS, keel.Health.NO_EXCEPTION)
        assert faults == [loop.fault]
        assert isinstance(loop.error, RuntimeError)
        assert calls == [1, 2, 3, 4, 5]  # steps keep running in faulted mode
        stats = loop.stats()
        assert (stats.step_errors, stats.faults, stats.cycles) == (5, 1, 5)

    @pytest.mark.parametrize("catch", [False, True])
    def test_a_refused_duty_fails_the_cycle(self, sim: keel.Device, catch: bool) -> None:
        refused: list[keel.InvalidArgumentError] = []

        def step(cycle: keel.Cycle) -> None:
            if armed(cycle.snapshot):
                try:
                    cycle.drive(1001, 0)
                except keel.InvalidArgumentError as error:
                    refused.append(error)
                    if not catch:
                        raise

        loop = keel.ControlLoop(sim, step)
        arm(loop)
        snapshot = loop.run(cycles=1)
        assert faulted(snapshot)
        assert loop.fault is not None
        assert (loop.fault.cause, loop.fault.detail) == (keel.FaultCause.HEALTH_FLAGS, keel.Health.NO_EXCEPTION)
        assert refused[0].status == keel.Status.E_INVALID_ARGUMENT
        assert "no foreign call was made" in str(refused[0])
        future = loop.clear_fault()
        snapshot = loop.run(cycles=1)
        assert future.result() is None
        assert snapshot.state == keel.State.DISARMED

    def test_a_non_integer_duty_fails_the_cycle(self, sim: keel.Device) -> None:
        def step(cycle: keel.Cycle) -> None:
            cycle.drive(0.5, 0)  # type: ignore[arg-type]

        loop = keel.ControlLoop(sim, step)
        assert faulted(loop.run(cycles=1))
        assert isinstance(loop.error, TypeError)

    def test_a_budget_overrun_clears_control_ok(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim, lambda cycle: cycle.device.sleep_until(cycle.now_us + 6000), step_budget_ms=5)
        loop.run(cycles=1)
        assert loop.fault is not None
        assert (loop.fault.cause, loop.fault.detail) == (keel.FaultCause.HEALTH_FLAGS, keel.Health.CONTROL_OK)
        assert loop.stats().max_step_us == 6000

    def test_a_slow_submitted_function_counts_against_the_budget(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim, step_budget_ms=5)
        future = loop.submit(lambda device: device.sleep_until(device.now_us() + 6000))
        loop.run(cycles=1)
        assert future.result() is None
        assert loop.fault is not None
        assert loop.fault.detail == keel.Health.CONTROL_OK

    def test_io_failure_clears_io_ok(self, sim: keel.Device) -> None:
        def step(cycle: keel.Cycle) -> None:
            cycle.io_ok = False

        loop = keel.ControlLoop(sim, step)
        loop.run(cycles=1)
        assert loop.fault is not None
        assert loop.fault.detail == keel.Health.IO_OK


class TestMailbox:
    def test_set_command_is_one_shot_and_the_deadman_expires_it(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim)
        arm(loop)
        loop.set_command(250, 250)
        first = loop.run(cycles=1)
        assert (first.command_left, first.output_left) == (250, 250)
        later = loop.run(cycles=10)
        assert later.command_left == 250
        assert later.command_remaining_ms < first.command_remaining_ms  # aged, never renewed
        expired = loop.run(cycles=50)
        assert keel.SnapshotFlag.COMMAND_EXPIRED in expired.flags
        assert (expired.output_left, expired.state) == (0, keel.State.ARMED)

    def test_the_last_intent_wins_and_brake_and_coast_post_intents(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim)
        arm(loop)
        loop.set_command(100, 100)
        loop.set_command(300, -300)
        assert loop.run(cycles=1).output_right == -300
        loop.brake()
        assert keel.SnapshotFlag.BRAKING in loop.run(cycles=1).flags
        loop.coast()
        assert loop.run(cycles=1).output_left == 0

    def test_the_steps_command_wins(self, sim: keel.Device) -> None:
        def step(cycle: keel.Cycle) -> None:
            if armed(cycle.snapshot):
                cycle.drive(100, 100)

        loop = keel.ControlLoop(sim, step)
        arm(loop)
        loop.set_command(300, 300)
        assert loop.run(cycles=1).output_left == 100

    def test_an_out_of_range_intent_raises_and_coasts(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim)
        arm(loop)
        loop.set_command(300, 300)
        assert loop.run(cycles=1).output_left == 300
        with pytest.raises(keel.InvalidArgumentError, match="a coast was posted instead"):
            loop.set_command(1001, 0)
        snapshot = loop.run(cycles=1)
        assert (snapshot.output_left, snapshot.state) == (0, keel.State.ARMED)
        loop.set_command(300, 300)
        loop.run(cycles=1)
        with pytest.raises(TypeError, match="a coast was posted instead"):
            loop.set_command(None, 0)  # type: ignore[arg-type]
        assert loop.run(cycles=1).output_left == 0
        assert loop.fault is None

    def test_no_motion_is_sent_while_faulted(self, sim: keel.Device, natives: NativeCallController) -> None:
        commands: list[int] = []
        original = natives.original["keel_cycle"]

        def spy(device: int, cycle_input: Any, out: Any) -> int:
            commands.append(cycle_input._obj.command)
            return int(original(device, cycle_input, out))

        loop = keel.ControlLoop(sim)
        arm(loop)
        natives.overrides["keel_cycle"] = spy
        loop.set_command(300, 300)
        loop.run(cycles=1)
        sim.emergency_stop()
        loop.set_command(300, 300)
        loop.run(cycles=1)
        assert commands == [keel.CycleCommand.DRIVE, keel.CycleCommand.NONE]


class TestFaultedMode:
    def test_clear_fault_and_the_hold_off_through_the_loop(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim)
        arm(loop)
        sim.emergency_stop()
        assert faulted(loop.run(cycles=1))
        assert loop.fault is not None
        assert loop.fault.cause == keel.FaultCause.USER_STOP
        cleared = loop.clear_fault()
        loop.run(cycles=1)
        assert cleared.result() is None
        refused = loop.request_arm()
        assert loop.run(cycles=1).state == keel.State.DISARMED
        with pytest.raises(keel.StateError):
            refused.result()
        assert keel.SnapshotFlag.REARM_HOLDOFF in loop.snapshot.flags
        loop.run(cycles=keel.limits.REARM_HOLDOFF_MS // keel.limits.TICK_PERIOD_MS)
        arm(loop)

    def test_on_fault_errors_do_not_end_the_loop(self, sim: keel.Device) -> None:
        def on_fault(fault: keel.Fault) -> None:
            raise ValueError("display offline")

        loop = keel.ControlLoop(sim, on_fault=on_fault)
        sim.emergency_stop()
        loop.run(cycles=3)
        assert isinstance(loop.error, ValueError)
        assert loop.stats().cycles == 3

    def test_a_fault_that_latches_again_in_the_clearing_cycle_is_reported(self, sim: keel.Device) -> None:
        faults: list[keel.Fault] = []

        def step(cycle: keel.Cycle) -> None:
            cycle.io_ok = False  # the cause persists, so every healthy-looking chance latches again

        loop = keel.ControlLoop(sim, step, on_fault=faults.append)
        assert faulted(loop.run(cycles=5))
        assert [fault.fault_count for fault in faults] == [1]
        cleared = loop.clear_fault()
        snapshot = loop.run(cycles=5)
        assert cleared.result() is None
        assert faulted(snapshot)
        assert snapshot.fault_count == 2
        assert [fault.fault_count for fault in faults] == [1, 2]
        assert loop.stats().faults == 2
        assert loop.fault is faults[-1]

    def test_a_clock_fault_while_sleeping_ends_the_loop(self, sim: keel.Device, natives: NativeCallController) -> None:
        # keel_sleep_until fails only with the clock's sticky fault: the loop cannot pace itself any more.
        natives.overrides["keel_sleep_until"] = lambda device, deadline: keel.Status.E_INTERNAL
        loop = keel.ControlLoop(sim)
        with natives.record() as calls, pytest.raises(keel.SafetyFaultError) as caught:
            loop.run(cycles=3)
        assert caught.value.function == "keel_sleep_until"
        assert loop.stats().cycles == 1
        assert "keel_disarm" in calls
        with pytest.raises(keel.SafetyFaultError):
            loop.run(cycles=1)


class TestOutcomes:
    @pytest.mark.parametrize(
        ("status", "error_type"),
        [
            (keel.Status.E_INVALID_ARGUMENT, keel.InvalidArgumentError),
            (keel.Status.E_UNSUPPORTED, keel.UnsupportedError),
        ],
    )
    def test_a_malformed_input_is_fatal(
        self, sim: keel.Device, natives: NativeCallController, status: keel.Status, error_type: type[keel.KeelError]
    ) -> None:
        loop = keel.ControlLoop(sim)
        natives.overrides["keel_cycle"] = lambda device, cycle_input, out: status
        with natives.record() as calls, pytest.raises(error_type) as caught:
            loop.run(cycles=5)
        assert "keel_disarm" in calls
        assert loop.stats().cycles == 1
        assert loop.error is caught.value
        assert (caught.value.snapshot is None) == (status == keel.Status.E_INVALID_ARGUMENT)
        with pytest.raises(error_type):
            loop.run(cycles=1)
        with pytest.raises(keel.HandleError, match="the control loop has ended"):
            loop.request_arm().result()

    def test_other_refusals_are_counted(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim, lambda cycle: cycle.drive(100, 100))
        snapshot = loop.run(cycles=5)
        assert snapshot.state == keel.State.DISARMED
        assert snapshot.last_rejection == keel.Status.E_STATE
        stats = loop.stats()
        assert dict(stats.rejected) == {keel.Status.E_STATE: 5}
        assert (stats.cycles, stats.faults, stats.overruns, stats.max_gap_us) == (5, 0, 0, 10_000)

    def test_a_peripheral_result_continues(self, sim: keel.Device, natives: NativeCallController) -> None:
        natives.overrides["keel_cycle"] = lambda device, cycle_input, out: keel.Status.E_PERIPHERAL
        loop = keel.ControlLoop(sim)
        loop.run(cycles=2)
        assert loop.stats().cycles == 2
        assert loop.error is None

    def test_a_handle_result_without_close_is_stored(self, sim: keel.Device, natives: NativeCallController) -> None:
        natives.overrides["keel_cycle"] = lambda device, cycle_input, out: keel.Status.E_INVALID_HANDLE
        loop = keel.ControlLoop(sim)
        with pytest.raises(keel.HandleError):
            loop.run(cycles=3)
        assert loop.stats().cycles == 1

    @pytest.mark.parametrize("function", ["keel_now_us", "keel_sleep_until"])
    def test_a_failed_clock_call_ends_the_loop(
        self, sim: keel.Device, natives: NativeCallController, function: str
    ) -> None:
        loop = keel.ControlLoop(sim)
        loop.run(cycles=1)
        natives.overrides[function] = lambda *args: keel.Status.E_INVALID_HANDLE
        with pytest.raises(keel.HandleError):
            loop.run(cycles=3)

    def test_a_rejected_sleep_is_fatal(self, sim: keel.Device, natives: NativeCallController) -> None:
        natives.overrides["keel_sleep_until"] = lambda *args: keel.Status.E_INVALID_ARGUMENT
        with pytest.raises(keel.InvalidArgumentError):
            keel.ControlLoop(sim).run(cycles=3)

    def test_close_from_the_step_is_a_normal_end(self, sim: keel.Device) -> None:
        def step(cycle: keel.Cycle) -> None:
            if cycle.cycle_id == 2:
                cycle.device.close()

        loop = keel.ControlLoop(sim, step)
        loop.run(cycles=10)
        assert loop.stats().cycles == 1
        assert loop.error is None
        assert loop.run(cycles=1) is loop.snapshot

    def test_close_from_another_thread_ends_a_running_loop_normally(self) -> None:
        device = open_sim(virtual=False)
        loop = keel.ControlLoop(device)
        loop.start()
        wait_for(lambda: loop.stats().cycles > 3)
        device.close()
        loop.join(timeout=5)
        assert not loop.running
        assert loop.error is None

    def test_stop_and_join_from_inside_the_step(self, sim: keel.Device) -> None:
        results: dict[str, Any] = {}

        def step(cycle: keel.Cycle) -> None:
            if cycle.cycle_id == 3:
                try:
                    loop.join()
                except RuntimeError as error:
                    results["join"] = error
                loop.stop()
                results["stopped"] = True

        loop = keel.ControlLoop(sim, step)
        loop.run(cycles=10)
        assert results["stopped"]
        assert "own thread" in str(results["join"])
        assert loop.stats().cycles == 3
        assert loop.run(cycles=5) is loop.snapshot  # a stopped loop runs no more cycles
        assert loop.stats().cycles == 3
        with pytest.raises(RuntimeError, match="has ended"):
            loop.start()

    def test_keyboard_interrupt_in_the_step_is_an_emergency_stop(self, sim: keel.Device) -> None:
        def step(cycle: keel.Cycle) -> None:
            if cycle.cycle_id == 2:
                raise KeyboardInterrupt

        loop = keel.ControlLoop(sim, step)
        with pytest.raises(KeyboardInterrupt):
            loop.run(cycles=5)
        assert sim.fault().cause == keel.FaultCause.USER_STOP
        assert isinstance(loop.error, KeyboardInterrupt)

    def test_system_exit_in_the_step_disarms(self, sim: keel.Device) -> None:
        def step(cycle: keel.Cycle) -> None:
            if armed(cycle.snapshot):
                raise SystemExit(0)

        loop = keel.ControlLoop(sim, step)
        loop.request_arm()
        with pytest.raises(SystemExit):
            loop.run(cycles=300)
        assert sim.state() == keel.State.DISARMED
        assert not sim.fault().recorded

    def test_an_until_error_ends_the_loop(self, sim: keel.Device) -> None:
        def until(snapshot: keel.Snapshot) -> bool:
            raise LookupError("broken predicate")

        loop = keel.ControlLoop(sim)
        with pytest.raises(LookupError):
            loop.run(cycles=3, until=until)

    def test_submitted_functions_and_their_errors(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim)
        value = loop.submit(lambda device: device.state())
        failing = loop.submit(lambda device: device.set_motors(100, 100))
        cancelled = loop.submit(lambda device: None)
        assert cancelled.cancel()
        loop.run(cycles=1)
        assert value.result() == keel.State.DISARMED
        with pytest.raises(keel.StateError):
            failing.result()
        assert loop.error is None

    def test_a_keyboard_interrupt_in_a_submitted_function_ends_the_loop(self, sim: keel.Device) -> None:
        def interrupt(device: keel.Device) -> None:
            raise KeyboardInterrupt

        loop = keel.ControlLoop(sim)
        future = loop.submit(interrupt)
        with pytest.raises(KeyboardInterrupt):
            loop.run(cycles=2)
        assert isinstance(future.exception(), KeyboardInterrupt)

    def test_stop_before_run(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim)
        loop.stop()
        assert loop.run(cycles=5).last_cycle_id == 0
        with pytest.raises(keel.HandleError):
            loop.submit(lambda device: None).result()

    def test_stop_on_a_closed_device(self) -> None:
        device = open_sim()
        loop = keel.ControlLoop(device)
        device.close()
        loop.stop(emergency=True)
        loop.run(cycles=1)
        assert loop.error is None

    def test_run_on_a_closed_device_is_a_normal_end(self) -> None:
        device = open_sim()
        loop = keel.ControlLoop(device)
        device.close()
        loop.run(cycles=3)
        assert loop.stats().cycles == 0
        assert loop.error is None
        with pytest.raises(keel.HandleError, match="the control loop has ended"):
            loop.submit(lambda device: None).result()


class TestOwnership:
    def test_start_is_refused_with_a_recorded_owner(self, sim: keel.Device) -> None:
        sim.tick(1, keel.Health.ALL)
        loop = keel.ControlLoop(sim)
        with pytest.raises(RuntimeError, match="already has a control owner"):
            loop.start()

    def test_run_is_refused_for_a_foreign_owner(self, sim: keel.Device) -> None:
        thread = threading.Thread(target=sim.tick, args=(1, keel.Health.ALL))
        thread.start()
        thread.join()
        loop = keel.ControlLoop(sim)
        with pytest.raises(RuntimeError, match="not this thread"):
            loop.run(cycles=1)

    def test_run_continues_the_calling_threads_own_ticks(self, sim: keel.Device) -> None:
        sim.tick(41, keel.Health.ALL)
        loop = keel.ControlLoop(sim)
        assert loop.run(cycles=1).last_cycle_id == 42
        assert loop.run(cycles=2).last_cycle_id == 44

    def test_a_second_loop_is_refused(self, sim: keel.Device, natives: NativeCallController) -> None:
        keel.ControlLoop(sim)
        with natives.record() as calls, pytest.raises(RuntimeError, match="already has a ControlLoop"):
            keel.ControlLoop(sim)
        assert calls == []

    def test_run_is_refused_on_an_event_loop_thread(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim)

        async def main() -> None:
            with pytest.raises(RuntimeError, match="event-loop thread"):
                loop.run(cycles=1)

        asyncio.run(main())


class TestThreadMode:
    def test_with_block_starts_stops_and_joins(self) -> None:
        with open_sim(virtual=False) as device:
            loop = keel.ControlLoop(device, name="keel-test-loop")
            with loop:
                wait_for(lambda: loop.stats().cycles > 3)
                assert loop.running
                assert any(thread.name == "keel-test-loop" for thread in threading.enumerate())
                with pytest.raises(RuntimeError, match="already running"):
                    loop.start()
                with pytest.raises(RuntimeError, match="already running"):
                    loop.run(cycles=1)
            assert not loop.running
            assert device.state() == keel.State.DISARMED

    def test_keyboard_interrupt_in_the_with_block_is_an_emergency_stop(self) -> None:
        with open_sim(virtual=False) as device:
            loop = keel.ControlLoop(device)
            with pytest.raises(KeyboardInterrupt), loop:
                raise_when(lambda: loop.stats().cycles > 1, KeyboardInterrupt())
            assert device.fault().cause == keel.FaultCause.USER_STOP

    def test_join_with_a_timeout_returns_while_running(self) -> None:
        with open_sim(virtual=False) as device:
            loop = keel.ControlLoop(device)
            loop.start()
            loop.join(timeout=0.02)
            assert loop.running
            loop.stop()
            loop.join()

    def test_the_loops_fatal_error_is_raised_from_the_with_block(self, natives: NativeCallController) -> None:
        with open_sim(virtual=False) as device:
            natives.overrides["keel_cycle"] = lambda *args: keel.Status.E_INVALID_ARGUMENT
            loop = keel.ControlLoop(device)
            with pytest.raises(keel.InvalidArgumentError), loop:
                wait_for(lambda: not loop.running and loop.error is not None)


class TestParameters:
    @pytest.mark.parametrize(("name", "value"), [("period_ms", 0), ("period_ms", 11), ("step_budget_ms", 41)])
    def test_out_of_range_parameters(self, sim: keel.Device, name: str, value: int) -> None:
        with pytest.raises(ValueError, match=name):
            keel.ControlLoop(sim, **{name: value})

    def test_bad_arguments(self, sim: keel.Device) -> None:
        with pytest.raises(TypeError, match=r"keel.Device"):
            keel.ControlLoop(object())  # type: ignore[arg-type]
        with pytest.raises(TypeError, match="step"):
            keel.ControlLoop(sim, step=5)  # type: ignore[arg-type]
        with pytest.raises(TypeError, match="on_fault"):
            keel.ControlLoop(sim, on_fault=5)  # type: ignore[arg-type]
        loop = keel.ControlLoop(sim)
        with pytest.raises(ValueError, match="cycles"):
            loop.run(cycles=-1)

    def test_a_shorter_period(self, sim: keel.Device) -> None:
        loop = keel.ControlLoop(sim, period_ms=5)
        start = sim.now_us()
        loop.run(cycles=4)
        assert sim.now_us() - start == 20_000
        assert loop.run(cycles=0).last_cycle_id == 4

    def test_without_taking_buttons(self, sim: keel.Device) -> None:
        sim.sim_set(keel.SimItem.BUTTONS, keel.Button.B)
        loop = keel.ControlLoop(sim, take_buttons=False)
        loop.run(cycles=5)
        assert sim.snapshot().buttons_pressed == keel.Button.B


class TestDiagnostics:
    def test_a_missed_period_is_an_overrun_not_a_fault(
        self, sim: keel.Device, caplog: pytest.LogCaptureFixture
    ) -> None:
        def step(cycle: keel.Cycle) -> None:
            if cycle.cycle_id == 2:
                cycle.device.sleep_until(cycle.now_us + 25_000)  # within the 40 ms budget, past the period

        loop = keel.ControlLoop(sim, step)
        with caplog.at_level(logging.WARNING, logger="keel"):
            snapshot = loop.run(cycles=4)
        assert snapshot.state == keel.State.DISARMED
        assert loop.stats().overruns == 1
        assert loop.fault is None
        assert "deadline resynchronised" in caplog.text

    def test_a_cycle_makes_four_foreign_calls(self, sim: keel.Device, natives: NativeCallController) -> None:
        # Each foreign call releases the GIL and has to win it back, so the loop makes as few as it can.
        loop = keel.ControlLoop(sim)
        loop.run(cycles=1)
        with natives.record() as calls:
            loop.run(cycles=10)
        cycles = calls[calls.index("keel_now_us") + 1 :]  # after the deadline is set at the start of run()
        assert cycles == ["keel_now_us", "keel_now_us", "keel_cycle", "keel_sleep_until"] * 10

    def test_a_fault_record_that_cannot_be_read(self, sim: keel.Device, natives: NativeCallController) -> None:
        sim.emergency_stop()
        natives.overrides["keel_get_fault"] = lambda *args: keel.Status.E_INVALID_HANDLE
        loop = keel.ControlLoop(sim)
        loop.run(cycles=2)
        assert loop.stats().faults == 1
        assert loop.fault is None

    def test_a_stop_that_fails_while_ending_is_logged(
        self, sim: keel.Device, natives: NativeCallController, caplog: pytest.LogCaptureFixture
    ) -> None:
        natives.overrides["keel_cycle"] = lambda *args: keel.Status.E_UNSUPPORTED
        natives.overrides["keel_disarm"] = lambda *args: keel.Status.E_IO
        loop = keel.ControlLoop(sim)
        with caplog.at_level(logging.ERROR, logger="keel"), pytest.raises(keel.UnsupportedError):
            loop.run(cycles=2)
        assert "the stop could not be confirmed" in caplog.text

    def test_requests_still_queued_when_the_loop_ends_fail(self, sim: keel.Device) -> None:
        late: list[Any] = []

        def step(cycle: keel.Cycle) -> None:
            late.append(loop.submit(lambda device: device.state()))
            loop.stop()

        loop = keel.ControlLoop(sim, step)
        loop.run(cycles=3)
        with pytest.raises(keel.HandleError, match="the control loop has ended"):
            late[0].result()

    def test_the_with_block_logs_a_stop_failure_and_a_loop_error(
        self, natives: NativeCallController, caplog: pytest.LogCaptureFixture
    ) -> None:
        with open_sim(virtual=False) as device:
            natives.overrides["keel_cycle"] = lambda *args: keel.Status.E_INVALID_ARGUMENT
            loop = keel.ControlLoop(device)

            def disarm_fails_once_the_loop_has_ended() -> bool:
                if loop.error is None:
                    return False
                natives.overrides["keel_disarm"] = lambda *args: keel.Status.E_IO
                return True

            with caplog.at_level(logging.ERROR, logger="keel"), pytest.raises(LookupError), loop:
                raise_when(disarm_fails_once_the_loop_has_ended, LookupError("the application failed"))
            assert "the stop could not be confirmed" in caplog.text
            assert "the loop had ended with an error" in caplog.text
