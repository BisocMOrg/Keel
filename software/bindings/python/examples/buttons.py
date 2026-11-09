#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Mirror buttons on their LEDs using an interactive RobotSession.

Usage: buttons.py [--drive LEFT RIGHT MS] [--duration MS] [--simulator]
The application polls UI state; RobotSession alone owns the hardware loop.
LEFT and RIGHT retain the example's legacy permille CLI units. Button B or a
stop signal ends the example. Without --drive this program never arms.
"""

from __future__ import annotations

import argparse
import sys
import time

import keel

IDLE_BLUE = 0x000008


def main(argv: list[str] | None = None) -> int:
    """Run button mirroring and an optional bounded drive; return an exit status."""
    parser = argparse.ArgumentParser(description="Mirror the buttons on their LEDs; button B ends the program.")
    parser.add_argument(
        "--drive",
        nargs=3,
        type=int,
        metavar=("LEFT", "RIGHT", "MS"),
        help="explicitly arm and drive at -1000..1000 permille for 1..2000 ms",
    )
    parser.add_argument("--duration", type=int, metavar="MS", help="end after 1..3600000 milliseconds")
    parser.add_argument("--simulator", action="store_true", help="use the real-time simulator")
    args = parser.parse_args(argv)
    if args.drive is not None:
        left, right, duration = args.drive
        if not (-1000 <= left <= 1000 and -1000 <= right <= 1000 and 1 <= duration <= 2000):
            parser.error("--drive needs LEFT and RIGHT in -1000..1000 and MS in 1..2000")
    if args.duration is not None and not 1 <= args.duration <= 3_600_000:
        parser.error("--duration needs MS in 1..3600000")
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    try:
        with (
            keel.RobotSession(keel.TaskOptions(board_kwargs={"backend": backend})) as robot,
            keel.stop_on_signals(robot),
        ):
            try:
                robot.set_rgb_all(IDLE_BLUE)
            except (keel.UnsupportedError, keel.PeripheralError):
                print("RGB LEDs unavailable; continuing without them")
            started = time.monotonic()
            end = started + args.duration / 1000 if args.duration else float("inf")
            if args.drive:
                print("state: ARMING", flush=True)
                robot.arm()
                print("state: ARMED", flush=True)
                left, right, duration = args.drive
                robot.refresh_drive(left / 10, right / 10, duration)
                end = min(end, time.monotonic() + duration / 1000)
            while time.monotonic() < end:
                snapshot = robot.snapshot()
                pressed, released = robot.buttons_pressed(), robot.buttons_released()
                if pressed or released:
                    print(
                        f"buttons: held={snapshot.buttons_held:x} pressed={pressed:x} released={released:x}",
                        flush=True,
                    )
                if keel.Button.B in (snapshot.buttons_held | pressed):
                    break
                if snapshot.button_leds != snapshot.buttons_held:
                    robot.set_button_leds(keel.Button.ALL, snapshot.buttons_held)
                time.sleep(0.02)
            robot.disarm()
            print("state: DISARMED" if args.drive else "buttons: done")
            return 0
    except KeyboardInterrupt:
        print("emergency stop (signal 2)")
        return 130
    except (keel.KeelError, keel.TaskError) as error:
        print(f"buttons: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
