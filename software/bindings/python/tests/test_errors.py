# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Errors: the class-to-exception mapping, messages, setup reports, injected faults and the negative tests.

The integer checks are tested from the same generated tables as the conversions themselves (PROTOTYPES, SATURATED
and the input structs' fields), so no position is missed.
"""

from __future__ import annotations

import ctypes
import inspect
import pickle
from collections.abc import Callable
from typing import Any

import pytest

import keel
from keel._internal import _generated as g
from keel.errors.error_factory import error_for
from support.native_calls import NativeCallController
from support.simulator import CycleDriver, open_sim

CLASS_TYPES = {
    keel.StatusClass.REJECTED: keel.RejectedError,
    keel.StatusClass.FAULT: keel.SafetyFaultError,
    keel.StatusClass.PERIPHERAL: keel.PeripheralError,
    keel.StatusClass.SETUP: keel.SafetyFaultError,  # SETUP is reached only through keel_open (SetupError)
    keel.StatusClass.HANDLE: keel.HandleError,
}
REJECTED_TYPES = {
    keel.Status.E_INVALID_ARGUMENT: keel.InvalidArgumentError,
    keel.Status.E_STATE: keel.StateError,
    keel.Status.E_AGAIN: keel.AgainError,
    keel.Status.E_UNSUPPORTED: keel.UnsupportedError,
    keel.Status.E_CONFLICT: keel.ConflictError,
}
LIMITS = {
    ctypes.c_uint32: (-1, 2**32 + 7),
    ctypes.c_uint64: (-1, 2**64 + 7),
    ctypes.c_int32: (-(2**31) - 1, 2**31),
}
NOT_INTEGERS = (1.5, "1", None)


class TestMapping:
    @pytest.mark.parametrize("status", [status for status in keel.Status if status != keel.Status.OK])
    def test_every_code_maps_by_class(self, natives: NativeCallController, status: keel.Status) -> None:
        error = error_for(status, "keel_test")
        status_class = keel.status_class(status)
        expected = REJECTED_TYPES.get(status, CLASS_TYPES[status_class])
        assert type(error) is expected
        assert isinstance(error, keel.KeelError)
        assert error.status == status
        assert error.code == int(status)
        assert error.status_class == status_class
        assert error.name == "KEEL_" + status.name
        assert error.description == keel.status_string(status)
        assert error.function == "keel_test"
        assert error.snapshot is None

    def test_invalid_argument_is_also_a_value_error(self, natives: NativeCallController) -> None:
        error = error_for(keel.Status.E_INVALID_ARGUMENT, "keel_set_motors")
        assert isinstance(error, ValueError)
        assert isinstance(error, keel.RejectedError)

    def test_an_undefined_code_is_a_fault(self, natives: NativeCallController) -> None:
        error = error_for(-99, "keel_test")
        assert type(error) is keel.SafetyFaultError
        assert error.name == "KEEL_E_UNKNOWN"
        assert error.status.name == "UNRECOGNIZED_-99"
        assert error.fault is None

    def test_an_unknown_class_is_a_fault(self, natives: NativeCallController, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setattr(natives.library, "_classes", {})
        natives.overrides["keel_status_class"] = lambda status: 9
        error = error_for(-77, "keel_test")
        assert type(error) is keel.SafetyFaultError
        assert error.status_class.name == "UNRECOGNIZED_9"

    def test_an_unknown_rejected_code_is_a_rejected_error(
        self, natives: NativeCallController, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.setattr(natives.library, "_classes", {})
        natives.overrides["keel_status_class"] = lambda status: keel.StatusClass.REJECTED
        assert type(error_for(-55, "keel_test")) is keel.RejectedError

    def test_the_hierarchy(self) -> None:
        for error_type in (keel.RejectedError, keel.SafetyFaultError, keel.PeripheralError, keel.SetupError):
            assert issubclass(error_type, keel.KeelError)
        assert issubclass(keel.HandleError, keel.KeelError)
        for error_type in REJECTED_TYPES.values():
            assert issubclass(error_type, keel.RejectedError)
        assert not issubclass(keel.LibraryError, keel.KeelError)

    def test_errors_pickle_without_the_library(self, sim: keel.Device) -> None:
        sim.emergency_stop()
        with pytest.raises(keel.SafetyFaultError) as caught:
            sim.request_arm()
        copy = pickle.loads(pickle.dumps(caught.value))  # a round trip of this test's own object
        assert type(copy) is keel.SafetyFaultError
        assert str(copy) == str(caught.value)
        assert copy.fault == caught.value.fault
        assert copy.status == keel.Status.E_FAULTED


class TestMessages:
    def test_rejected_message(self, sim: keel.Device) -> None:
        with pytest.raises(keel.StateError) as caught:
            sim.set_motors(100, 100)
        description = keel.status_string(keel.Status.E_STATE)
        assert str(caught.value) == f"keel_set_motors: KEEL_E_STATE: {description}"

    def test_fault_message_names_cause_and_detail(self, sim: keel.Device) -> None:
        with pytest.raises(keel.SafetyFaultError) as caught:
            sim.tick(1, keel.Health.CONTROL_OK)
        error = caught.value
        assert error.fault is not None
        assert (error.fault.cause, error.fault.detail) == (keel.FaultCause.HEALTH_FLAGS, 6)
        assert str(error).startswith("keel_tick: KEEL_E_HEALTH: ")
        assert str(error).endswith("; cause HEALTH_FLAGS, detail 6")

    def test_setup_error_carries_the_report(self, natives: NativeCallController) -> None:
        with natives.record() as calls, pytest.raises(keel.SetupError) as caught:
            keel.open(backend=99)
        assert calls == ["keel_open"]
        error = caught.value
        assert error.status == keel.Status.E_INVALID_ARGUMENT
        assert error.report.status == keel.Status.E_INVALID_ARGUMENT
        assert error.report.resource == keel.OpenResource.OPTIONS
        assert error.report.hint
        assert "; resource OPTIONS; remedy: " in str(error)
        assert not isinstance(error, ValueError)

    def test_setup_error_for_a_timeout_out_of_range(self, natives: NativeCallController) -> None:
        with pytest.raises(keel.SetupError) as caught:
            keel.open(backend=keel.Backend.SIMULATOR, command_timeout_ms=5000)
        assert caught.value.report.resource == keel.OpenResource.OPTIONS

    def test_setup_error_for_a_missing_backend(self, natives: NativeCallController) -> None:
        with pytest.raises(keel.SetupError) as caught:
            keel.open(backend=keel.Backend.R3)
        assert caught.value.status == keel.Status.E_NOT_FOUND
        assert caught.value.report.resource == keel.OpenResource.BACKEND

    def test_report_details_in_the_message(self, natives: NativeCallController) -> None:
        report = keel.OpenReport(
            status=keel.Status.E_IN_USE,
            resource=keel.OpenResource.GPIO_LINE,
            os_error=16,
            line=8,
            backend=keel.Backend.R3,
            capabilities=keel.Capability(0),
            warnings=keel.OpenWarning(0),
            timing_class=keel.TimingClass.NONE,
            realtime_os_error=0,
            path="/dev/gpiochip0",
            consumer="spi0",
            hint="set dtparam=spi=off",
        )
        message = str(keel.SetupError(keel.Status.E_IN_USE, "keel_open", report=report))
        assert "; resource GPIO_LINE; BCM8 is used by spi0; OS error 16 (" in message
        assert message.endswith("; path /dev/gpiochip0; remedy: set dtparam=spi=off")

    @pytest.mark.parametrize(
        ("line", "consumer", "expected"),
        [(13, "", "; BCM13; "), (-1, "keel", "; used by keel; "), (-1, "", "; remedy: ")],
    )
    def test_report_line_and_consumer(
        self, natives: NativeCallController, line: int, consumer: str, expected: str
    ) -> None:
        report = keel.OpenReport(
            status=keel.Status.E_IN_USE,
            resource=keel.OpenResource.NONE,
            os_error=0,
            line=line,
            backend=keel.Backend.R3,
            capabilities=keel.Capability(0),
            warnings=keel.OpenWarning(0),
            timing_class=keel.TimingClass.NONE,
            realtime_os_error=0,
            path="",
            consumer=consumer,
            hint="check the wiring",
        )
        assert expected in str(keel.SetupError(keel.Status.E_IN_USE, "keel_open", report=report))

    def test_detail_signed_for_hal_contract(self) -> None:
        record = g.FaultStruct()
        record.status = keel.Status.E_INTERNAL
        record.cause = keel.FaultCause.HAL_CONTRACT
        record.detail = 2**64 - 9
        fault = g.convert_fault(record)
        assert fault.detail == 2**64 - 9
        assert fault.detail_signed == -9
        record.detail = 42
        assert g.convert_fault(record).detail_signed == 42


class TestInjectedFaults:
    def test_gpio_write_failure(self, sim: keel.Device) -> None:
        sim.sim_set(keel.SimItem.FAIL_GPIO_WRITES, 1)
        with pytest.raises(keel.SafetyFaultError) as caught:
            sim.set_button_leds(keel.Button.A, keel.Button.A)
        assert caught.value.status == keel.Status.E_IO
        assert caught.value.fault is not None
        assert caught.value.fault.cause == keel.FaultCause.GPIO_WRITE
        assert sim.state() == keel.State.FAULTED

    def test_gpio_read_failure(self, sim: keel.Device) -> None:
        sim.sim_set(keel.SimItem.FAIL_GPIO_READS, 1)
        with pytest.raises(keel.SafetyFaultError) as caught:
            sim.take_buttons()
        assert caught.value.status == keel.Status.E_IO
        assert caught.value.fault is not None
        assert caught.value.fault.cause == keel.FaultCause.GPIO_READ

    def test_pwm_lateness(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        driver.cycles(2, keel.CycleCommand.DRIVE, 300, 300)
        sim.sim_set(keel.SimItem.PWM_LATENESS_US, 40_000)
        fault = self._fault_while_driving(driver)
        assert (fault.status, fault.cause) == (keel.Status.E_PWM, keel.FaultCause.PWM_OVERRUN)

    def test_pwm_stall(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        driver.cycles(2, keel.CycleCommand.DRIVE, 300, 300)
        sim.sim_set(keel.SimItem.PWM_STALL, 1)
        fault = self._fault_while_driving(driver)
        assert (fault.status, fault.cause) == (keel.Status.E_PWM, keel.FaultCause.PWM_STALL)

    def test_mcu_reset_while_armed(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        sim.sim_set(keel.SimItem.MCU_RESET, 1)
        fault = self._fault_while_driving(driver)
        assert (fault.status, fault.cause) == (keel.Status.E_FEEDBACK, keel.FaultCause.READY_LOST)

    def test_armed_stuck_high_is_sticky(self, sim: keel.Device) -> None:
        driver = CycleDriver(sim)
        driver.cycles(5)
        sim.sim_set(keel.SimItem.ARMED_STUCK_HIGH, 1)
        fault = self._fault_while_driving(driver)
        assert fault.cause == keel.FaultCause.ARMED_UNEXPECTED
        assert fault.sticky
        assert keel.SnapshotFlag.FAULT_STICKY in sim.snapshot().flags
        with pytest.raises(keel.SafetyFaultError) as caught:
            sim.clear_fault()
        assert caught.value.status == keel.Status.E_FAULTED

    def test_led_failure_is_a_peripheral_error(self, sim: keel.Device) -> None:
        sim.set_rgb(keel.Led.RGB1, 0x102030)
        sim.sim_set(keel.SimItem.FAIL_I2C, 1)
        with pytest.raises(keel.PeripheralError) as caught:
            sim.set_rgb_all(0xFF0000)
        assert caught.value.status == keel.Status.E_PERIPHERAL
        assert sim.state() == keel.State.DISARMED
        assert sim.rgb(keel.Led.RGB1) == 0x102030

    @staticmethod
    def _fault_while_driving(driver: CycleDriver) -> keel.Fault:
        error = TestInjectedFaults._drive_until_fault(driver)
        assert error.fault is not None
        assert error.snapshot is not None
        assert error.snapshot.state == keel.State.FAULTED
        return error.fault

    @staticmethod
    def _drive_until_fault(driver: CycleDriver) -> keel.SafetyFaultError:
        for _ in range(50):
            try:
                driver.cycle(keel.CycleCommand.DRIVE, 300, 300)
            except keel.SafetyFaultError as error:
                return error
            except keel.StateError:
                continue
        raise AssertionError("no fault within 50 cycles")


def integer_positions() -> list[tuple[str, str, Any]]:
    """(function, parameter, ctype) for every integer parameter of every generated call wrapper."""
    positions = []
    for name, (_, argtypes) in g.PROTOTYPES.items():
        parameters = list(inspect.signature(getattr(g, name)).parameters.values())[1:]
        for parameter, ctype in zip(parameters, argtypes, strict=True):
            if parameter.annotation == "SupportsIndex":
                positions.append((name, parameter.name, ctype))
    return positions


def field_positions() -> list[tuple[Callable[..., Any], type[ctypes.Structure], str, Any]]:
    """(builder, struct, field, ctype) for every integer field the binding writes."""
    positions = []
    for builder, struct in ((g.fill_open_options, g.OpenOptionsStruct), (g.fill_cycle_input, g.CycleInputStruct)):
        types = dict(struct._fields_)
        for parameter in list(inspect.signature(builder).parameters.values())[1:]:
            if parameter.annotation == "SupportsIndex":
                positions.append((builder, struct, parameter.name, types[parameter.name]))
    return positions


def call_with(natives: NativeCallController, token: int, function: str, parameter: str, value: object) -> None:
    """Call a generated wrapper with value in one integer position and harmless values elsewhere."""
    wrapper = getattr(g, function)
    arguments: list[object] = []
    for name in list(inspect.signature(wrapper).parameters)[1:]:
        if name == parameter:
            arguments.append(value)
        elif name == "device":
            arguments.append(token)
        else:
            arguments.append(0 if inspect.signature(wrapper).parameters[name].annotation == "SupportsIndex" else None)
    wrapper(natives.library.natives, *arguments)


def fill_with(builder: Callable[..., Any], struct: type[ctypes.Structure], field: str, value: object) -> Any:
    arguments: dict[str, object] = {}
    for name, parameter in list(inspect.signature(builder).parameters.items())[1:]:
        arguments[name] = 0 if parameter.annotation == "SupportsIndex" else ""
    arguments[field] = value
    return builder(struct(), **arguments)


class TestIntegerChecks:
    def test_every_integer_position_is_covered(self) -> None:
        positions = integer_positions()
        assert len(positions) == 21
        assert {(function, name) for function, name, _ in positions} >= {
            ("keel_set_motors", "left"),
            ("keel_tick", "health"),
            ("keel_status_name", "status"),
            ("keel_sleep_until", "deadline_us"),
        }
        assert len(field_positions()) == 9
        assert len(g.SATURATED) == 4

    def test_out_of_range_integers_raise_overflow_error_with_no_call(
        self, sim: keel.Device, natives: NativeCallController
    ) -> None:
        token = sim._state.token
        checked = 0
        for function, parameter, ctype in integer_positions():
            if (function, parameter) in g.SATURATED:
                continue
            for value in LIMITS[ctype]:
                with natives.record() as calls, pytest.raises(OverflowError) as caught:
                    call_with(natives, token, function, parameter, value)
                assert calls == [], function
                assert f"{function}: {parameter} = {value} is outside" in str(caught.value)
                assert str(caught.value).endswith("no call was made")
                checked += 1
        assert checked == 2 * (21 - len({(f, p) for f, p, _ in integer_positions()} & g.SATURATED))

    def test_non_integers_raise_type_error_with_no_call(self, sim: keel.Device, natives: NativeCallController) -> None:
        token = sim._state.token
        for function, parameter, _ in integer_positions():
            for value in NOT_INTEGERS:
                with natives.record() as calls, pytest.raises(TypeError, match=f"{function}: {parameter} must be"):
                    call_with(natives, token, function, parameter, value)
                assert calls == [], function

    def test_written_fields_are_checked(self) -> None:
        for builder, struct, field, ctype in field_positions():
            c_name = "keel_cycle_input" if struct is g.CycleInputStruct else "keel_open_options"
            if (c_name, field) not in g.SATURATED:
                for value in LIMITS[ctype]:
                    with pytest.raises(OverflowError, match=f": {field} = {value} is outside"):
                        fill_with(builder, struct, field, value)
            for value in NOT_INTEGERS:
                with pytest.raises(TypeError, match=f": {field} must be an integer"):
                    fill_with(builder, struct, field, value)

    def test_saturated_positions(self) -> None:
        assert g.saturate_i32("f", "p", 2**32 + 100) == 2**31 - 1
        assert g.saturate_i32("f", "p", -(2**40)) == -(2**31)
        assert g.saturate_i32("f", "p", 5) == 5
        built = fill_with(g.fill_cycle_input, g.CycleInputStruct, "left", 2**32 + 100)
        assert built.left == 2**31 - 1
        built = fill_with(g.fill_cycle_input, g.CycleInputStruct, "right", -(2**40))
        assert built.right == -(2**31)

    def test_integer_like_values_pass(self, sim: keel.Device) -> None:
        class Index:
            def __index__(self) -> int:
                return int(keel.Led.RGB2)

        sim.set_rgb(Index(), True)
        assert sim.rgb(keel.Led.RGB2) == 1

    def test_options_that_would_wrap_to_the_simulator_are_refused(self, natives: NativeCallController) -> None:
        with natives.record() as calls:
            with pytest.raises(OverflowError, match="backend = 4294967298"):
                keel.open(backend=2**32 + 2)
            with pytest.raises(OverflowError, match="flags = 4294967304"):
                keel.open(backend=keel.Backend.SIMULATOR, flags=2**32 + 8)
            with pytest.raises(OverflowError, match="command_timeout_ms = 4294967796"):
                keel.open(backend=keel.Backend.SIMULATOR, command_timeout_ms=2**32 + 500)
        assert calls == []

    def test_paths_must_fit(self, natives: NativeCallController) -> None:
        with natives.record() as calls:
            with pytest.raises(ValueError, match="gpiochip_path must encode to at most 127 bytes"):
                keel.open(backend=keel.Backend.R3, gpiochip_path="/dev/" + "g" * 123)
            with pytest.raises(ValueError, match="i2c_path must encode"):
                keel.open(backend=keel.Backend.R3, i2c_path="/dev/i2c\0-1")
            with pytest.raises(TypeError, match="i2c_path must be str or bytes"):
                keel.open(backend=keel.Backend.R3, i2c_path=1)  # type: ignore[arg-type]
        assert calls == []

    def test_a_path_is_passed_through(self, natives: NativeCallController) -> None:
        with pytest.raises(keel.SetupError) as caught:
            keel.open(backend=keel.Backend.SIMULATOR, gpiochip_path=b"/dev/gpiochip0")
        assert caught.value.status == keel.Status.E_INVALID_ARGUMENT

    def test_health_that_would_truncate_to_all_is_refused(
        self, sim: keel.Device, natives: NativeCallController
    ) -> None:
        with natives.record() as calls, pytest.raises(OverflowError):
            sim.tick(1, 2**32 + 7)
        assert calls == []
        assert sim.snapshot().last_cycle_id == 0

    @pytest.mark.parametrize("duty", [2**32 + 100, -(2**40)])
    def test_out_of_range_duties_are_refused_by_libkeel_and_coast(self, sim: keel.Device, duty: int) -> None:
        driver = CycleDriver(sim)
        driver.arm()
        driver.cycles(2, keel.CycleCommand.DRIVE, 300, 300)
        with pytest.raises(keel.InvalidArgumentError):
            sim.set_motors(duty, 0)
        snapshot = sim.snapshot()
        assert (snapshot.output_left, snapshot.command_left) == (0, 0)
        assert snapshot.state == keel.State.ARMED
        driver.cycles(2, keel.CycleCommand.DRIVE, 300, 300)
        with pytest.raises(keel.InvalidArgumentError) as caught:
            sim.cycle(driver.cycle_id + 1, keel.Health.ALL, command=keel.CycleCommand.DRIVE, left=0, right=duty)
        assert caught.value.snapshot is None
        assert sim.snapshot().output_right == 0

    @pytest.mark.parametrize(("item", "value"), [(keel.SimItem.BUTTONS, 2**32 + 1), (keel.SimItem.RANGE_MM, 2**31)])
    def test_simulator_values_are_range_checked(
        self, sim: keel.Device, natives: NativeCallController, item: keel.SimItem, value: int
    ) -> None:
        # Truncated, 2**32 + 1 would press button A, and saturated, 2**31 would be a valid distance: both are refused.
        with natives.record() as calls, pytest.raises(OverflowError, match=f"keel_sim_set: value = {value} is outside"):
            sim.sim_set(item, value)
        assert calls == []
        CycleDriver(sim).cycles(4)
        assert sim.snapshot().buttons_held == keel.Button(0)


def pattern(size: int) -> Any:
    buffer = (ctypes.c_ubyte * size)()
    ctypes.memset(buffer, 0xA5, size)
    return buffer


class TestStructSizes:
    @pytest.mark.parametrize("struct_size", [4, keel.limits.STRUCT_SIZE_MAX + 1])
    def test_a_bad_output_size_is_invalid_and_leaves_the_output_untouched(
        self, sim: keel.Device, natives: NativeCallController, struct_size: int
    ) -> None:
        buffer = pattern(keel.limits.STRUCT_SIZE_MAX + 8)
        ctypes.c_uint32.from_buffer(buffer).value = struct_size
        status = g.keel_get_snapshot(
            natives.library.natives, sim._state.token, ctypes.cast(buffer, ctypes.POINTER(g.SnapshotStruct))
        )
        assert type(error_for(status, "keel_get_snapshot")) is keel.InvalidArgumentError
        assert bytes(buffer)[4:] == b"\xa5" * (len(buffer) - 4)

    def test_an_oversize_input_with_a_zero_tail_is_accepted(
        self, sim: keel.Device, natives: NativeCallController
    ) -> None:
        status, last = self._cycle_with_tail(sim, natives, tail=b"\0" * 32)
        assert status == keel.Status.OK
        assert last == 1

    def test_an_oversize_input_with_a_tail_is_unsupported_without_a_tick(
        self, sim: keel.Device, natives: NativeCallController
    ) -> None:
        status, last = self._cycle_with_tail(sim, natives, tail=b"\0" * 31 + b"\1")
        assert type(error_for(status, "keel_cycle")) is keel.UnsupportedError
        assert last == 0

    def test_a_short_cycle_input_is_invalid_without_a_tick(
        self, sim: keel.Device, natives: NativeCallController
    ) -> None:
        cycle_input = g.fill_cycle_input(
            g.CycleInputStruct(), health=keel.Health.ALL, cycle_id=1, command=0, left=0, right=0, flags=0
        )
        cycle_input.struct_size = 4
        out = g.SnapshotStruct()
        ctypes.memset(ctypes.addressof(out) + 4, 0xA5, ctypes.sizeof(out) - 4)
        status = g.keel_cycle(natives.library.natives, sim._state.token, ctypes.byref(cycle_input), ctypes.byref(out))
        assert type(error_for(status, "keel_cycle")) is keel.InvalidArgumentError
        assert bytes(out)[4:] == b"\xa5" * (ctypes.sizeof(out) - 4)
        assert sim.snapshot().last_cycle_id == 0

    def test_an_oversize_options_tail_is_a_setup_error(self, natives: NativeCallController) -> None:
        size = ctypes.sizeof(g.OpenOptionsStruct) + 8
        buffer = (ctypes.c_ubyte * size)()
        options = g.OpenOptionsStruct.from_buffer(buffer)
        g.fill_open_options(
            options, backend=keel.Backend.SIMULATOR, flags=0, command_timeout_ms=0, gpiochip_path="", i2c_path=""
        )
        options.struct_size = size
        buffer[size - 1] = 1
        raw = ctypes.c_void_p()
        report = g.OpenReportStruct()
        status = g.keel_open(
            natives.library.natives,
            ctypes.cast(buffer, ctypes.POINTER(g.OpenOptionsStruct)),
            ctypes.byref(raw),
            ctypes.byref(report),
        )
        assert raw.value is None
        error = keel.SetupError(status, "keel_open", report=g.convert_open_report(report))
        assert error.status == keel.Status.E_INVALID_ARGUMENT
        assert error.report.resource == keel.OpenResource.OPTIONS

    def test_null_and_closed_handles(self, natives: NativeCallController) -> None:
        device = open_sim()
        token = device._state.token
        device.close()
        functions = natives.library.natives
        assert g.keel_close(functions, None, None) == keel.Status.OK
        assert g.keel_close(functions, token, None) == keel.Status.E_INVALID_HANDLE
        assert type(error_for(keel.Status.E_INVALID_HANDLE, "keel_close")) is keel.HandleError

    @staticmethod
    def _cycle_with_tail(sim: keel.Device, natives: NativeCallController, tail: bytes) -> tuple[int, int]:
        size = ctypes.sizeof(g.CycleInputStruct) + len(tail)
        buffer = (ctypes.c_ubyte * size)()
        cycle_input = g.CycleInputStruct.from_buffer(buffer)
        g.fill_cycle_input(cycle_input, health=keel.Health.ALL, cycle_id=1, command=0, left=0, right=0, flags=0)
        cycle_input.struct_size = size
        ctypes.memmove(ctypes.addressof(buffer) + ctypes.sizeof(g.CycleInputStruct), tail, len(tail))
        out = g.SnapshotStruct()
        out.state = 0xA5A5
        status = g.keel_cycle(
            natives.library.natives,
            sim._state.token,
            ctypes.cast(buffer, ctypes.POINTER(g.CycleInputStruct)),
            ctypes.byref(out),
        )
        assert out.state == keel.State.DISARMED  # written for every status but the three that leave it untouched
        return status, sim.snapshot().last_cycle_id
