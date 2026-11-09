# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""LoopStats: the counters of a ControlLoop, as ControlLoop.stats() copies them."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass

from keel.errors import Status

__all__ = ["LoopStats"]


@dataclass(frozen=True, slots=True)
class LoopStats:
    """Counters of a ControlLoop, copied when stats() is called."""

    cycles: int
    """keel_cycle calls made."""

    overruns: int
    """Cycles that missed their period, after which the deadline was resynchronised (no catch-up burst)."""

    max_gap_us: int
    """The largest interval between two keel_cycle calls, on the device clock."""

    max_step_us: int
    """The longest application work of one cycle (queued requests, submitted functions and the step)."""

    rejected: Mapping[Status, int]
    """REJECTED results of keel_cycle by status, for example E_STATE for a drive command while ARMING."""

    step_errors: int
    """Exceptions raised by the step."""

    faults: int
    """Fault latches the loop reported (each rise of the snapshot's fault_count it saw)."""
