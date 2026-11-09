# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Reference ctypes binding for libkeel (ABI 1.x), as docs/bindings.md describes.

It is the worked example behind the binding rules and the subject of the
ctypes consumer tests; the published Python binding grows from it. Rules it
demonstrates:

- Loading: KEEL_LIBRARY (an absolute path) if set, otherwise the SONAME
  libkeel.so.1; then keel_abi_version() >> 16 must be 1.
- Constants come from the preprocessed header (cc -E -P -DKEEL_API=), never
  from hand-copied numbers.
- Structs carry struct_size; strings are static and read with a bound.
- Statuses map to exceptions by class, never by individual code.
- open() raises SetupError for every non-OK status; close() never raises.
"""

import ctypes
import os
import sys
import tempfile

# Sibling modules are imported from the source tree; leave no __pycache__ there.
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "abi"))
import check_constants  # noqa: E402  (the header parser of the ABI gate)

ABI_MAJOR = 1
ABI_MINOR_REQUIRED = 0
SONAME = "libkeel.so.1"
STRING_MAX = 256  # KEEL_STRING_MAX: the header's bound on every returned string


def preprocess_header(header, cc="cc"):
    """keel.h as binding generators read it: cc -E -P -DKEEL_API= with plain <stdint.h>."""
    with tempfile.TemporaryDirectory() as fake:
        with open(os.path.join(fake, "stdint.h"), "w", encoding="utf-8") as handle:
            handle.write(check_constants.FAKE_STDINT)
        with open(os.path.join(fake, "stddef.h"), "w", encoding="utf-8") as handle:
            handle.write(check_constants.FAKE_STDDEF)
        return check_constants.preprocess(cc, header, fake)


def generate_constants(header_text):
    """Return {name: value} for every enumerator of the preprocessed header."""
    known = {}
    check_constants.parse_enums(header_text, known)
    return known


class Constants:
    """Attribute access to the generated enumerators: K.KEEL_OK, K.KEEL_E_IO, ..."""

    def __init__(self, values):
        self.__dict__.update(values)


# ---- structs (keel.h layouts; tests check them against tests/abi/layout.c) ----

u32, i32, u64 = ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint64


class Sized(ctypes.Structure):
    """A keel struct: constructing one sets struct_size to the size this binding knows."""

    def __init__(self, **fields):
        super().__init__(struct_size=ctypes.sizeof(self), **fields)


class OpenOptions(Sized):
    _fields_ = [("struct_size", u32), ("backend", u32), ("flags", u32),
                ("command_timeout_ms", u32), ("gpiochip_path", ctypes.c_char * 128),
                ("i2c_path", ctypes.c_char * 128)]


class OpenReport(Sized):
    _fields_ = [("struct_size", u32), ("status", i32), ("resource", u32), ("os_error", i32),
                ("line", i32), ("backend", u32), ("capabilities", u32), ("warnings", u32),
                ("timing_class", u32), ("realtime_os_error", i32),
                ("path", ctypes.c_char * 128), ("consumer", ctypes.c_char * 32),
                ("hint", ctypes.c_char * 256)]


class Fault(Sized):
    _fields_ = [("struct_size", u32), ("status", i32), ("cause", u32), ("state", u32),
                ("ack_phase", u32), ("inputs", u32), ("os_error", i32), ("cleanup_status", i32),
                ("detail", u64), ("time_us", u64), ("fault_count", u32), ("flags", u32),
                ("cleanup_os_error", i32), ("reserved0", u32)]


class Snapshot(Sized):
    _fields_ = [("struct_size", u32), ("state", u32), ("flags", u32), ("inputs", u32),
                ("time_us", u64), ("inputs_sampled_us", u64), ("last_cycle_id", u64),
                ("owner_thread_id", u64), ("tick_budget_us", i32), ("ack_phase", u32),
                ("command_left", i32), ("command_right", i32), ("output_left", i32),
                ("output_right", i32), ("command_remaining_ms", u32), ("buttons_held", u32),
                ("buttons_pressed", u32), ("buttons_released", u32), ("button_leds", u32),
                ("sensor_power", u32), ("servo_pulse_us", u32), ("servo_lease_remaining_ms", u32),
                ("servo_cooldown_remaining_ms", u32), ("fault_cause", u32), ("last_rejection", i32),
                ("heartbeat_restarts", u32), ("stale_ack_restarts", u32), ("timing_class", u32),
                ("pwm_late_wakes", u32), ("pwm_max_lateness_us", u32), ("pwm_lease_expiries", u32),
                ("fault_count", u32)]


class Buttons(Sized):
    _fields_ = [("struct_size", u32), ("held", u32), ("pressed", u32), ("released", u32)]


class Range(Sized):
    _fields_ = [("struct_size", u32), ("status", u32), ("distance_mm", u32), ("echo_us", u32),
                ("measured_at_us", u64), ("sequence", u32), ("reserved0", u32)]


class CycleInput(Sized):
    _fields_ = [("struct_size", u32), ("health", u32), ("cycle_id", u64), ("command", u32),
                ("left", i32), ("right", i32), ("flags", u32)]


class SimState(Sized):
    _fields_ = [("struct_size", u32), ("pins", u32), ("mcu_stage", u32), ("mcu_faults", u32),
                ("flags", u32), ("bridge_left", i32), ("bridge_right", i32),
                ("servo_pulse_us", u32), ("now_us", u64), ("heartbeat_falls", u32),
                ("reserved0", u32)]


STRUCTS = {"keel_open_options": OpenOptions, "keel_open_report": OpenReport,
           "keel_fault": Fault, "keel_snapshot": Snapshot, "keel_buttons": Buttons,
           "keel_range": Range, "keel_cycle_input": CycleInput, "keel_sim_state": SimState}

# ---- functions: restype and argtypes of all 37 exports, in header order -------

P = ctypes.POINTER
handle = ctypes.c_void_p  # keel_device *: an opaque token, never dereferenced
PROTOTYPES = {
    "keel_abi_version": (u32, []),
    "keel_version_string": (ctypes.c_void_p, []),
    "keel_status_name": (ctypes.c_void_p, [i32]),
    "keel_status_string": (ctypes.c_void_p, [i32]),
    "keel_status_class": (u32, [i32]),
    "keel_fault_cause_name": (ctypes.c_void_p, [u32]),
    "keel_open": (i32, [P(OpenOptions), P(handle), P(OpenReport)]),
    "keel_close": (i32, [handle, P(Fault)]),
    "keel_get_capabilities": (i32, [handle, P(u32)]),
    "keel_now_us": (i32, [handle, P(u64)]),
    "keel_sleep_until": (i32, [handle, u64]),
    "keel_tick": (i32, [handle, u64, u32]),
    "keel_cycle": (i32, [handle, P(CycleInput), P(Snapshot)]),
    "keel_request_arm": (i32, [handle]),
    "keel_disarm": (i32, [handle]),
    "keel_emergency_stop": (i32, [handle]),
    "keel_clear_fault": (i32, [handle]),
    "keel_get_state": (i32, [handle, P(u32)]),
    "keel_get_snapshot": (i32, [handle, P(Snapshot)]),
    "keel_get_fault": (i32, [handle, P(Fault)]),
    "keel_read_safety_inputs": (i32, [handle, P(u32)]),
    "keel_set_motors": (i32, [handle, i32, i32]),
    "keel_brake": (i32, [handle]),
    "keel_servo_start": (i32, [handle, u32, u32]),
    "keel_servo_set_pulse": (i32, [handle, u32]),
    "keel_servo_end": (i32, [handle]),
    "keel_range_start": (i32, [handle]),
    "keel_range_poll": (i32, [handle, P(Range)]),
    "keel_range_measure": (i32, [handle, P(Range)]),
    "keel_set_rgb": (i32, [handle, u32, u32]),
    "keel_set_rgb_all": (i32, [handle, u32]),
    "keel_set_rgb_frame": (i32, [handle, P(u32), u32]),
    "keel_get_rgb": (i32, [handle, u32, P(u32)]),
    "keel_set_button_leds": (i32, [handle, u32, u32]),
    "keel_take_buttons": (i32, [handle, P(Buttons)]),
    "keel_sim_set": (i32, [handle, u32, i32]),
    "keel_sim_get_state": (i32, [handle, P(SimState)]),
}


def load_library(path=None):
    """Load libkeel the way every binding does and check the ABI version."""
    path = path or os.environ.get("KEEL_LIBRARY")
    if path and not os.path.isabs(path):
        raise ImportError("KEEL_LIBRARY must be an absolute path, not %r" % path)
    library = ctypes.CDLL(path or SONAME)
    for name, (restype, argtypes) in PROTOTYPES.items():
        function = getattr(library, name)
        function.restype = restype
        function.argtypes = argtypes
    version = library.keel_abi_version()
    if version >> 16 != ABI_MAJOR or (version & 0xFFFF) < ABI_MINOR_REQUIRED:
        raise ImportError("libkeel ABI %d.%d does not satisfy %d.%d"
                          % (version >> 16, version & 0xFFFF, ABI_MAJOR, ABI_MINOR_REQUIRED))
    return library


def static_string(address):
    """Read a returned static string with the header's bound; never free it.

    The scan stops at the NUL, so it never reads memory past the string, and at
    KEEL_STRING_MAX bytes, so a missing terminator cannot run away.
    """
    if not address:
        raise ValueError("libkeel returned a NULL string")
    view = (ctypes.c_ubyte * STRING_MAX).from_address(address)
    for length in range(STRING_MAX):
        if view[length] == 0:
            return ctypes.string_at(address, length).decode("utf-8")
    raise ValueError("a libkeel string is not terminated within KEEL_STRING_MAX bytes")


# ---- exceptions: one per status class ------------------------------------------

class KeelError(Exception):
    """A non-OK status. status is the raw code, name its enumerator name."""

    def __init__(self, library, status, detail=""):
        self.status = status
        self.name = static_string(library.keel_status_name(status))
        message = "%s: %s" % (self.name, static_string(library.keel_status_string(status)))
        super().__init__(message + (" (%s)" % detail if detail else ""))


class CommandRejected(KeelError):
    """REJECTED class: refused, device unchanged (except the motion coast rule)."""


class InvalidArgument(CommandRejected, ValueError):
    """KEEL_E_INVALID_ARGUMENT, the REJECTED status Python reports as a ValueError."""


class SafetyFault(KeelError):
    """FAULT class: the device is FAULTED; fault holds the record (or None)."""

    def __init__(self, library, status, fault):
        self.fault = fault
        cause = static_string(library.keel_fault_cause_name(fault.cause)) if fault else "unknown"
        super().__init__(library, status, "cause " + cause)


class PeripheralError(KeelError):
    """PERIPHERAL class: an LED or ultrasonic failure; safety state unchanged."""


class SetupError(KeelError):
    """Any failed open; report holds the keel_open_report."""

    def __init__(self, library, status, report):
        self.report = report
        super().__init__(library, status, report.hint.decode("utf-8", "replace"))


class ClosedError(KeelError):
    """HANDLE class: the device is closed, stale or owned by another process."""


class Keel:
    """The loaded library plus the constants generated from its header."""

    def __init__(self, header, library_path=None, cc="cc"):
        self.lib = load_library(library_path)
        self.header_text = preprocess_header(header, cc)
        self.K = Constants(generate_constants(self.header_text))

    def error(self, status, device=None):
        """The exception for status, chosen by its class."""
        K = self.K
        status_class = self.lib.keel_status_class(status)
        if status_class == K.KEEL_CLASS_FAULT:
            fault = Fault()
            if device is None or self.lib.keel_get_fault(device, ctypes.byref(fault)) != K.KEEL_OK:
                fault = None
            return SafetyFault(self.lib, status, fault)
        if status == K.KEEL_E_INVALID_ARGUMENT:
            return InvalidArgument(self.lib, status)
        if status_class == K.KEEL_CLASS_REJECTED:
            return CommandRejected(self.lib, status)
        if status_class == K.KEEL_CLASS_PERIPHERAL:
            return PeripheralError(self.lib, status)
        if status_class == K.KEEL_CLASS_HANDLE:
            return ClosedError(self.lib, status)
        return KeelError(self.lib, status)

    def open(self, backend=0, flags=0, command_timeout_ms=0):
        options = OpenOptions(backend=backend, flags=flags, command_timeout_ms=command_timeout_ms)
        report = OpenReport()
        raw = handle()
        status = self.lib.keel_open(ctypes.byref(options), ctypes.byref(raw), ctypes.byref(report))
        if status != self.K.KEEL_OK:
            raise SetupError(self.lib, status, report)
        return Device(self, raw.value, report)


class Device:
    """One open device. Use it in a with-block; close() is idempotent and never raises."""

    def __init__(self, keel, raw, report):
        self.keel = keel
        self.raw = raw
        self.report = report
        self.close_status = None
        self.final_fault = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self.close_status is None:
            fault = Fault()
            self.close_status = self.keel.lib.keel_close(self.raw, ctypes.byref(fault))
            self.final_fault = fault
        return self.close_status

    def call(self, name, *args):
        """Call keel_<name>(device, *args) and raise by class on a non-OK status."""
        status = getattr(self.keel.lib, "keel_" + name)(self.raw, *args)
        if status != self.keel.K.KEEL_OK:
            raise self.keel.error(status, self.raw)

    def get(self, name, struct_type, *args):
        out = struct_type()
        self.call(name, *(args + (ctypes.byref(out),)))
        return out

    def value(self, name, ctype=u32, *args):
        out = ctype()
        self.call(name, *(args + (ctypes.byref(out),)))
        return out.value

    def state(self):
        return self.value("get_state")

    def now_us(self):
        return self.value("now_us", u64)


class ControlLoop:
    """The loop of docs/bindings.md: one keel_cycle per 10 ms on the device clock.

    It runs on the calling thread, which must be a dedicated OS thread for the
    whole time the device is armed. While it runs, sys.setswitchinterval is
    0.001 s so the GIL hand-off after keel_sleep_until stays far inside the
    40 ms stall budget.
    """

    def __init__(self, device):
        self.device = device
        self.cycle_id = 0
        self.next_us = device.now_us()
        self.snapshot = Snapshot()

    def run(self, cycles, step):
        """Run cycles iterations; step(snapshot) returns (command, left, right)."""
        K = self.device.keel.K
        previous = sys.getswitchinterval()
        sys.setswitchinterval(0.001)
        try:
            for _ in range(cycles):
                command, left, right = step(self.snapshot)
                self.cycle_id += 1
                cycle_input = CycleInput(health=K.KEEL_HEALTH_ALL, cycle_id=self.cycle_id,
                                         command=command, left=left, right=right)
                self.cycle(cycle_input)
        finally:
            sys.setswitchinterval(previous)
        return self.snapshot

    def cycle(self, cycle_input):
        lib, K = self.device.keel.lib, self.device.keel.K
        self.snapshot = Snapshot()
        status = lib.keel_cycle(self.device.raw, ctypes.byref(cycle_input),
                                ctypes.byref(self.snapshot))
        self.next_us += K.KEEL_TICK_PERIOD_MS * 1000
        if status != K.KEEL_OK:
            raise self.device.keel.error(status, self.device.raw)
        self.device.call("sleep_until", self.next_us)
