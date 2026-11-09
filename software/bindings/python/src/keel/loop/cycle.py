# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Cycle: the step's view of one cycle of a ControlLoop, and the motor command the step hands back."""

from __future__ import annotations

import operator
from typing import SupportsIndex

from keel import limits
from keel.device import Device
from keel.errors import InvalidArgumentError
from keel.errors.error_factory import refused
from keel.state import Snapshot

from ._generated import CycleCommand

__all__ = ["Cycle"]


class Cycle:
    """The step's view of one cycle; one object is reused, and it is valid only inside the step.

    Calls made through device from the step run on the loop thread, which is the control owner.
    """

    __slots__ = ("_command", "_left", "_refused", "_right", "cycle_id", "device", "io_ok", "now_us", "snapshot")

    snapshot: Snapshot
    """The snapshot of the previous keel_cycle (before the first: Device.snapshot())."""

    cycle_id: int
    """The cycle_id this cycle's keel_cycle will carry."""

    now_us: int
    """The device clock at the start of the step, in microseconds."""

    device: Device
    """The device; calls from the step run on the loop thread, the control owner."""

    io_ok: bool
    """Set it to False when the application's own I/O failed this cycle; the tick then reports it (IO_OK)."""

    def __init__(self, device: Device, snapshot: Snapshot) -> None:
        """Prepare the reusable cycle object of a loop."""
        self.device = device
        self.snapshot = snapshot
        self.cycle_id = 0
        self.now_us = 0
        self.io_ok = True
        self._command: int | None = None
        self._left = 0
        self._right = 0
        self._refused = False

    def _reset(self, snapshot: Snapshot, cycle_id: int, now_us: int) -> None:
        self.snapshot = snapshot
        self.cycle_id = cycle_id
        self.now_us = now_us
        self.io_ok = True
        self._command = None
        self._left = self._right = 0
        self._refused = False

    def drive(self, left: SupportsIndex, right: SupportsIndex) -> None:
        """Drive at left and right permille this cycle; send it again every cycle while it should stay in force.

        A duty outside -limits.DUTY_MAX..limits.DUTY_MAX (or a non-integer) is refused: this raises
        InvalidArgumentError (or TypeError) and the cycle fails its health check even if the step catches the
        exception, so the tick fail-stops (HEALTH_FLAGS without NO_EXCEPTION), a latched Category-0 stop that
        ControlLoop.clear_fault() recovers from.
        """
        try:
            duties = operator.index(left), operator.index(right)
        except TypeError:
            self._refuse()
            raise TypeError(
                f"Cycle.drive: duties must be integers, not {type(left).__name__} and {type(right).__name__}; "
                "this cycle fails its health check"
            ) from None
        if not all(-limits.DUTY_MAX <= duty <= limits.DUTY_MAX for duty in duties):
            self._refuse()
            raise refused(
                InvalidArgumentError,
                "keel_cycle",
                f"Cycle.drive{duties} is outside -{limits.DUTY_MAX}..{limits.DUTY_MAX}, so this cycle fails its "
                "health check",
            )
        self._command = CycleCommand.DRIVE
        self._left, self._right = duties

    def brake(self) -> None:
        """Brake both motors this cycle (BRAKE)."""
        self._command = CycleCommand.BRAKE
        self._left = self._right = 0

    def coast(self) -> None:
        """Coast both motors now: an explicit DRIVE (0, 0) this cycle."""
        self._command = CycleCommand.DRIVE
        self._left = self._right = 0

    def _refuse(self) -> None:
        self._refused = True
        self._command = None
