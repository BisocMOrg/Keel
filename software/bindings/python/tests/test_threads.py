# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Threads: libkeel's thread rule through the binding, stops from other threads, close races and the GIL."""

from __future__ import annotations

import sys
import threading
import time
from collections.abc import Callable
from typing import Any

import pytest

import keel
from support.simulator import CycleDriver, open_sim


def in_thread(function: Callable[[], Any]) -> Any:
    """Run function on a new thread; return its result or the exception it raised."""
    outcome: list[Any] = []

    def target() -> None:
        try:
            outcome.append(function())
        except BaseException as error:  # noqa: BLE001  (the test inspects whatever the call raised)
            outcome.append(error)

    thread = threading.Thread(target=target)
    thread.start()
    thread.join()
    return outcome[0]


def wait_for(condition: Callable[[], bool], timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while not condition():
        if time.monotonic() > deadline:
            raise AssertionError("condition not met in time")
        time.sleep(0.005)


def test_go_calls_from_another_thread_conflict_and_stops_pass(sim: keel.Device) -> None:
    CycleDriver(sim).arm()
    assert isinstance(in_thread(lambda: sim.set_motors(100, 100)), keel.ConflictError)
    assert isinstance(in_thread(sim.request_arm), keel.ConflictError)
    assert isinstance(in_thread(sim.brake), keel.ConflictError)
    assert in_thread(lambda: sim.set_motors(0, 0)) is None
    assert in_thread(sim.coast) is None
    assert sim.state() == keel.State.ARMED
    assert in_thread(sim.disarm) is None
    assert sim.state() == keel.State.DISARMED


def test_a_tick_from_another_thread_fail_stops(sim: keel.Device) -> None:
    sim.tick(1, keel.Health.ALL)
    error = in_thread(lambda: sim.tick(2, keel.Health.ALL))
    assert isinstance(error, keel.SafetyFaultError)
    assert error.status == keel.Status.E_HEALTH
    assert error.fault is not None
    assert error.fault.cause == keel.FaultCause.TICK_THREAD_CHANGED
    assert isinstance(in_thread(sim.clear_fault), keel.ConflictError)
    sim.clear_fault()


def test_emergency_stop_from_another_thread_while_the_loop_runs() -> None:
    with open_sim(virtual=False) as device:
        faults: list[keel.Fault] = []
        loop = keel.ControlLoop(device, on_fault=faults.append)
        with loop:
            future = loop.request_arm()
            wait_for(lambda: loop.snapshot.state == keel.State.ARMED)
            assert future.result() is None
            device.emergency_stop()
            wait_for(lambda: loop.fault is not None)
            assert loop.running
            assert loop.fault is not None
            assert loop.fault.cause == keel.FaultCause.USER_STOP
            assert faults == [loop.fault]
            wait_for(lambda: loop.snapshot.state == keel.State.FAULTED)


def test_close_racing_calls_in_flight_never_crashes() -> None:
    device = open_sim(virtual=False)
    errors: list[BaseException] = []
    started = threading.Barrier(5)

    def hammer(call: Callable[[], object]) -> None:
        started.wait()
        try:
            while True:
                call()
        except keel.HandleError as error:
            errors.append(error)
        except BaseException as error:  # noqa: BLE001  (any other exception fails the test below)
            errors.append(error)

    calls: list[Callable[[], object]] = [
        device.snapshot,
        device.state,
        lambda: device.set_rgb_all(0x010203),
        lambda: device.sleep_until(device.now_us() + 2000),
    ]
    threads = [threading.Thread(target=hammer, args=(call,)) for call in calls]
    for thread in threads:
        thread.start()
    started.wait()
    time.sleep(0.05)
    device.close()
    for thread in threads:
        thread.join(timeout=5)
    assert len(errors) == len(calls)
    assert all(isinstance(error, keel.HandleError) for error in errors), errors
    assert all(error.status == keel.Status.E_INVALID_HANDLE for error in errors)


def test_sleep_until_releases_the_gil() -> None:
    with open_sim(virtual=False) as device:
        counter = [0]
        running = threading.Event()
        done = threading.Event()

        def count() -> None:
            running.set()
            while not done.is_set():
                counter[0] += 1

        thread = threading.Thread(target=count)
        thread.start()
        running.wait()
        before = counter[0]
        device.sleep_until(device.now_us() + 50_000)
        progressed = counter[0] - before
        done.set()
        thread.join()
        assert progressed > 1000


def test_the_switch_interval_is_reference_counted_and_restored() -> None:
    original = sys.getswitchinterval()
    with open_sim(virtual=False) as first, open_sim(virtual=False) as second:
        loop_a = keel.ControlLoop(first)
        loop_b = keel.ControlLoop(second)
        loop_a.start()
        loop_b.start()
        wait_for(lambda: loop_a.running and loop_b.running)
        assert sys.getswitchinterval() == pytest.approx(0.001)
        loop_a.stop()
        loop_a.join()
        assert sys.getswitchinterval() == pytest.approx(0.001)
        loop_b.stop()
        loop_b.join()
    assert sys.getswitchinterval() == original
