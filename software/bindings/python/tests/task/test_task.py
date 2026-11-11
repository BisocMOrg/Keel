# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Platform-independent task contracts against a controllable threaded board model."""

from __future__ import annotations

import threading
import time
from concurrent.futures import Future
from queue import Empty, SimpleQueue
from types import SimpleNamespace

import pytest

import keel
from keel.task import keel_task, task_robot

pytestmark = pytest.mark.portability


class FakeDevice:
    def __init__(self) -> None:
        self.state = keel.State.DISARMED
        self.arm_calls = 0
        self.disarm_calls = 0
        self.closed = False
        self.arm_allowed = True
        self.closed_with_output = None
        self.left = self.right = 0
        self.samples = []

    def request_arm(self):
        self.arm_calls += 1
        self.state = keel.State.ARMED if self.arm_allowed else keel.State.ARMING

    def disarm(self):
        self.disarm_calls += 1
        self.left = self.right = 0
        if self.state != keel.State.FAULTED:
            self.state = keel.State.DISARMED

    def close(self):
        self.closed_with_output = (self.left, self.right)
        self.disarm()
        self.closed = True


class FakeCycle:
    def __init__(self, loop):
        self.device = loop.device
        self.cycle_id = loop.snapshot.last_cycle_id + 1
        self.snapshot = loop.snapshot
        self.command = (0, 0)

    def drive(self, left, right):
        self.command = (left, right)

    def coast(self):
        self.command = (0, 0)


class FakeLoop:
    def __init__(self, device, step, **kwargs):
        self.device = device
        self.step = step
        self.running = False
        self.error = None
        self.queue = SimpleQueue()
        self.snapshot = self.make_snapshot(0)
        self.thread = None

    def make_snapshot(self, cycle_id):
        return SimpleNamespace(
            state=self.device.state,
            last_cycle_id=cycle_id,
            command_left=self.device.left,
            command_right=self.device.right,
            output_left=self.device.left,
            output_right=self.device.right,
        )

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while self.running:
            try:
                while True:
                    future, action = self.queue.get_nowait()
                    try:
                        future.set_result(action(self.device))
                    except BaseException as error:  # noqa: BLE001 - emulate native loop's future error transport.
                        future.set_exception(error)
            except Empty:
                pass
            cycle = FakeCycle(self)
            try:
                self.step(cycle)
                if self.device.state == keel.State.ARMED:
                    self.device.left, self.device.right = cycle.command
                else:
                    self.device.left = self.device.right = 0
                self.snapshot = self.make_snapshot(cycle.cycle_id)
                self.device.samples.append((time.monotonic(), self.device.left, self.device.right))
            except BaseException as error:  # noqa: BLE001 - expose controller failures to the task under test.
                self.error = error
                self.running = False
            time.sleep(0.001)

    def submit(self, action):
        future = Future()
        self.queue.put((future, action))
        return future

    def stop(self):
        self.device.disarm()
        self.running = False

    def join(self):
        if self.thread:
            self.thread.join(1)


@pytest.fixture
def hardware(monkeypatch):
    device = FakeDevice()
    monkeypatch.setattr(keel_task.device_api, "open", lambda **kwargs: device)
    monkeypatch.setattr(task_robot, "ControlLoop", FakeLoop)
    return device


class ActionTask(keel.KeelTask):
    def __init__(self, action):
        self.action = action

    def run(self):
        self.action(self.keel)


def test_task_scope_single_use_and_cleanup(hardware):
    kept = []
    task = ActionTask(lambda robot: kept.append(robot))
    with pytest.raises(keel.TaskError, match="only while"):
        _ = task.keel
    keel.run(task)
    assert hardware.closed
    assert hardware.arm_calls == 0
    with pytest.raises(keel.TaskError):
        _ = task.keel
    with pytest.raises(keel.TaskError) as closed:
        kept[0].drive_for(20, 20, 10)
    assert closed.value.reason == "CLOSED"
    with pytest.raises(RuntimeError, match="single-use"):
        keel.run(task)


def test_motion_percent_rounding_duration_stop_ack_and_one_arm(hardware):
    times = []

    def action(robot):
        robot.drive_for(12.25, -12.25, 30)
        times.append(time.monotonic())
        assert (hardware.left, hardware.right) == (0, 0)
        robot.turn_left_for(20)
        assert (hardware.left, hardware.right) == (0, 0)
        robot.turn_right_for(20)

    keel.run(ActionTask(action))
    driven = [s for s in hardware.samples if s[1] or s[2]]
    assert (123, -122) in [(left, right) for _, left, right in driven]
    assert (-200, 200) in [(left, right) for _, left, right in driven]
    assert (200, -200) in [(left, right) for _, left, right in driven]
    assert hardware.arm_calls == 1
    assert hardware.closed_with_output == (0, 0)
    first = [s[0] for s in driven if s[1:] == (123, -122)]
    assert times[0] - first[0] >= 0.03


def test_zero_duration_and_zero_duty_do_not_arm(hardware):
    def action(robot):
        robot.drive_for(100, 100, 0)
        robot.drive_for(0, 0, 10)

    keel.run(ActionTask(action))
    assert hardware.arm_calls == 0
    assert not any(left or right for _, left, right in hardware.samples)


@pytest.mark.parametrize("bad", [float("nan"), float("inf"), -101, 101])
def test_invalid_percent_rejected_before_arming(hardware, bad):
    with pytest.raises(ValueError, match="percentage"):
        keel.run(ActionTask(lambda robot: robot.drive_for(bad, 0, 10)))
    assert hardware.arm_calls == 0
    assert hardware.closed


@pytest.mark.parametrize("bad", [True, "20", None])
def test_non_numeric_percent_rejected(hardware, bad):
    with pytest.raises(TypeError):
        keel.run(ActionTask(lambda robot: robot.drive_for(bad, 0, 10)))
    assert hardware.arm_calls == 0


@pytest.mark.parametrize("duration", [-1, 10.5, True, 31_000])
def test_bad_duration_rejected_before_arming(hardware, duration):
    with pytest.raises((ValueError, TypeError)):
        keel.run(ActionTask(lambda robot: robot.drive_for(20, 20, duration)))
    assert hardware.arm_calls == 0


def test_user_exception_is_preserved_and_cleanup_runs(hardware):
    def action(robot):
        robot.drive_for(20, 20, 10)
        raise LookupError("student failure")

    with pytest.raises(LookupError, match="student failure"):
        keel.run(ActionTask(action))
    assert hardware.closed_with_output == (0, 0)


def test_stalled_student_code_loses_authority_at_total_deadline(hardware):
    observed = []

    def action(robot):
        # The budget must cover startup, arming and this first drive even on a slow CI scheduler;
        # the deadline has to expire during the stall below, not here.
        robot.drive_for(20, 20, 10)
        # Stall past the deadline without touching the robot, leaving the control thread time to tick.
        time.sleep(max(0, robot._deadline_ns - time.monotonic_ns()) / 1e9 + 0.2)
        observed.append(hardware.state)
        with pytest.raises(keel.TaskError) as later:
            robot.drive_for(20, 20, 10)
        assert later.value.reason == "TASK_TIMEOUT"

    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(action), keel.TaskOptions(task_timeout_ms=500))
    assert result.value.reason == "TASK_TIMEOUT"
    assert observed == [keel.State.DISARMED]
    assert hardware.arm_calls == 1


def test_control_thread_ends_motion_without_waiting_for_caller(hardware, monkeypatch):
    original_check = task_robot.TaskRobot._check
    slept = False

    def delayed_check(robot):
        nonlocal slept
        if robot._motion is not None and not slept:
            slept = True
            time.sleep(0.09)
            assert (hardware.left, hardware.right) == (0, 0)
        original_check(robot)

    monkeypatch.setattr(task_robot.TaskRobot, "_check", delayed_check)
    keel.run(ActionTask(lambda robot: robot.drive_for(20, 20, 20)))
    driven = [t for t, left, right in hardware.samples if left or right]
    assert driven[-1] - driven[0] < 0.05


def test_cancel_from_other_thread_is_terminal(hardware):
    threads = []

    def action(robot):
        stopper = threading.Thread(target=lambda: (time.sleep(0.02), robot.stop()))
        threads.append(stopper)
        stopper.start()
        with pytest.raises(keel.TaskError) as cancelled:
            robot.drive_for(20, 20, 1000)
        assert cancelled.value.reason == "CANCELLED"
        with pytest.raises(keel.TaskError):
            robot.drive_for(20, 20, 10)

    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(action))
    for thread in threads:
        thread.join()
    assert result.value.reason == "CANCELLED"
    assert hardware.arm_calls <= 1


def test_arming_timeout_is_bounded_and_stops(hardware):
    hardware.arm_allowed = False
    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(lambda robot: robot.drive_for(20, 20, 10)), keel.TaskOptions(arm_timeout_ms=20))
    assert result.value.reason == "ARM_TIMEOUT"
    assert hardware.closed_with_output == (0, 0)


def test_unexpected_disarm_does_not_auto_rearm(hardware):
    def action(robot):
        robot.drive_for(20, 20, 10)
        hardware.disarm()
        time.sleep(0.005)
        robot.drive_for(20, 20, 10)

    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(action))
    assert result.value.reason == "FAULT"
    assert hardware.arm_calls == 1


def test_unexpected_disarm_while_idle_is_terminal_without_another_action(hardware):
    def action(robot):
        robot.drive_for(20, 20, 10)
        hardware.disarm()
        time.sleep(0.02)

    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(action))
    assert result.value.reason == "FAULT"
    assert hardware.arm_calls == 1


def test_hardware_fault_stops_active_action(hardware):
    threads = []

    def action(robot):
        breaker = threading.Thread(target=lambda: (time.sleep(0.02), setattr(hardware, "state", keel.State.FAULTED)))
        threads.append(breaker)
        breaker.start()
        robot.drive_for(20, 20, 1000)

    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(action))
    for thread in threads:
        thread.join()
    assert result.value.reason == "FAULT"
    assert hardware.closed_with_output == (0, 0)


def test_task_options_finite_limits_and_virtual_clock():
    for name in ("max_motion_ms", "arm_timeout_ms", "task_timeout_ms", "camera_timeout_ms"):
        for value in (0, -1, 86_400_001, float("inf"), True):
            with pytest.raises((TypeError, ValueError)):
                keel.TaskOptions(**{name: value})
    with pytest.raises(ValueError, match="VIRTUAL_CLOCK"):
        keel.TaskOptions(board_kwargs={"flags": keel.OpenFlag.VIRTUAL_CLOCK})
    options_dict = {"backend": keel.Backend.SIMULATOR}
    options = keel.TaskOptions(board_kwargs=options_dict)
    options_dict.clear()
    assert options.board_kwargs["backend"] == keel.Backend.SIMULATOR
    with pytest.raises(TypeError):
        options.board_kwargs["backend"] = 0


def test_camera_is_lazy_owned_and_closed(hardware, monkeypatch):
    events = []
    image = object()

    class FakeCamera:
        def start(self):
            events.append(("start", threading.get_ident()))

        def read_frame(self, timeout):
            events.append(("read", threading.get_ident(), timeout))
            return image

        def close(self):
            events.append(("close", threading.get_ident()))

    def open_camera(**options):
        events.append(("open", threading.get_ident(), options))
        return FakeCamera()

    monkeypatch.setattr(task_robot.camera_api, "open", open_camera)

    def action(robot):
        assert events == []
        assert robot.take_image() is image
        assert robot.take_image() is image

    keel.run(ActionTask(action), keel.TaskOptions(camera_timeout_ms=100))
    assert [e[0] for e in events] == ["open", "start", "read", "read", "close"]
    assert all(e[1] == threading.get_ident() for e in events)
    assert events[0][2]["pixel_format"] == keel.camera.PixelFormat.BGR888
    assert 0 < events[2][2] <= 0.1
    assert hardware.arm_calls == 0


def test_camera_failure_is_terminal_even_when_caught(hardware, monkeypatch):
    def fail(**options):
        raise OSError("camera missing")

    monkeypatch.setattr(task_robot.camera_api, "open", fail)

    def action(robot):
        with pytest.raises(keel.TaskError) as camera:
            robot.take_image()
        assert camera.value.reason == "CAMERA_FAILED"
        with pytest.raises(keel.TaskError):
            robot.drive_for(20, 20, 10)

    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(action))
    assert result.value.reason == "CAMERA_FAILED"
    assert hardware.arm_calls == 0


def test_delayed_control_thread_start_is_not_a_failure(hardware, monkeypatch):
    class DelayedLoop(FakeLoop):
        def start(self):
            def delayed():
                time.sleep(0.03)
                super(DelayedLoop, self).start()

            threading.Thread(target=delayed, daemon=True).start()

    monkeypatch.setattr(task_robot, "ControlLoop", DelayedLoop)
    began = time.monotonic()
    keel.run(ActionTask(lambda robot: robot.drive_for(20, 20, 10)))
    assert time.monotonic() - began >= 0.03
    assert hardware.arm_calls == 1


def test_control_loop_that_never_starts_is_bounded(hardware, monkeypatch):
    class NeverStarts(FakeLoop):
        def start(self):
            pass

    monkeypatch.setattr(task_robot, "ControlLoop", NeverStarts)
    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(lambda robot: None), keel.TaskOptions(arm_timeout_ms=20))
    assert result.value.reason == "LOOP_FAILED"
    assert hardware.closed


def test_constructor_failure_is_not_masked_by_cleanup(hardware, monkeypatch):
    def fail_construction(*args, **kwargs):
        raise LookupError("construction failed")

    def fail_close():
        raise OSError("cleanup failed")

    monkeypatch.setattr(keel_task, "TaskRobot", fail_construction)
    monkeypatch.setattr(hardware, "close", fail_close)
    with pytest.raises(LookupError, match="construction failed") as failure:
        keel.run(ActionTask(lambda robot: None))
    assert "cleanup failed" in failure.value.__notes__[0]


def test_run_validates_inputs_without_opening_hardware(hardware):
    with pytest.raises(TypeError, match="KeelTask"):
        keel.run(object())
    with pytest.raises(TypeError, match="TaskOptions"):
        keel.run(ActionTask(lambda robot: None), object())


def test_wrong_thread_action_is_refused(hardware):
    errors = []

    def action(robot):
        def other_thread():
            try:
                robot.drive_for(20, 20, 10)
            except RuntimeError as error:
                errors.append(error)

        thread = threading.Thread(target=other_thread)
        thread.start()
        thread.join()

    keel.run(ActionTask(action))
    assert "task thread" in str(errors[0])
    assert hardware.arm_calls == 0


def test_camera_other_pixel_format_is_refused():
    with pytest.raises(ValueError, match="BGR888"):
        keel.TaskOptions(camera_kwargs={"pixel_format": keel.camera.PixelFormat.I420})


def test_cancelled_queued_arming_cannot_restore_authority(hardware, monkeypatch):
    queued = threading.Event()
    pending = []
    errors = []

    class PausedQueue(FakeLoop):
        def submit(self, action):
            future = Future()
            pending.append((future, action))
            queued.set()
            return future

    monkeypatch.setattr(task_robot, "ControlLoop", PausedQueue)

    def action(robot):
        def cancel_then_release():
            assert queued.wait(1)
            robot.stop()
            future, callback = pending[0]
            try:
                callback(hardware)
            except keel.TaskError as error:
                errors.append(error)
                future.set_exception(error)

        stopper = threading.Thread(target=cancel_then_release)
        stopper.start()
        try:
            robot.drive_for(20, 20, 100)
        finally:
            stopper.join()

    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(action))
    assert result.value.reason == "CANCELLED"
    assert errors[0].reason == "CANCELLED"
    assert hardware.arm_calls == 0


def test_stop_reports_unconfirmed_native_stop_failure(hardware, monkeypatch):
    def fail_stop():
        raise OSError("unconfirmed stop")

    monkeypatch.setattr(hardware, "disarm", fail_stop)
    with pytest.raises(OSError, match="unconfirmed stop"):
        keel.run(ActionTask(lambda robot: robot.stop()))


def test_loop_failure_between_actions_revokes_later_motion(hardware):
    def action(robot):
        robot._loop.error = RuntimeError("control failed")
        robot.drive_for(20, 20, 10)

    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(action))
    assert result.value.reason == "LOOP_FAILED"
    assert hardware.arm_calls == 0


def test_camera_result_after_deadline_cannot_resume_task(hardware, monkeypatch):
    closed = []

    class DelayedCamera:
        def start(self):
            pass

        def read_frame(self, timeout):
            time.sleep(0.06)
            return object()

        def close(self):
            closed.append(True)

    monkeypatch.setattr(task_robot.camera_api, "open", lambda **options: DelayedCamera())
    with pytest.raises(keel.TaskError) as result:
        keel.run(ActionTask(lambda robot: robot.take_image()), keel.TaskOptions(task_timeout_ms=30))
    assert result.value.reason == "TASK_TIMEOUT"
    assert closed == [True]
    assert hardware.arm_calls == 0
