#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Mirror the buttons on their indicator LEDs and print button edges; optionally arm and drive.

Usage: buttons.py [--drive LEFT RIGHT MS] [--duration MS] [--simulator]

Button B ends the program normally; Ctrl-C ends it with an emergency stop, as stop buttons in applications
should. Without --drive the robot never moves. With --drive LEFT RIGHT MS the program arms, drives at LEFT/RIGHT
permille for MS milliseconds once ARMED, then disarms and ends; --drive is itself the operator's decision to move
the robot.
Exit status: 0 success, 1 failure, 2 usage, 130 after Ctrl-C, 128 + N after another stop signal N (143 after
SIGTERM).
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from dataclasses import dataclass

import keel

IDLE_BLUE = 0x000008  # dim blue: the program is running
ARM_LIMIT_US = 5_000_000  # arming normally takes about 0.7 s


@dataclass
class Plan:
    """What the program was asked to do, and how far it got."""

    drive: tuple[int, int, int] | None
    duration_ms: int | None
    started_us: int | None = None
    driving_since_us: int | None = None
    last_state: keel.State = keel.State.DISARMED
    outcome: int | None = None


def make_step(plan: Plan, loop_stop: list[keel.ControlLoop]) -> Callable[[keel.Cycle], None]:
    """Return the loop's step for plan; loop_stop holds the loop once it exists."""

    def finish(outcome: int) -> None:
        plan.outcome = outcome
        loop_stop[0].stop()

    def step(cycle: keel.Cycle) -> None:
        snapshot = cycle.snapshot
        if plan.started_us is None:
            plan.started_us = cycle.now_us
        if snapshot.state != plan.last_state:
            print(f"state: {snapshot.state.name}", flush=True)
            plan.last_state = snapshot.state
        if snapshot.buttons_pressed or snapshot.buttons_released:
            print(
                f"buttons: held={snapshot.buttons_held:x} pressed={snapshot.buttons_pressed:x} "
                f"released={snapshot.buttons_released:x}",
                flush=True,
            )
        if keel.Button.B in snapshot.buttons_held:
            finish(0)
            return
        if snapshot.button_leds != snapshot.buttons_held:
            cycle.device.set_button_leds(keel.Button.ALL, snapshot.buttons_held)
        if plan.drive is not None:
            left, right, milliseconds = plan.drive
            if snapshot.state == keel.State.ARMED:
                if plan.driving_since_us is None:
                    plan.driving_since_us = cycle.now_us
                if cycle.now_us - plan.driving_since_us >= milliseconds * 1000:
                    finish(0)
                    return
                cycle.drive(left, right)
            elif plan.driving_since_us is None and cycle.now_us - plan.started_us > ARM_LIMIT_US:
                print("buttons: arming did not complete within 5 s", file=sys.stderr)
                finish(1)
                return
        if plan.duration_ms is not None and cycle.now_us - plan.started_us >= plan.duration_ms * 1000:
            finish(0)

    return step


def main(argv: list[str] | None = None) -> int:
    """Run the example; return the exit status."""
    parser = argparse.ArgumentParser(description="Mirror the buttons on their LEDs; button B ends the program.")
    parser.add_argument(
        "--drive",
        nargs=3,
        type=int,
        metavar=("LEFT", "RIGHT", "MS"),
        help="arm, drive at LEFT/RIGHT permille (-1000..1000) for MS ms (1..2000), then disarm and end",
    )
    parser.add_argument("--duration", type=int, metavar="MS", help="end after MS ms (1..3600000)")
    parser.add_argument("--simulator", action="store_true", help="use the simulator instead of the robot")
    args = parser.parse_args(argv)
    if args.drive is not None:
        left, right, milliseconds = args.drive
        limit = keel.limits.DUTY_MAX
        if not (-limit <= left <= limit and -limit <= right <= limit and 1 <= milliseconds <= 2000):
            parser.error("--drive needs LEFT and RIGHT in -1000..1000 and MS in 1..2000")
    if args.duration is not None and not 1 <= args.duration <= 3_600_000:
        parser.error("--duration needs MS in 1..3600000")

    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    plan = Plan(drive=tuple(args.drive) if args.drive else None, duration_ms=args.duration)
    try:
        with keel.open(backend=backend) as device, keel.stop_on_signals(device):
            try:
                device.set_rgb_all(IDLE_BLUE)
            except (keel.UnsupportedError, keel.PeripheralError):
                print("RGB LEDs unavailable; continuing without them")
            holder: list[keel.ControlLoop] = []
            loop = keel.ControlLoop(device, make_step(plan, holder))
            holder.append(loop)
            if plan.drive is not None:
                device.request_arm()  # only with --drive, the operator's decision to move
            loop.run()
            print("state: DISARMED" if plan.last_state in (keel.State.ARMING, keel.State.ARMED) else "buttons: done")
            return plan.outcome or 0
    except KeyboardInterrupt:
        print("emergency stop (signal 2)")
        return 130
    except keel.KeelError as error:
        print(f"buttons: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
