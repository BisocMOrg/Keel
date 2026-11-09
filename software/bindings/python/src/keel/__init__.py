# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""keel: the Python binding of libkeel, the host library of the Keel robot baseboard.

A minimal program opens the device, starts a control loop and arms:

    import keel

    def step(cycle: keel.Cycle) -> None:
        if cycle.snapshot.state == keel.State.ARMED:
            cycle.drive(200, 200)              # permille, sent again every cycle

    with keel.open() as device:                # the board, or KEEL_BACKEND=simulator
        loop = keel.ControlLoop(device, step)
        device.request_arm()                   # before the first tick every thread is the owner
        loop.run(cycles=300)                   # 3 s on the device clock, on this thread
        device.disarm()

The binding loads the system's libkeel.so.1 (or the absolute path in KEEL_LIBRARY) on first use; importing keel
never touches the library. libkeel is the only safety logic: every call passes through unchanged, and every status
other than OK raises the exception of its class (keel.errors).

The package keeps the entry points and the open options and report at its root: open() and Device (keel.device),
load() and the library metadata (keel.library), stop_on_signals() (keel.stop_signals), and the constant modules
keel.abi and keel.limits. The other types are grouped by what they describe, and keel re-exports them all:

| Package | Contents |
|---|---|
| keel.errors | the exception hierarchy, Status and StatusClass |
| keel.loop | ControlLoop, Cycle, LoopStats and the cycle's CycleCommand, CycleFlag and Health |
| keel.state | State, AckPhase, SafetyInput, Snapshot, Fault and their flags and causes |
| keel.peripherals | buttons and their LEDs, the RGB LEDs, ranging and the sensor power |
| keel.sim | the simulator's stimuli and ground truth |
| keel.camera | the Raspberry Pi camera, through its own library libkeel-camera.so.1 (not re-exported here) |
"""

import logging

# isort: off
# This is the import order of the whole package, because every import of keel.X runs this file first. errors comes
# first: every other module raises its exceptions. loop comes before device: device imports keel.loop._generated,
# and the modules of keel.loop import keel.device. camera comes last: it uses only errors, and none of the others.
from . import errors
from .errors import (
    AgainError,
    ConflictError,
    HandleError,
    InvalidArgumentError,
    KeelError,
    LibraryError,
    PeripheralError,
    RejectedError,
    SafetyFaultError,
    SetupError,
    StateError,
    Status,
    StatusClass,
    UnsupportedError,
)
from ._generated import Backend, Capability, OpenFlag, OpenReport, OpenResource, OpenWarning, TimingClass
from . import peripherals, sim, state
from .peripherals import Button, ButtonState, Led, RangeMeasurement, RangeStatus, SensorPower
from .sim import SimFlag, SimItem, SimState
from .state import AckPhase, Fault, FaultCause, FaultFlag, SafetyInput, Snapshot, SnapshotFlag, State
from . import loop
from .loop import ControlLoop, Cycle, CycleCommand, CycleFlag, Health, LoopStats
from . import abi, limits
from .library import abi_version, fault_cause_name, load, status_class, status_name, status_string, version_string
from .device import Device, open  # noqa: A004  (keel.open, after keel_open)
from .stop_signals import stop_on_signals
from . import camera

# isort: on

__version__ = "1.0.0"
"""The binding's version; it equals the version of the keel.h it was generated from."""

logging.getLogger(__name__).addHandler(logging.NullHandler())

__all__ = [
    "AckPhase",
    "AgainError",
    "Backend",
    "Button",
    "ButtonState",
    "Capability",
    "ConflictError",
    "ControlLoop",
    "Cycle",
    "CycleCommand",
    "CycleFlag",
    "Device",
    "Fault",
    "FaultCause",
    "FaultFlag",
    "HandleError",
    "Health",
    "InvalidArgumentError",
    "KeelError",
    "Led",
    "LibraryError",
    "LoopStats",
    "OpenFlag",
    "OpenReport",
    "OpenResource",
    "OpenWarning",
    "PeripheralError",
    "RangeMeasurement",
    "RangeStatus",
    "RejectedError",
    "SafetyFaultError",
    "SafetyInput",
    "SensorPower",
    "SetupError",
    "SimFlag",
    "SimItem",
    "SimState",
    "Snapshot",
    "SnapshotFlag",
    "State",
    "StateError",
    "Status",
    "StatusClass",
    "TimingClass",
    "UnsupportedError",
    "__version__",
    "abi",
    "abi_version",
    "camera",
    "errors",
    "fault_cause_name",
    "limits",
    "load",
    "loop",
    "open",
    "peripherals",
    "sim",
    "state",
    "status_class",
    "status_name",
    "status_string",
    "stop_on_signals",
    "version_string",
]
