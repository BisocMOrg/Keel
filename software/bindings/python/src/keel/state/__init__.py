# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The safety state of a device: the coarse state, the arming handshake, the safety inputs, snapshots and faults.

Device.state() returns a State, Device.snapshot() and every cycle a Snapshot, Device.fault() the Fault record, and
Device.read_safety_inputs() the SafetyInput bits. The keel package re-exports every name here.
"""

from ._generated import AckPhase, Fault, FaultCause, FaultFlag, SafetyInput, Snapshot, SnapshotFlag, State

__all__ = [
    "AckPhase",
    "Fault",
    "FaultCause",
    "FaultFlag",
    "SafetyInput",
    "Snapshot",
    "SnapshotFlag",
    "State",
]
