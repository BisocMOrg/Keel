#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Measure distances with the ultrasonic sensor: start and poll from the loop, measure from another thread.

Usage: ranging.py [--simulator] [--count N]

The control loop triggers a measurement with range_start() and collects it with range_poll(), which never block, so
the loop keeps its period. A second thread then uses the blocking range_measure(), which is allowed from any thread
but the control owner while armed. Only RangeStatus.VALID carries a distance: NO_ECHO means nothing was detected,
never that the path is clear. Sensor power needs a running heartbeat, not arming; the robot never moves. On the
simulator an object is placed 500 mm away. Exit status: 0 success, 1 failure, 2 usage, 130 after Ctrl-C,
128 + N after another stop signal N.
"""

from __future__ import annotations

import argparse
import sys
import threading
import time

import keel

TRIGGER_CYCLES = 10  # one measurement every 100 ms, well above the 60 ms trigger spacing
SIMULATED_DISTANCE_MM = 500


def describe(result: keel.RangeMeasurement) -> str:
    """One line for a measurement; a distance only for VALID."""
    if result.status == keel.RangeStatus.VALID:
        return f"{result.distance_mm} mm (echo {result.echo_us} us, sequence {result.sequence})"
    if result.status == keel.RangeStatus.NO_ECHO:
        return f"nothing detected (sequence {result.sequence})"
    return f"{result.status.name} (sequence {result.sequence})"


def main(argv: list[str] | None = None) -> int:
    """Run the measurements; return the exit status."""
    parser = argparse.ArgumentParser(description="Measure distances with the ultrasonic sensor.")
    parser.add_argument("--simulator", action="store_true", help="use the simulator instead of the robot")
    parser.add_argument("--count", type=int, default=5, metavar="N", help="measurements from the loop (1..100)")
    args = parser.parse_args(argv)
    if not 1 <= args.count <= 100:
        parser.error("--count needs N in 1..100")
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    try:
        with keel.open(backend=backend) as device, keel.stop_on_signals(device):
            if keel.Capability.RANGE not in device.capabilities():
                print("ranging: this device has no ultrasonic sensor", file=sys.stderr)
                return 1
            if keel.Capability.SIMULATED in device.capabilities():
                device.sim_set(keel.SimItem.RANGE_MM, SIMULATED_DISTANCE_MM)
            readings: list[keel.RangeMeasurement] = []
            done = threading.Event()

            def step(cycle: keel.Cycle) -> None:
                if cycle.cycle_id % TRIGGER_CYCLES == 1:
                    try:
                        cycle.device.range_start()
                    except keel.AgainError:
                        return  # a measurement is still pending; poll it below
                result = cycle.device.range_poll()
                if result.status not in (keel.RangeStatus.IDLE, keel.RangeStatus.PENDING) and (
                    not readings or result.sequence != readings[-1].sequence
                ):
                    readings.append(result)
                    print(f"loop: {describe(result)}", flush=True)
                    if len(readings) >= args.count:
                        done.set()

            with keel.ControlLoop(device, step):
                if not done.wait(timeout=10 + args.count):
                    print("ranging: no measurement completed", file=sys.stderr)
                    return 1
                for _ in range(5):
                    try:
                        print(f"thread: {describe(device.range_measure())}", flush=True)
                        break
                    except keel.AgainError:
                        time.sleep(0.07)  # the loop's trigger is too recent; the spacing is 60 ms
            return 0
    except KeyboardInterrupt:
        print("emergency stop")
        return 130
    except keel.KeelError as error:
        print(f"ranging: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
