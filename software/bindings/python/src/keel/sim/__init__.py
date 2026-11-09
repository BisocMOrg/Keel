# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The simulator's stimuli and ground truth: Device.sim_set() takes a SimItem, Device.sim_state() returns a SimState.

Both raise UnsupportedError on hardware. The keel package re-exports every name here.
"""

from ._generated import SimFlag, SimItem, SimState

__all__ = [
    "SimFlag",
    "SimItem",
    "SimState",
]
