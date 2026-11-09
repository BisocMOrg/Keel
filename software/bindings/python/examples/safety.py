#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Walk through the safety contract without moving the robot.

Usage: safety.py [--simulator] [--allow-motion]

The steps:
1. a go call from a thread that is not the control owner is refused (ConflictError);
2. a user-interface thread makes an emergency stop; the loop reports the fault record (USER_STOP);
3. the fault is cleared through the loop, on the owner thread;
4. a request to arm within 1 s of the fault is refused by the re-arm hold-off (StateError). This step calls
   request_arm, so it runs only on the simulator or with --allow-motion, and is skipped otherwise.

Exit status: 0 success, 1 failure, 2 usage, 130 after Ctrl-C, 128 + N after another stop signal N.
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from collections.abc import Callable

import keel


def motion_allowed(capabilities: keel.Capability, allow_motion: bool) -> bool:
    """Return True when this program may request arming: on the simulator, or with --allow-motion."""
    return keel.Capability.SIMULATED in capabilities or allow_motion


def wait_until(loop: keel.ControlLoop, what: str, condition: Callable[[keel.Snapshot], bool]) -> None:
    """Wait up to 5 s for the loop's published snapshot to satisfy condition."""
    deadline = time.monotonic() + 5.0
    while not condition(loop.snapshot):
        if time.monotonic() > deadline:
            raise TimeoutError(f"timed out waiting for {what}")
        time.sleep(0.01)


def main(argv: list[str] | None = None) -> int:
    """Run the walk-through; return the exit status."""
    parser = argparse.ArgumentParser(description="Walk through the safety contract without moving the robot.")
    parser.add_argument("--simulator", action="store_true", help="use the simulator instead of the robot")
    parser.add_argument("--allow-motion", action="store_true", help="allow the re-arm hold-off step on a real robot")
    args = parser.parse_args(argv)
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    try:
        with keel.open(backend=backend) as device, keel.stop_on_signals(device):
            faults: list[keel.Fault] = []
            loop = keel.ControlLoop(device, on_fault=faults.append)
            with loop:
                wait_until(loop, "the first tick", lambda s: s.owner_thread_id != 0)

                try:
                    device.brake()  # a go call from this thread, which is not the control owner
                    print("safety: a foreign go call was accepted", file=sys.stderr)
                    return 1
                except keel.ConflictError as error:
                    print(f"foreign go call refused: {error.name}", flush=True)

                stopper = threading.Thread(target=device.emergency_stop, name="user-interface")
                stopper.start()
                stopper.join()
                wait_until(loop, "the fault", lambda s: s.state == keel.State.FAULTED)
                fault = loop.fault or device.fault()
                print(f"fault: {fault.status.name}, cause {fault.cause.name}, state {fault.state.name}", flush=True)

                loop.clear_fault().result(timeout=5)
                print(f"fault cleared: state {device.state().name}", flush=True)

                if motion_allowed(device.capabilities(), args.allow_motion):
                    try:
                        loop.request_arm().result(timeout=5)
                        print("safety: arming was accepted during the hold-off", file=sys.stderr)
                        return 1
                    except keel.StateError as error:
                        print(f"re-arm refused during the {keel.limits.REARM_HOLDOFF_MS} ms hold-off: {error.name}")
                else:
                    print("re-arm hold-off step skipped: it calls request_arm, which needs --allow-motion")
            return 0
    except KeyboardInterrupt:
        print("emergency stop")
        return 130
    except (keel.KeelError, TimeoutError) as error:
        print(f"safety: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
