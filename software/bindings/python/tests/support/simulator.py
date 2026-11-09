# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Simulator devices and CycleDriver, which drives a device by hand one keel_cycle at a time.

Every helper that opens a device opens the simulator explicitly (Backend.SIMULATOR), never AUTO, so no test can
reach a robot's board by accident.
"""

from __future__ import annotations

import keel

PERIOD_US = keel.limits.TICK_PERIOD_MS * 1000


def open_sim(flags: int = 0, *, virtual: bool = True, **options: object) -> keel.Device:
    """Open a simulator device, on the virtual clock unless virtual is False."""
    clock = keel.OpenFlag.VIRTUAL_CLOCK if virtual else keel.OpenFlag(0)
    return keel.open(backend=keel.Backend.SIMULATOR, flags=clock | flags, **options)  # type: ignore[arg-type]


class CycleDriver:
    """Drives a device by hand, one keel_cycle and one period of device time per cycle, from the calling thread."""

    def __init__(self, device: keel.Device) -> None:
        self.device = device
        self.cycle_id = device.snapshot().last_cycle_id
        self.next_us = device.now_us()

    def cycle(
        self,
        command: int = keel.CycleCommand.NONE,
        left: int = 0,
        right: int = 0,
        *,
        health: int = keel.Health.ALL,
        take_buttons: bool = False,
    ) -> keel.Snapshot:
        self.cycle_id += 1
        snapshot = self.device.cycle(
            self.cycle_id, health, command=command, left=left, right=right, take_buttons=take_buttons
        )
        self.sleep()
        return snapshot

    def sleep(self) -> None:
        self.next_us = max(self.next_us + PERIOD_US, self.device.now_us())
        self.device.sleep_until(self.next_us)

    def cycles(self, count: int, command: int = keel.CycleCommand.NONE, left: int = 0, right: int = 0) -> keel.Snapshot:
        snapshot = self.device.snapshot()
        for _ in range(count):
            snapshot = self.cycle(command, left, right)
        return snapshot

    def arm(self) -> keel.Snapshot:
        self.device.request_arm()
        for _ in range(300):
            snapshot = self.cycle()
            if snapshot.state == keel.State.ARMED:
                return snapshot
        raise AssertionError("the simulator did not arm within 3 s")

    def wait_ms(self, milliseconds: int) -> keel.Snapshot:
        return self.cycles(milliseconds // keel.limits.TICK_PERIOD_MS)
