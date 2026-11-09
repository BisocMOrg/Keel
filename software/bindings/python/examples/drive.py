#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Drive forward with one bounded task action, then close the robot.

Usage: drive.py [--simulator] [--allow-motion]
The first physical run should keep the wheels clear. The task owns arming,
timing and cleanup; no application hardware control loop is needed.
"""

from __future__ import annotations

import argparse
import sys

import keel


def motion_allowed(capabilities: keel.Capability, allow_motion: bool) -> bool:
    """Return whether explicit operator permission or a simulator permits motion."""
    return keel.Capability.SIMULATED in capabilities or allow_motion


class DriveTask(keel.KeelTask):
    """Drive both motors at 20 percent for two seconds."""

    def run(self) -> None:
        """Let the bounded action arm, drive and confirm zero output before returning."""
        with keel.stop_on_signals(self.keel):
            print("arming automatically; driving forward at 20 percent", flush=True)
            self.keel.drive_for(20, 20, 2000)
            print("bounded drive complete; motor output is zero", flush=True)


def main(argv: list[str] | None = None) -> int:
    """Parse explicit motion permission and execute the task; return an exit code."""
    parser = argparse.ArgumentParser(description="Drive forward at 20%% for two seconds.")
    parser.add_argument("--simulator", action="store_true", help="use the real-time simulator")
    parser.add_argument("--allow-motion", action="store_true", help="permit movement on a real robot")
    args = parser.parse_args(argv)
    if not args.simulator and not args.allow_motion:
        print("drive: motion needs --allow-motion on a real robot", file=sys.stderr)
        return 2
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    try:
        keel.run(DriveTask(), keel.TaskOptions(board_kwargs={"backend": backend}))
        print("task closed; robot disarmed")
        return 0
    except KeyboardInterrupt:
        print("emergency stop")
        return 130
    except (keel.KeelError, keel.TaskError) as error:
        print(f"drive: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
