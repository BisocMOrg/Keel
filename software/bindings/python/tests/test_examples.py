# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Examples: every example program against the real-time simulator, the README quick start and the motion checks.

Each example runs in a child process with --simulator. The motion check of each example is also called directly,
with simulated and real capabilities, because the suite has no robot to reach the real-board branch.
"""

from __future__ import annotations

import importlib.util
import re
import signal
import subprocess
import sys
import time
from types import ModuleType

import pytest

import keel
from support.environment import PYTHON, backends, subprocess_env
from support.native_calls import NativeCallController

EXAMPLES = PYTHON / "examples"
# SIGHUP and SIGQUIT do not exist on Windows, where this module is still collected for the portability tests.
POSIX_ONLY_SIGNALS = [getattr(signal, name) for name in ("SIGHUP", "SIGQUIT") if hasattr(signal, name)]
README = PYTHON / "README.md"
REAL = keel.Capability.MOTORS | keel.Capability.BRAKE | keel.Capability.RGB | keel.Capability.REALTIME_PWM
SIMULATED = REAL | keel.Capability.SIMULATED


def run(name: str, *arguments: str, timeout: float = 60) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-W", "error", str(EXAMPLES / f"{name}.py"), *arguments],
        capture_output=True,
        text=True,
        env=subprocess_env(),
        timeout=timeout,
        check=False,
    )


def example(name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(f"keel_example_{name}", EXAMPLES / f"{name}.py")
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses resolve their annotations through sys.modules
    try:
        spec.loader.exec_module(module)
    finally:
        del sys.modules[spec.name]
    return module


@pytest.mark.parametrize(
    ("name", "arguments", "expected"),
    [
        (
            "drive",
            [],
            ["arming", "armed; driving forward at 200 permille", r"disarmed after \d+ cycles \(state DISARMED\)"],
        ),
        ("buttons", ["--duration", "300"], ["buttons: done"]),
        ("buttons", ["--drive", "200", "-200", "300"], ["state: ARMING", "state: ARMED", "state: DISARMED"]),
        (
            "leds",
            ["--hold", "50"],
            [
                "rgb: rainbow frame 400000 402000 404000 004000 000040 200040",
                "fault: E_FAULTED, cause USER_STOP; showing the error colour",
                "leds: done",
            ],
        ),
        ("ranging", ["--count", "3"], [r"loop: 500 mm \(echo \d+ us, sequence \d+\)", r"thread: 500 mm"]),
        (
            "safety",
            [],
            [
                "foreign go call refused: KEEL_E_CONFLICT",
                "fault: E_FAULTED, cause USER_STOP",
                "fault cleared: state DISARMED",
                "re-arm refused during the 1000 ms hold-off: KEEL_E_STATE",
            ],
        ),
    ],
)
def test_example_on_the_simulator(name: str, arguments: list[str], expected: list[str]) -> None:
    result = run(name, "--simulator", *arguments)
    assert result.returncode == 0, result.stdout + result.stderr
    for line in expected:
        assert re.search(line, result.stdout), f"{line!r} not in:\n{result.stdout}"
    assert result.stderr == ""


@pytest.mark.parametrize("name", ["drive", "safety"])
def test_motion_checks(name: str) -> None:
    motion_allowed = example(name).motion_allowed
    assert motion_allowed(SIMULATED, False)
    assert motion_allowed(SIMULATED, True)
    assert motion_allowed(REAL, True)
    assert not motion_allowed(REAL, False)


def test_buttons_without_drive_runs_on_a_robot_and_never_arms(
    natives: NativeCallController, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """Without --drive the buttons example only mirrors the buttons, so it needs no motion permission on a robot."""
    monkeypatch.setattr(keel.Device, "capabilities", lambda self: REAL)
    with natives.record() as calls:
        status = example("buttons").main(["--simulator", "--duration", "200"])
    assert status == 0
    assert "buttons: done" in capsys.readouterr().out
    assert "keel_request_arm" not in calls


@pytest.mark.parametrize(
    ("name", "arguments"),
    [
        ("buttons", ["--drive", "2000", "0", "100"]),
        ("buttons", ["--duration", "0"]),
        ("leds", ["--hold", "1"]),
        ("ranging", ["--count", "0"]),
        ("drive", ["--unknown"]),
    ],
)
def test_usage_errors_exit_with_status_2(name: str, arguments: list[str]) -> None:
    result = run(name, "--simulator", *arguments)
    assert result.returncode == 2
    assert "usage:" in result.stderr


@pytest.mark.parametrize(
    ("signum", "status"),
    [(signal.SIGINT, 130), (signal.SIGTERM, 128 + signal.SIGTERM)]
    + [(number, 128 + number) for number in POSIX_ONLY_SIGNALS[:1]],
)
def test_a_signal_is_an_emergency_stop(signum: int, status: int) -> None:
    process = subprocess.Popen(
        [sys.executable, "-W", "error", str(EXAMPLES / "buttons.py"), "--simulator"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=subprocess_env(),
    )
    time.sleep(1.0)
    process.send_signal(signum)
    stdout, stderr = process.communicate(timeout=30)
    assert process.returncode == status, stdout + stderr
    if signum == signal.SIGINT:
        assert "emergency stop" in stdout


def quick_start() -> str:
    """The first Python block of the README's quick start."""
    text = README.read_text(encoding="utf-8")
    section = text[text.index("## Quick start") :]
    match = re.search(r"```python\n(.*?)```", section, re.S)
    assert match is not None
    return match.group(1)


def test_the_readme_quick_start(natives: object) -> None:
    """The quick start opens with AUTO, so it runs only against a library that has no keel_r3 backend at all."""
    if backends(keel.version_string()) != ["simulator"]:
        pytest.fail("the quick-start test needs a simulator-only libkeel (cmake --preset sim-only)")
    result = subprocess.run(
        [sys.executable, "-W", "error", "-c", quick_start()],
        capture_output=True,
        text=True,
        env=subprocess_env(KEEL_BACKEND="simulator"),
        timeout=60,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
