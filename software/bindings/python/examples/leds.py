#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Walk RGB and button LEDs through RobotSession, including a fault indication.

Usage: leds.py [--simulator] [--hold MS]
No motor is armed. The session owns the hardware loop while application code
uses ordinary timed pauses and peripheral methods.
"""

from __future__ import annotations

import argparse
import sys
import time

import keel

RAINBOW = [0x400000, 0x402000, 0x404000, 0x004000, 0x000040, 0x200040]
ERROR_RED = 0xFF0000


def show_rgb(robot: keel.RobotSession, hold_ms: int) -> None:
    """Show solid colours and a six-LED rainbow, holding each for hold_ms."""
    for color in (0x400000, 0x004000, 0x000040):
        robot.set_rgb_all(color)
        print(f"rgb: all LEDs {color:06x}", flush=True)
        time.sleep(hold_ms / 1000)
    robot.set_rgb_frame(RAINBOW)
    print("rgb: rainbow frame " + " ".join(f"{robot.rgb(led):06x}" for led in keel.Led), flush=True)
    time.sleep(hold_ms / 1000)


def show_buttons(robot: keel.RobotSession, hold_ms: int) -> None:
    """Light each button LED and then all of them for hold_ms each."""
    for button in (keel.Button.A, keel.Button.B, keel.Button.X, keel.Button.Y, keel.Button.ALL):
        robot.set_button_leds(keel.Button.ALL, button)
        print(f"button LEDs: {button.name}", flush=True)
        time.sleep(hold_ms / 1000)
    robot.set_button_leds(keel.Button.ALL, 0)


def main(argv: list[str] | None = None) -> int:
    """Run the no-motion LED diagnostic and close its session."""
    parser = argparse.ArgumentParser(description="Walk the RGB and button LEDs.")
    parser.add_argument("--simulator", action="store_true", help="use the simulator")
    parser.add_argument("--hold", type=int, default=300, metavar="MS", help="time each pattern is shown (50..5000)")
    args = parser.parse_args(argv)
    if not 50 <= args.hold <= 5000:
        parser.error("--hold needs MS in 50..5000")
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    try:
        with (
            keel.RobotSession(keel.TaskOptions(board_kwargs={"backend": backend})) as robot,
            keel.stop_on_signals(robot),
        ):
            has_rgb = keel.Capability.RGB in robot.capabilities()
            try:
                show_rgb(robot, args.hold)
            except keel.UnsupportedError:
                print("rgb: unavailable on this device; walking the button LEDs only")
            show_buttons(robot, args.hold)
            robot.emergency_stop()
            fault = robot.fault()
            print(f"fault: {fault.status.name}, cause {fault.cause.name}; showing the error colour", flush=True)
            if has_rgb:
                robot.set_rgb_all(ERROR_RED)
                time.sleep(args.hold / 1000)
                robot.set_rgb_all(0)
            print("leds: done")
            return 0
    except KeyboardInterrupt:
        print("emergency stop")
        return 130
    except (keel.KeelError, keel.TaskError) as error:
        print(f"leds: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
