#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Read bounded ultrasonic measurements from a sequential task without arming.

Only VALID measurements contain distance; NO_ECHO never means a clear path.
Usage: ranging.py [--simulator] [--count N]
"""

from __future__ import annotations

import argparse
import sys

import keel


def describe(result: keel.RangeMeasurement) -> str:
    """Describe a typed measurement without inventing distances for failed readings."""
    if result.status == keel.RangeStatus.VALID:
        return f"{result.distance_mm} mm (echo {result.echo_us} us, sequence {result.sequence})"
    if result.status == keel.RangeStatus.NO_ECHO:
        return f"nothing detected (sequence {result.sequence})"
    return f"{result.status.name} (sequence {result.sequence})"


class RangingTask(keel.KeelTask):
    """Read a requested number of distances through the task facade."""

    def __init__(self, count: int) -> None:
        """Store the positive number of readings requested by the caller."""
        self.count = count

    def run(self) -> None:
        """Measure without a control callback or manual sensor-power polling."""
        with keel.stop_on_signals(self.keel):
            if keel.Capability.SIMULATED in self.keel.capabilities():
                self.keel.sim_set(keel.SimItem.RANGE_MM, 500)
            for _ in range(self.count):
                print(f"range: {describe(self.keel.measure_distance())}", flush=True)


def main(argv: list[str] | None = None) -> int:
    """Parse options and run a no-motion task; return an exit code."""
    parser = argparse.ArgumentParser(description="Measure distances with the ultrasonic sensor.")
    parser.add_argument("--simulator", action="store_true", help="use the simulator")
    parser.add_argument("--count", type=int, default=5, metavar="N", help="number of readings (1..100)")
    args = parser.parse_args(argv)
    if not 1 <= args.count <= 100:
        parser.error("--count needs N in 1..100")
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    try:
        keel.run(RangingTask(args.count), keel.TaskOptions(board_kwargs={"backend": backend}))
        return 0
    except KeyboardInterrupt:
        print("emergency stop")
        return 130
    except (keel.KeelError, keel.TaskError, TimeoutError) as error:
        print(f"ranging: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
