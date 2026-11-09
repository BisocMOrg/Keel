# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Robot stage: the binding against the keel_r3 board.

Run on the robot only, with the libkeel Debian packages installed, KEEL_LIBRARY unset (the installed libkeel.so.1 is
loaded by its SONAME) and KEEL_TEST_HARDWARE=1:

    KEEL_TEST_HARDWARE=1 python3 -m pytest -m hardware -p no:cacheprovider

Nothing here moves the robot: no test calls request_arm, and the motion guard of support/native_calls.py fails a test
before any call that could move it, including the control loop's own calls. The LED tests need an operator to
confirm the colours.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from collections.abc import Iterator

import pytest

import keel
from support.environment import backends, subprocess_env

pytestmark = [
    pytest.mark.hardware,
    pytest.mark.skipif(os.environ.get("KEEL_TEST_HARDWARE") != "1", reason="robot only (set KEEL_TEST_HARDWARE=1)"),
]


@pytest.fixture
def board() -> Iterator[keel.Device]:
    if os.environ.get("KEEL_LIBRARY") or os.environ.get("KEEL_BACKEND"):
        pytest.fail("unset KEEL_LIBRARY and KEEL_BACKEND: the robot stage tests the installed library on the board")
    device = keel.open()
    yield device
    device.close()


def test_open_and_the_report(board: keel.Device) -> None:
    report = board.report
    assert report.backend == keel.Backend.R3
    assert report.warnings == keel.OpenWarning(0), report.hint
    assert report.timing_class == keel.TimingClass.REALTIME
    assert "r3" in backends(keel.version_string())


def test_a_second_open_is_in_use(board: keel.Device) -> None:
    with pytest.raises(keel.SetupError) as caught:
        keel.open(backend=keel.Backend.R3)
    error = caught.value
    assert error.status == keel.Status.E_IN_USE
    assert error.report.line >= 0
    assert error.report.consumer


def test_capabilities(board: keel.Device) -> None:
    capabilities = board.capabilities()
    assert keel.Capability.SERVO not in capabilities
    assert keel.Capability.SIMULATED not in capabilities
    assert keel.Capability.REALTIME_PWM in capabilities
    assert keel.Capability.MOTORS in capabilities


def test_best_effort_timing_is_reported_and_logged() -> None:
    code = (
        "import logging, keel; logging.basicConfig(level=logging.WARNING); "
        "device = keel.open(); print(device.report.warnings.name); device.close()"
    )
    result = subprocess.run(
        ["prlimit", "--rtprio=0", "--", sys.executable, "-c", code],
        capture_output=True,
        text=True,
        env=subprocess_env(),
        timeout=60,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert "BEST_EFFORT_TIMING" in result.stdout
    assert "opened degraded (BEST_EFFORT_TIMING)" in result.stderr


def test_leds_and_button_leds(board: keel.Device) -> None:
    for color, name in ((0x400000, "red"), (0x004000, "green"), (0x000040, "blue")):
        board.set_rgb_all(color)
        print(f"operator: all RGB LEDs should be {name}")
        time.sleep(1.0)
    board.set_rgb_frame([0x400000, 0x402000, 0x404000, 0x004000, 0x000040, 0x200040])
    board.set_button_leds(keel.Button.ALL, keel.Button.ALL)
    print("operator: the RGB LEDs show a rainbow and every button LED is lit")
    time.sleep(1.0)
    board.set_rgb_all(0)
    board.set_button_leds(keel.Button.ALL, 0)


def test_take_buttons(board: keel.Device) -> None:
    buttons = board.take_buttons()
    assert buttons.held == keel.Button(0), "release every button before the test"


def test_a_disarmed_loop_qualifies_the_heartbeat_and_recovers_from_an_emergency_stop(board: keel.Device) -> None:
    faults: list[keel.Fault] = []
    loop = keel.ControlLoop(board, on_fault=faults.append)
    loop.start()
    try:
        time.sleep(3.0)
        snapshot = loop.snapshot
        assert snapshot.state == keel.State.DISARMED
        assert keel.SnapshotFlag.HEARTBEAT_RUNNING in snapshot.flags
        assert keel.SafetyInput.READY in board.read_safety_inputs()
        assert snapshot.sensor_power in (keel.SensorPower.PERMITTED, keel.SensorPower.CONFIRMED)

        board.emergency_stop()
        time.sleep(0.1)
        assert [fault.cause for fault in faults] == [keel.FaultCause.USER_STOP]
        loop.clear_fault().result(timeout=5)
        time.sleep(0.5)
        snapshot = loop.snapshot
        assert snapshot.state == keel.State.DISARMED
        assert keel.SnapshotFlag.HEARTBEAT_RUNNING in snapshot.flags
    finally:
        loop.stop()
        loop.join()
