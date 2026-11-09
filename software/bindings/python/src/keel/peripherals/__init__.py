# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The peripherals besides the motors: buttons and their LEDs, the RGB LEDs, the ultrasonic sensor and its power.

Device.take_buttons() returns a ButtonState, Device.range_poll() and Device.range_measure() a RangeMeasurement, and
snapshots report the buttons and the SensorPower inference. The servo port has no types of its own: its limits are
in keel.limits and its presence is Capability.SERVO. The keel package re-exports every name here.
"""

from ._generated import Button, ButtonState, Led, RangeMeasurement, RangeStatus, SensorPower

__all__ = [
    "Button",
    "ButtonState",
    "Led",
    "RangeMeasurement",
    "RangeStatus",
    "SensorPower",
]
