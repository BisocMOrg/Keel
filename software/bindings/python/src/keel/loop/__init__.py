# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""ControlLoop: the control loop of libkeel's binding rules, one keel_cycle per period on one thread.

Each cycle runs the queued go requests and submitted functions, then the application's step, then one keel_cycle
that carries the tick, the motor command and honest health, then sleeps on the device clock until the next period.
Besides keel_cycle and keel_sleep_until, a cycle reads the device clock twice to measure the step budget: four
foreign calls in all, each of which releases the GIL.
Health is Health.ALL only when the step returned normally within its budget, reported healthy I/O and handed over
only valid duties; anything else fail-stops the device on purpose (cause HEALTH_FLAGS, detail = the missing bits).

The loop never arms, never clears a fault and never re-sends a command on the application's behalf. One loop serves
a device for the device's whole life: the thread that ticks becomes the control owner, and only that thread can
later clear a fault. After a fault the loop keeps running in faulted mode, so the application can show an error,
read buttons and clear the fault through the loop.

The package also holds the cycle's own types: Cycle (the step's view), LoopStats (the loop's counters), and the
CycleCommand, CycleFlag and Health values of keel_cycle_input. The keel package re-exports every name here.
"""

from ._generated import CycleCommand, CycleFlag, Health
from .control_loop import ControlLoop
from .cycle import Cycle
from .loop_stats import LoopStats

__all__ = [
    "ControlLoop",
    "Cycle",
    "CycleCommand",
    "CycleFlag",
    "Health",
    "LoopStats",
]
