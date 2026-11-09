#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Drive forward at 20% for about two seconds, then disarm: the quick start of the libkeel README, in Python.

Usage: drive.py [--simulator] [--allow-motion]

On a robot, the wheels turn: put the robot on a stand the first time. The robot arms only on the simulator or with
--allow-motion. Ctrl-C or another stop signal makes an emergency stop. Exit status: 0 success, 1 failure, 2 usage
(including motion refused), 130 after Ctrl-C, 128 + N after another stop signal N (143 after SIGTERM).
"""

from __future__ import annotations

import argparse
import sys

import keel

DUTY = 200  # permille: 20% forward
DRIVE_CYCLES = 200  # 2 s at 10 ms per cycle
ARM_CYCLES = 300  # arming normally takes about 0.7 s


def motion_allowed(capabilities: keel.Capability, allow_motion: bool) -> bool:
    """Return True when this program may arm: on the simulator, or when the operator passed --allow-motion."""
    return keel.Capability.SIMULATED in capabilities or allow_motion


def step(cycle: keel.Cycle) -> None:
    """Drive while ARMED; the command is sent again every cycle, so it never outlives the loop."""
    if cycle.snapshot.state == keel.State.ARMED:
        cycle.drive(DUTY, DUTY)


def main(argv: list[str] | None = None) -> int:
    """Run the program; return the exit status."""
    parser = argparse.ArgumentParser(description="Drive forward at 20%% for about two seconds, then disarm.")
    parser.add_argument("--simulator", action="store_true", help="use the simulator instead of the robot")
    parser.add_argument("--allow-motion", action="store_true", help="arm and drive a real robot")
    args = parser.parse_args(argv)
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    try:
        with keel.open(backend=backend) as device, keel.stop_on_signals(device):
            if not motion_allowed(device.capabilities(), args.allow_motion):
                print("drive: motion needs --allow-motion on a real robot", file=sys.stderr)
                return 2
            loop = keel.ControlLoop(device, step)
            device.request_arm()  # before the first tick every thread is the owner
            print("arming")
            snapshot = loop.run(until=lambda s: s.state == keel.State.ARMED, cycles=ARM_CYCLES)
            if snapshot.state != keel.State.ARMED:
                print(f"drive: not armed ({snapshot.state.name}); fault: {device.fault()}", file=sys.stderr)
                return 1
            print(f"armed; driving forward at {DUTY} permille")
            loop.run(cycles=DRIVE_CYCLES)
            device.disarm()
            print(f"disarmed after {loop.stats().cycles} cycles (state {device.state().name})")
            return 0
    except KeyboardInterrupt:
        print("emergency stop")
        return 130
    except keel.KeelError as error:
        print(f"drive: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
