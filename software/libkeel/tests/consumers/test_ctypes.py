#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Python ctypes consumer of libkeel: the ABI smoke test against the simulator.

Cases (run one by name, or "all"):
  metadata    ABI version, bounded static strings, the class of every status
              and the exception each class maps to
  layout      ctypes structs against tests/abi/layout.c, prototypes against
              symbols.txt and the preprocessed keel.h
  loop        open the simulator, arm on the virtual clock, cycle, drive, the
              thread rule (stop from anywhere, go only from the loop), close
  faults      health fault, FAULTED refusals, clear, re-arm hold-off,
              emergency stop
  peripherals LED I2C failure and ultrasonic ranging
  handles     setup errors at open, closed handles, close never raising
  scenarios   replay of tests/bindings/scenarios.json, the language-neutral
              conformance scenarios every binding replays (SPEC 21)

Environment: KEEL_LIBRARY (absolute path of libkeel.so.1; otherwise the
SONAME is loaded), KEEL_HEADER (keel.h to generate constants from; default
the source tree's), CC (compiler for the preprocessor, default cc).

SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
"""

import ctypes
import json
import os
import re
import sys
import threading
import unittest

# Sibling modules are imported from the source tree; leave no __pycache__ there.
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests", "docs"))
import check_header_docs  # noqa: E402  (status classes as keel.h documents them)
import keel_ctypes as kc  # noqa: E402

HEADER = os.environ.get("KEEL_HEADER") or os.path.join(ROOT, "include", "keel", "keel.h")
KEEL = kc.Keel(HEADER, cc=os.environ.get("CC", "cc"))
K = KEEL.K
LIB = KEEL.lib


def open_sim(flags=0):
    return KEEL.open(backend=K.KEEL_BACKEND_SIMULATOR, flags=K.KEEL_OPEN_VIRTUAL_CLOCK | flags)


def idle(snapshot):
    return K.KEEL_CYCLE_NONE, 0, 0


def arm(device, loop):
    device.call("request_arm")
    for _ in range(200):
        if loop.run(1, idle).state == K.KEEL_STATE_ARMED:
            return
    raise AssertionError("the simulator did not arm within 2 s")


class Metadata(unittest.TestCase):
    def test_abi_and_strings(self):
        self.assertEqual(LIB.keel_abi_version(), K.KEEL_ABI_VERSION)
        version = kc.static_string(LIB.keel_version_string())
        self.assertTrue(version.startswith("1.0.0 ("), version)
        self.assertIn("simulator", version)
        self.assertEqual(kc.static_string(LIB.keel_status_name(-99)), "KEEL_E_UNKNOWN")
        self.assertEqual(kc.static_string(LIB.keel_fault_cause_name(9999)), "UNKNOWN")
        for name in dir(K):
            if name.startswith("KEEL_CAUSE_"):
                cause = kc.static_string(LIB.keel_fault_cause_name(getattr(K, name)))
                self.assertEqual(cause, name[len("KEEL_CAUSE_"):])

    def test_status_classes_and_exceptions(self):
        tags = check_header_docs.status_classes(check_header_docs.Header(HEADER))
        expected_type = {"REJECTED": kc.CommandRejected, "FAULT": kc.SafetyFault,
                         "PERIPHERAL": kc.PeripheralError, "SETUP": kc.KeelError,
                         "HANDLE": kc.ClosedError}
        for name, tag in tags.items():
            status = getattr(K, name)
            self.assertEqual(kc.static_string(LIB.keel_status_name(status)), name)
            self.assertTrue(kc.static_string(LIB.keel_status_string(status)))
            self.assertEqual(LIB.keel_status_class(status), getattr(K, "KEEL_CLASS_" + tag), name)
            if status != K.KEEL_OK:
                self.assertIsInstance(KEEL.error(status), expected_type[tag], name)
        self.assertIsInstance(KEEL.error(K.KEEL_E_INVALID_ARGUMENT), ValueError)
        # An undefined code is read conservatively as a fault.
        self.assertEqual(LIB.keel_status_class(-99), K.KEEL_CLASS_FAULT)
        self.assertIsInstance(KEEL.error(-99), kc.SafetyFault)


def layout_pins():
    """{struct: ({field: offset}, size)} from the FIRST/NEXT/LAST lines of layout.c."""
    pins = {}
    with open(os.path.join(ROOT, "tests", "abi", "layout.c"), encoding="utf-8") as handle:
        for line in handle:
            first = re.match(r"FIRST\((\w+), (\w+)\)", line)
            nxt = re.match(r"NEXT\((\w+), \w+, (\w+), (\d+)\)", line)
            last = re.match(r"LAST\((\w+), \w+, (\d+)\)", line)
            if first:
                pins[first.group(1)] = ({first.group(2): 0}, None)
            elif nxt:
                pins[nxt.group(1)][0][nxt.group(2)] = int(nxt.group(3))
            elif last:
                pins[last.group(1)] = (pins[last.group(1)][0], int(last.group(2)))
    return pins


def c_type_to_ctypes(declaration):
    """ctypes type a correct binding uses for one C parameter or return type."""
    text = re.sub(r"\bconst\b", "", declaration).strip()
    pointers = text.count("*")
    base = text.replace("*", " ").split()[0]
    scalars = {"uint32_t": kc.u32, "int32_t": kc.i32, "uint64_t": kc.u64}
    if base == "keel_device":
        return kc.handle if pointers == 1 else kc.P(kc.handle)
    if base == "char" and pointers == 1:
        return ctypes.c_void_p  # static strings: read with a bound, never freed
    target = scalars.get(base) or kc.STRUCTS[base]
    return kc.P(target) if pointers == 1 else target


class Layout(unittest.TestCase):
    def test_structs_match_layout_c(self):
        pins = layout_pins()
        for name, struct in kc.STRUCTS.items():
            offsets, size = pins[name]
            self.assertEqual(ctypes.sizeof(struct), size, name)
            self.assertEqual([f[0] for f in struct._fields_], list(offsets), name)
            for field, offset in offsets.items():
                self.assertEqual(getattr(struct, field).offset, offset, name + "." + field)

    def test_prototypes_match_header(self):
        with open(os.path.join(ROOT, "tests", "abi", "symbols.txt"), encoding="utf-8") as handle:
            symbols = [line.strip() for line in handle if line.strip() and line[0] != "#"]
        self.assertEqual(list(kc.PROTOTYPES), symbols)
        # Each declaration "type name(params);" of the preprocessed header.
        declared = re.findall(r"([\w\s\*]+?)\b(keel_\w+)\s*\(([^)]*)\)\s*;", KEEL.header_text)
        self.assertEqual([d[1] for d in declared], symbols)
        for result, name, params in declared:
            restype, argtypes = kc.PROTOTYPES[name]
            self.assertIs(restype, c_type_to_ctypes(result), name)
            wanted = [] if params.strip() == "void" else [
                c_type_to_ctypes(re.sub(r"\w+$", "", p.strip())) for p in params.split(",")]
            self.assertEqual(argtypes, wanted, name)


class Loop(unittest.TestCase):
    def test_arm_drive_threads_close(self):
        with open_sim() as device:
            caps = device.value("get_capabilities")
            self.assertEqual(caps & (K.KEEL_CAP_SIMULATED | K.KEEL_CAP_MOTORS),
                             K.KEEL_CAP_SIMULATED | K.KEEL_CAP_MOTORS)
            self.assertEqual(device.state(), K.KEEL_STATE_DISARMED)
            loop = kc.ControlLoop(device)
            arm(device, loop)
            snapshot = loop.run(3, lambda s: (K.KEEL_CYCLE_DRIVE, 400, -400))
            self.assertEqual((snapshot.output_left, snapshot.output_right), (400, -400))
            sim = device.get("sim_get_state", kc.SimState)
            self.assertEqual((sim.bridge_left, sim.bridge_right), (400, -400))
            self.assertTrue(sim.flags & K.KEEL_SIM_MOTOR_PERMIT)

            results = {}

            def foreign():
                for key, args in (("go", (100, 100)), ("stop", (0, 0))):
                    try:
                        device.call("set_motors", *args)
                        results[key] = K.KEEL_OK
                    except kc.KeelError as error:
                        results[key] = error
            thread = threading.Thread(target=foreign)
            thread.start()
            thread.join()
            self.assertIsInstance(results["go"], kc.CommandRejected)
            self.assertEqual(results["go"].status, K.KEEL_E_CONFLICT)
            self.assertEqual(results["stop"], K.KEEL_OK)
            snapshot = loop.run(1, idle)
            self.assertEqual(snapshot.state, K.KEEL_STATE_ARMED)
            self.assertEqual((snapshot.output_left, snapshot.output_right), (0, 0))

            device.call("disarm")
            self.assertEqual(device.state(), K.KEEL_STATE_DISARMED)
            self.assertEqual(device.close(), K.KEEL_OK)
            self.assertEqual(device.final_fault.status, K.KEEL_OK)
            self.assertEqual(device.final_fault.cleanup_status, K.KEEL_OK)


class Faults(unittest.TestCase):
    def test_fault_clear_holdoff_emergency_stop(self):
        with open_sim() as device:
            loop = kc.ControlLoop(device)
            loop.run(5, idle)
            unhealthy = kc.CycleInput(health=K.KEEL_HEALTH_CONTROL_OK, cycle_id=loop.cycle_id + 1)
            with self.assertRaises(kc.SafetyFault) as caught:
                loop.cycle(unhealthy)
            fault = caught.exception.fault
            self.assertEqual(caught.exception.status, K.KEEL_E_HEALTH)
            self.assertEqual((fault.status, fault.cause), (K.KEEL_E_HEALTH, K.KEEL_CAUSE_HEALTH_FLAGS))
            self.assertEqual(fault.detail, K.KEEL_HEALTH_IO_OK | K.KEEL_HEALTH_NO_EXCEPTION)
            self.assertEqual(device.state(), K.KEEL_STATE_FAULTED)
            with self.assertRaises(kc.SafetyFault) as caught:
                device.call("request_arm")
            self.assertEqual(caught.exception.status, K.KEEL_E_FAULTED)

            device.call("clear_fault")
            self.assertEqual(device.state(), K.KEEL_STATE_DISARMED)
            self.assertTrue(device.get("get_fault", kc.Fault).flags & K.KEEL_FAULT_CLEARED)
            with self.assertRaises(kc.CommandRejected) as caught:
                device.call("request_arm")
            self.assertEqual(caught.exception.status, K.KEEL_E_STATE)
            self.assertTrue(device.get("get_snapshot", kc.Snapshot).flags & K.KEEL_SNAP_REARM_HOLDOFF)

            loop.run(K.KEEL_REARM_HOLDOFF_MS // K.KEEL_TICK_PERIOD_MS, idle)
            arm(device, loop)
            device.call("emergency_stop")
            fault = device.get("get_fault", kc.Fault)
            self.assertEqual((fault.status, fault.cause), (K.KEEL_E_FAULTED, K.KEEL_CAUSE_USER_STOP))
            with self.assertRaises(kc.SafetyFault) as caught:
                loop.run(1, idle)
            self.assertEqual(caught.exception.status, K.KEEL_E_FAULTED)
            self.assertEqual(caught.exception.fault.cause, K.KEEL_CAUSE_USER_STOP)


class Peripherals(unittest.TestCase):
    def test_leds_and_range(self):
        with open_sim() as device:
            loop = kc.ControlLoop(device)
            device.call("set_rgb", 0, 0x102030)
            device.call("sim_set", K.KEEL_SIM_FAIL_I2C, 1)
            with self.assertRaises(kc.PeripheralError) as caught:
                device.call("set_rgb_all", 0xFF0000)
            self.assertEqual(caught.exception.status, K.KEEL_E_PERIPHERAL)
            self.assertEqual(device.state(), K.KEEL_STATE_DISARMED)
            self.assertEqual(device.value("get_rgb", kc.u32, 0), 0x102030)
            frame = (kc.u32 * K.KEEL_LED_COUNT)(*range(1, K.KEEL_LED_COUNT + 1))
            device.call("set_rgb_frame", frame, K.KEEL_LED_COUNT)
            self.assertEqual(device.value("get_rgb", kc.u32, 5), 6)

            loop.run(30, idle)  # two heartbeat falls qualify sensor power
            device.call("sim_set", K.KEEL_SIM_RANGE_MM, 750)
            result = device.get("range_measure", kc.Range)
            self.assertEqual((result.status, result.distance_mm), (K.KEEL_RANGE_VALID, 750))
            self.assertEqual(device.get("get_snapshot", kc.Snapshot).sensor_power,
                             K.KEEL_SENSOR_CONFIRMED)
        with open_sim(K.KEEL_OPEN_NO_RANGE) as device:
            with self.assertRaises(kc.CommandRejected) as caught:
                device.call("range_start")
            self.assertEqual(caught.exception.status, K.KEEL_E_UNSUPPORTED)


class Handles(unittest.TestCase):
    def test_open_errors_and_closed_handles(self):
        with self.assertRaises(kc.SetupError) as caught:
            KEEL.open(backend=99)
        self.assertEqual(caught.exception.status, K.KEEL_E_INVALID_ARGUMENT)
        self.assertEqual(caught.exception.report.status, K.KEEL_E_INVALID_ARGUMENT)
        with self.assertRaises(kc.SetupError):
            KEEL.open(backend=K.KEEL_BACKEND_SIMULATOR, command_timeout_ms=5000)

        device = open_sim()
        loop = kc.ControlLoop(device)
        short = kc.CycleInput(health=K.KEEL_HEALTH_ALL, cycle_id=1)
        short.struct_size = 4
        with self.assertRaises(kc.InvalidArgument):
            loop.cycle(short)
        self.assertEqual(device.get("get_snapshot", kc.Snapshot).last_cycle_id, 0)  # no tick
        self.assertEqual(device.close(), K.KEEL_OK)
        self.assertEqual(device.close(), K.KEEL_OK)  # idempotent: closed once
        with self.assertRaises(kc.ClosedError) as caught:
            device.state()
        self.assertEqual(caught.exception.status, K.KEEL_E_INVALID_HANDLE)
        self.assertEqual(LIB.keel_close(device.raw, None), K.KEEL_E_INVALID_HANDLE)
        self.assertEqual(LIB.keel_close(None, None), K.KEEL_OK)


# ---- conformance scenarios -----------------------------------------------------

SCENARIOS = os.path.join(ROOT, "tests", "bindings", "scenarios.json")
METADATA = {"keel_abi_version", "keel_version_string", "keel_status_name", "keel_status_string",
            "keel_status_class", "keel_fault_cause_name"}
STRINGS = {"keel_version_string", "keel_status_name", "keel_status_string", "keel_fault_cause_name"}
# Functions whose last parameter is an output, and its type.
OUTPUTS = {"keel_get_capabilities": kc.u32, "keel_now_us": kc.u64, "keel_get_state": kc.u32,
           "keel_read_safety_inputs": kc.u32, "keel_get_rgb": kc.u32, "keel_get_snapshot": kc.Snapshot,
           "keel_get_fault": kc.Fault, "keel_range_poll": kc.Range, "keel_range_measure": kc.Range,
           "keel_take_buttons": kc.Buttons, "keel_sim_get_state": kc.SimState, "keel_close": kc.Fault}


class ScenarioReplay:
    """Replays one scenario on one simulator device; the calling thread is the owner thread."""

    def __init__(self, device):
        self.device = device
        self.cycle_id = device.get("get_snapshot", kc.Snapshot).last_cycle_id
        self.next_us = device.now_us()
        self.status, self.output, self.where = K.KEEL_OK, None, ""

    def argument(self, value):
        if isinstance(value, str):
            return getattr(K, value)
        if isinstance(value, list):
            return [self.argument(item) for item in value]
        if value == {"cycle_id": "next"}:
            self.cycle_id += 1
            return self.cycle_id
        if isinstance(value, dict) and "now_plus_us" in value:
            return self.device.now_us() + value["now_plus_us"]
        return value

    def invoke(self, name, arguments):
        """Make one call; return (status, output)."""
        raw = self.device.raw
        if name in METADATA:
            result = getattr(LIB, name)(*arguments)
            return K.KEEL_OK, kc.static_string(result) if name in STRINGS else result
        if name == "keel_cycle":
            health, cycle_id, command, left, right, flags = arguments
            cycle_input = kc.CycleInput(health=health, cycle_id=cycle_id, command=command, left=left,
                                        right=right, flags=flags)
            out = kc.Snapshot()
            return LIB.keel_cycle(raw, ctypes.byref(cycle_input), ctypes.byref(out)), out
        if name == "keel_set_rgb_frame":
            frame = (kc.u32 * len(arguments[0]))(*arguments[0])
            return LIB.keel_set_rgb_frame(raw, frame, len(arguments[0])), None
        out_type = OUTPUTS.get(name)
        if out_type is None:
            return getattr(LIB, name)(raw, *arguments), None
        out = out_type()
        status = getattr(LIB, name)(raw, *arguments, ctypes.byref(out))
        if name == "keel_close":
            self.device.close_status = status
        return status, out.value if isinstance(out, (kc.u32, kc.u64)) else out

    def call(self, name, args, thread):
        arguments = [self.argument(value) for value in args]
        outcome = []
        if thread == "other":
            worker = threading.Thread(target=lambda: outcome.append(self.invoke(name, arguments)))
            worker.start()
            worker.join()
        else:
            outcome.append(self.invoke(name, arguments))
        self.status, self.output = outcome[0]

    def cycles(self, count, command, left, right):
        self.status, self.output = K.KEEL_OK, None
        for _ in range(count):
            self.cycle_id += 1
            cycle_input = kc.CycleInput(health=K.KEEL_HEALTH_ALL, cycle_id=self.cycle_id, command=command,
                                        left=left, right=right)
            status = LIB.keel_cycle(self.device.raw, ctypes.byref(cycle_input), None)
            if self.status == K.KEEL_OK:
                self.status = status
            self.next_us = max(self.next_us + K.KEEL_TICK_PERIOD_MS * 1000, self.device.now_us())
            self.device.call("sleep_until", self.next_us)

    def check(self, actual, expected, where):
        if isinstance(expected, dict):
            for field, value in expected.items():
                self.check(getattr(actual, field), value, where + "." + field)
        elif isinstance(expected, list):
            bits = sum(getattr(K, name) for name in expected)
            assert actual & bits == bits, "%s: %r lacks %s" % (where, actual, expected)
        elif isinstance(expected, str) and isinstance(actual, str):
            assert actual == expected, "%s: %r" % (where, actual)
        else:
            value = getattr(K, expected) if isinstance(expected, str) else expected
            assert actual == value, "%s: %r is not %s" % (where, actual, expected)

    def expect(self, step):
        name = kc.static_string(LIB.keel_status_name(self.status))
        assert self.status == getattr(K, step["expect"]), "%s: status %s" % (self.where, name)
        if "output" in step:
            self.check(self.output, step["output"], self.where + " output")
        if "state" in step:
            assert self.device.state() == getattr(K, step["state"]), self.where + ": state"
        if "fault" in step:
            fault = self.device.get("get_fault", kc.Fault)
            wanted = step["fault"]
            assert fault.cause == getattr(K, wanted["cause"]), self.where + ": fault cause"
            assert fault.status == getattr(K, wanted["status"]), self.where + ": fault status"
            assert fault.flags == sum(getattr(K, flag) for flag in wanted["flags"]), self.where + ": flags"
        if "snapshot" in step:
            self.check(self.device.get("get_snapshot", kc.Snapshot), step["snapshot"],
                       self.where + " snapshot")


class Scenarios(unittest.TestCase):
    def test_scenarios_hold_against_the_library(self):
        with open(SCENARIOS, encoding="utf-8") as handle:
            data = json.load(handle)
        self.assertEqual((data["license"], data["copyright"], data["format"]),
                         ("SPDX-License-Identifier: MIT", "Copyright (c) 2026 BisocM", 1))
        used = {"keel_open"}
        for scenario in data["scenarios"]:
            with self.subTest(scenario=scenario["name"]):
                self.replay(scenario)
            for step in scenario["steps"]:
                used.add(step.get("call", "keel_sim_set" if "sim" in step else ""))
        self.assertEqual(sorted(set(kc.PROTOTYPES) - used), [], "functions no scenario covers")

    def replay(self, scenario):
        opening = scenario["open"]
        # Scenarios run on the simulator only, so no replay can reach a robot's board.
        self.assertEqual(opening.get("backend"), "KEEL_BACKEND_SIMULATOR", scenario["name"])
        flags = sum(getattr(K, flag) for flag in opening.get("flags", []))
        with KEEL.open(backend=K.KEEL_BACKEND_SIMULATOR, flags=flags) as device:
            replay = ScenarioReplay(device)
            for number, step in enumerate(scenario["steps"], 1):
                replay.where = "%s step %d" % (scenario["name"], number)
                if "call" in step:
                    replay.call(step["call"], step.get("args", []), step.get("thread", "owner"))
                elif "cycles" in step:
                    command = getattr(K, step.get("command", "KEEL_CYCLE_NONE"))
                    replay.cycles(step["cycles"], command, step.get("left", 0), step.get("right", 0))
                elif "sim" in step:
                    replay.call("keel_sim_set", [step["sim"], step["value"]], "owner")
                elif "expect" in step:
                    replay.expect(step)
                else:
                    self.fail("%s: unknown step %r" % (replay.where, step))


CASES = {"metadata": Metadata, "layout": Layout, "loop": Loop, "faults": Faults,
         "peripherals": Peripherals, "handles": Handles, "scenarios": Scenarios}


def main(argv):
    names = argv[1:] or ["all"]
    if names == ["all"]:
        names = list(CASES)
    unknown = [name for name in names if name not in CASES]
    if unknown:
        print("test_ctypes: unknown case(s): %s (cases: %s)" % (" ".join(unknown), " ".join(CASES)))
        return 2
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(CASES[name])
                               for name in names)
    result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
