# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Conformance: replay software/libkeel/tests/bindings/scenarios.json through the public API.

libkeel's own suite replays the same file through its reference ctypes binding, which proves every expectation
against the C library; this replay maps each C function name to the binding's API, which proves the mapping
complete. Scenarios must name the simulator.
"""

from __future__ import annotations

import enum
import json
import threading
from collections.abc import Callable
from typing import Any

import pytest

import keel
from support.environment import libkeel_source
from support.simulator import PERIOD_US

SOURCE = libkeel_source()
SCENARIOS = SOURCE / "tests" / "bindings" / "scenarios.json" if SOURCE else None
pytestmark = pytest.mark.skipif(
    SCENARIOS is None or not SCENARIOS.is_file(), reason="needs the libkeel source tree (set KEEL_SOURCE_DIR)"
)

# Every enum type of the binding, to resolve enumerator names to members.
PREFIXES: dict[str, type[enum.Enum]] = {
    "KEEL_CLASS_": keel.StatusClass,
    "KEEL_STATE_": keel.State,
    "KEEL_ACK_": keel.AckPhase,
    "KEEL_HEALTH_": keel.Health,
    "KEEL_BUTTON_": keel.Button,
    "KEEL_LED_": keel.Led,
    "KEEL_INPUT_": keel.SafetyInput,
    "KEEL_CAP_": keel.Capability,
    "KEEL_BACKEND_": keel.Backend,
    "KEEL_OPEN_WARN_": keel.OpenWarning,
    "KEEL_OPEN_": keel.OpenFlag,
    "KEEL_RESOURCE_": keel.OpenResource,
    "KEEL_CAUSE_": keel.FaultCause,
    "KEEL_FAULT_": keel.FaultFlag,
    "KEEL_SNAP_": keel.SnapshotFlag,
    "KEEL_SENSOR_": keel.SensorPower,
    "KEEL_TIMING_": keel.TimingClass,
    "KEEL_RANGE_": keel.RangeStatus,
    "KEEL_CYCLE_": keel.CycleCommand,
    "KEEL_SIM_": keel.SimItem,
}
FLAG_TYPES: dict[str, type[enum.Enum]] = {"KEEL_CYCLE_": keel.CycleFlag, "KEEL_SIM_": keel.SimFlag}


def resolve(name: str) -> int:
    """The value of an enumerator name, such as "KEEL_E_CONFLICT" or "KEEL_ABI_VERSION"."""
    if name.startswith("KEEL_ABI_"):
        return int(getattr(keel.abi, name[len("KEEL_ABI_") :]))
    for prefix, cls in [*PREFIXES.items(), *FLAG_TYPES.items()]:
        if name.startswith(prefix) and name[len(prefix) :] in cls.__members__:
            return int(cls.__members__[name[len(prefix) :]].value)
    if name.startswith("KEEL_") and name[len("KEEL_") :] in keel.Status.__members__:
        return int(keel.Status[name[len("KEEL_") :]])
    if hasattr(keel.limits, name[len("KEEL_") :]):
        return int(getattr(keel.limits, name[len("KEEL_") :]))
    raise AssertionError(f"unknown enumerator {name}")


def packed(version: tuple[int, int]) -> int:
    return (version[0] << 16) | version[1]


def cycle(device: keel.Device, a: list[Any]) -> keel.Snapshot:
    health, cycle_id, command, left, right, flags = a
    take = bool(flags & keel.CycleFlag.TAKE_BUTTONS)
    return device.cycle(cycle_id, health, command=command, left=left, right=right, take_buttons=take)


# The C function -> the binding's API. Every function of keel.h must appear here (checked below).
CALLS: dict[str, Callable[[keel.Device, list[Any]], Any]] = {
    "keel_abi_version": lambda d, a: packed(keel.abi_version()),
    "keel_version_string": lambda d, a: keel.version_string(),
    "keel_status_name": lambda d, a: keel.status_name(*a),
    "keel_status_string": lambda d, a: keel.status_string(*a),
    "keel_status_class": lambda d, a: keel.status_class(*a),
    "keel_fault_cause_name": lambda d, a: keel.fault_cause_name(*a),
    "keel_open": lambda d, a: pytest.fail("keel_open is the scenario's 'open'"),
    "keel_close": lambda d, a: d.close(),
    "keel_get_capabilities": lambda d, a: d.capabilities(),
    "keel_now_us": lambda d, a: d.now_us(),
    "keel_sleep_until": lambda d, a: d.sleep_until(*a),
    "keel_tick": lambda d, a: d.tick(*a),
    "keel_cycle": cycle,
    "keel_request_arm": lambda d, a: d.request_arm(),
    "keel_disarm": lambda d, a: d.disarm(),
    "keel_emergency_stop": lambda d, a: d.emergency_stop(),
    "keel_clear_fault": lambda d, a: d.clear_fault(),
    "keel_get_state": lambda d, a: d.state(),
    "keel_get_snapshot": lambda d, a: d.snapshot(),
    "keel_get_fault": lambda d, a: d.fault(),
    "keel_read_safety_inputs": lambda d, a: d.read_safety_inputs(),
    "keel_set_motors": lambda d, a: d.set_motors(*a),
    "keel_brake": lambda d, a: d.brake(),
    "keel_servo_start": lambda d, a: d.servo_start(*a),
    "keel_servo_set_pulse": lambda d, a: d.servo_set_pulse(*a),
    "keel_servo_end": lambda d, a: d.servo_end(),
    "keel_range_start": lambda d, a: d.range_start(),
    "keel_range_poll": lambda d, a: d.range_poll(),
    "keel_range_measure": lambda d, a: d.range_measure(),
    "keel_set_rgb": lambda d, a: d.set_rgb(*a),
    "keel_set_rgb_all": lambda d, a: d.set_rgb_all(*a),
    "keel_set_rgb_frame": lambda d, a: d.set_rgb_frame(*a),
    "keel_get_rgb": lambda d, a: d.rgb(*a),
    "keel_set_button_leds": lambda d, a: d.set_button_leds(*a),
    "keel_take_buttons": lambda d, a: d.take_buttons(),
    "keel_sim_set": lambda d, a: d.sim_set(*a),
    "keel_sim_get_state": lambda d, a: d.sim_state(),
}


def load() -> dict[str, Any]:
    assert SCENARIOS is not None
    with SCENARIOS.open(encoding="utf-8") as handle:
        data: dict[str, Any] = json.load(handle)
    return data


def scenario_names() -> list[str]:
    if SCENARIOS is None or not SCENARIOS.is_file():
        return []
    return [scenario["name"] for scenario in load()["scenarios"]]


class Replay:
    """Replays one scenario on one device; the calling thread is the owner thread."""

    def __init__(self, device: keel.Device) -> None:
        self.device = device
        self.cycle_id = device.snapshot().last_cycle_id
        self.next_us = device.now_us()
        self.status = keel.Status.OK
        self.output: Any = None
        self.where = ""

    def argument(self, value: Any) -> Any:
        if isinstance(value, str):
            return resolve(value)
        if isinstance(value, list):
            return [self.argument(item) for item in value]
        if isinstance(value, dict) and value == {"cycle_id": "next"}:
            self.cycle_id += 1
            return self.cycle_id
        if isinstance(value, dict) and "now_plus_us" in value:
            return self.device.now_us() + value["now_plus_us"]
        assert isinstance(value, int), value
        return value

    def call(self, name: str, args: list[Any], thread: str) -> None:
        arguments = [self.argument(value) for value in args]
        outcome: list[Any] = []

        def run() -> None:
            try:
                outcome.append((keel.Status.OK, CALLS[name](self.device, arguments)))
            except keel.KeelError as error:
                outcome.append((error.status, None))

        if thread == "other":
            worker = threading.Thread(target=run)
            worker.start()
            worker.join()
        else:
            assert thread == "owner", thread
            run()
        self.status, self.output = outcome[0]
        if name == "keel_close":
            assert self.device.close_status is not None
            self.status = self.device.close_status

    def cycles(self, count: int, command: int, left: int, right: int) -> None:
        self.status = keel.Status.OK
        for _ in range(count):
            self.cycle_id += 1
            try:
                self.device.cycle(self.cycle_id, keel.Health.ALL, command=command, left=left, right=right)
            except keel.KeelError as error:
                if self.status == keel.Status.OK:
                    self.status = error.status
            self.next_us = max(self.next_us + PERIOD_US, self.device.now_us())
            self.device.sleep_until(self.next_us)
        self.output = None

    def expect(self, step: dict[str, Any]) -> None:
        assert self.status == resolve(step["expect"]), f"{self.where}: status {self.status.name}"
        if "output" in step:
            check(self.output, step["output"], self.where + " output")
        if "state" in step:
            assert self.device.state() == resolve(step["state"]), f"{self.where}: state"
        if "fault" in step:
            fault = self.device.fault()
            expected = step["fault"]
            assert fault.cause == resolve(expected["cause"]), f"{self.where}: fault cause {fault.cause.name}"
            assert fault.status == resolve(expected["status"]), f"{self.where}: fault status"
            flags = sum(resolve(name) for name in expected["flags"])
            assert fault.flags == flags, f"{self.where}: fault flags {fault.flags!r}"
        if "snapshot" in step:
            check(self.device.snapshot(), step["snapshot"], self.where + " snapshot")


def check(actual: Any, expected: Any, where: str) -> None:
    """Compare a result with an expected value: a scalar, a set of named bits, a string or struct fields."""
    if isinstance(expected, dict):
        for field, value in expected.items():
            check(getattr(actual, field), value, f"{where}.{field}")
    elif isinstance(expected, list):
        bits = sum(resolve(name) for name in expected)
        assert int(actual) & bits == bits, f"{where}: {actual!r} lacks {expected}"
    elif isinstance(expected, str) and isinstance(actual, str):
        assert actual == expected, f"{where}: {actual!r}"
    elif isinstance(expected, str):
        assert int(actual) == resolve(expected), f"{where}: {actual!r} is not {expected}"
    else:
        assert int(actual) == expected, f"{where}: {actual!r} is not {expected}"


def test_the_file_format() -> None:
    data = load()
    assert data["license"] == "SPDX-License-Identifier: MIT"
    assert data["copyright"] == "Copyright (c) 2026 BisocM"
    assert data["format"] == 1


def test_every_function_is_mapped_and_covered() -> None:
    assert SOURCE is not None
    text = (SOURCE / "tests" / "abi" / "symbols.txt").read_text(encoding="utf-8")
    symbols = [line.strip() for line in text.splitlines() if line.strip() and not line.startswith("#")]
    assert list(CALLS) == symbols
    used = {"keel_open"}
    for scenario in load()["scenarios"]:
        for step in scenario["steps"]:
            used.add(step.get("call", "keel_sim_set" if "sim" in step else ""))
    assert set(symbols) <= used


@pytest.mark.parametrize("name", scenario_names())
def test_scenario(name: str) -> None:
    scenario = next(item for item in load()["scenarios"] if item["name"] == name)
    opening = scenario["open"]
    if opening.get("backend") != "KEEL_BACKEND_SIMULATOR":
        pytest.fail(f"scenario {name} must open KEEL_BACKEND_SIMULATOR")
    flags = sum(resolve(flag) for flag in opening.get("flags", []))
    device = keel.open(backend=keel.Backend.SIMULATOR, flags=flags)
    try:
        replay = Replay(device)
        for number, step in enumerate(scenario["steps"], 1):
            replay.where = f"{name} step {number}"
            if "call" in step:
                replay.call(step["call"], step.get("args", []), step.get("thread", "owner"))
            elif "cycles" in step:
                command = resolve(step["command"]) if "command" in step else keel.CycleCommand.NONE
                replay.cycles(step["cycles"], command, step.get("left", 0), step.get("right", 0))
            elif "sim" in step:
                replay.call("keel_sim_set", [step["sim"], step["value"]], "owner")
            elif "expect" in step:
                replay.expect(step)
            else:
                pytest.fail(f"{replay.where}: unknown step {step}")
    finally:
        device.close()
