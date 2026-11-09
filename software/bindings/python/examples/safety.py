#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Demonstrate explicit stop and fault recovery through RobotSession.

Usage: safety.py [--simulator] [--allow-motion]
A user-interface thread emergency-stops the robot. Recovery is explicit and
never re-arms automatically. No movement command is issued; the final arming
refusal check requires simulator or --allow-motion permission.
"""

from __future__ import annotations

import argparse
import sys
import threading

import keel


def motion_allowed(capabilities: keel.Capability, allow_motion: bool) -> bool:
    """Return whether the optional arming diagnostic is explicitly permitted."""
    return keel.Capability.SIMULATED in capabilities or allow_motion


def main(argv: list[str] | None = None) -> int:
    """Run a stop/recovery diagnostic without application hardware-loop code."""
    parser = argparse.ArgumentParser(description="Walk through explicit session stop and recovery without driving.")
    parser.add_argument("--simulator", action="store_true", help="use the simulator")
    parser.add_argument("--allow-motion", action="store_true", help="allow the arming refusal check on hardware")
    args = parser.parse_args(argv)
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    try:
        with (
            keel.RobotSession(keel.TaskOptions(board_kwargs={"backend": backend})) as robot,
            keel.stop_on_signals(robot),
        ):
            print("session owns the hardware control loop", flush=True)
            stopper = threading.Thread(target=robot.emergency_stop, name="user-interface")
            stopper.start()
            stopper.join()
            fault = robot.fault()
            print(f"fault: {fault.status.name}, cause {fault.cause.name}, state {fault.state.name}", flush=True)
            robot.clear_fault()
            print("fault cleared explicitly; robot remains disarmed", flush=True)
            if motion_allowed(robot.capabilities(), args.allow_motion):
                try:
                    robot.request_arm().result(timeout=3)
                    print("safety: arming was accepted during the hold-off", file=sys.stderr)
                    return 1
                except keel.StateError as error:
                    print(f"re-arm refused during the {keel.limits.REARM_HOLDOFF_MS} ms hold-off: {error.name}")
            else:
                print("re-arm hold-off step skipped: it needs --allow-motion")
            return 0
    except KeyboardInterrupt:
        print("emergency stop")
        return 130
    except (keel.KeelError, keel.TaskError, TimeoutError) as error:
        print(f"safety: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
