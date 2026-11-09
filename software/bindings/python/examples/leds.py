#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Walk the RGB LEDs and the button indicator LEDs; show an error colour whenever the device is FAULTED.

Usage: leds.py [--simulator] [--hold MS]

The LEDs work in every state, FAULTED included, so an application can always show what happened: after the walk an
emergency stop latches FAULTED and the error colour is shown, then the LEDs go off. Without the RGB
LEDs (Capability.RGB absent, for example with no LED driver on the I2C bus) only the button LEDs are walked. The
robot never moves. Exit status: 0 success, 1 failure, 2 usage, 130 after Ctrl-C, 128 + N after another stop
signal N.
"""

from __future__ import annotations

import argparse
import sys

import keel

RAINBOW = [0x400000, 0x402000, 0x404000, 0x004000, 0x000040, 0x200040]
ERROR_RED = 0xFF0000
PERIOD_MS = keel.limits.TICK_PERIOD_MS


def show_rgb(device: keel.Device, hold_cycles: int, loop: keel.ControlLoop) -> None:
    """Show one colour on all LEDs, then a rainbow frame, holding each for hold_cycles cycles."""
    for color in (0x400000, 0x004000, 0x000040):
        device.set_rgb_all(color)
        print(f"rgb: all LEDs {color:06x}", flush=True)
        loop.run(cycles=hold_cycles)
    device.set_rgb_frame(RAINBOW)
    print("rgb: rainbow frame " + " ".join(f"{device.rgb(led):06x}" for led in keel.Led), flush=True)
    loop.run(cycles=hold_cycles)


def show_buttons(device: keel.Device, hold_cycles: int, loop: keel.ControlLoop) -> None:
    """Light each button LED in turn, then all of them, then none."""
    for button in (keel.Button.A, keel.Button.B, keel.Button.X, keel.Button.Y, keel.Button.ALL):
        device.set_button_leds(keel.Button.ALL, button)
        print(f"button LEDs: {button.name}", flush=True)
        loop.run(cycles=hold_cycles)
    device.set_button_leds(keel.Button.ALL, 0)


def main(argv: list[str] | None = None) -> int:
    """Run the walk; return the exit status."""
    parser = argparse.ArgumentParser(description="Walk the RGB and button LEDs.")
    parser.add_argument("--simulator", action="store_true", help="use the simulator instead of the robot")
    parser.add_argument("--hold", type=int, default=300, metavar="MS", help="time each pattern is shown (50..5000)")
    args = parser.parse_args(argv)
    if not 50 <= args.hold <= 5000:
        parser.error("--hold needs MS in 50..5000")
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    hold_cycles = args.hold // PERIOD_MS
    try:
        with keel.open(backend=backend) as device, keel.stop_on_signals(device):
            has_rgb = keel.Capability.RGB in device.capabilities()

            def on_fault(fault: keel.Fault) -> None:
                # Called on the loop thread when the device enters FAULTED: the LEDs still work.
                print(f"fault: {fault.status.name}, cause {fault.cause.name}; showing the error colour", flush=True)
                if has_rgb:
                    device.set_rgb_all(ERROR_RED)

            loop = keel.ControlLoop(device, on_fault=on_fault)
            try:
                show_rgb(device, hold_cycles, loop)
            except keel.UnsupportedError:
                # Capability.RGB is absent, for example with no LED driver on the I2C bus.
                print("rgb: unavailable on this device; walking the button LEDs only", flush=True)
            show_buttons(device, hold_cycles, loop)
            print("leds: emergency stop, to show that the LEDs work while FAULTED", flush=True)
            device.emergency_stop()
            loop.run(cycles=hold_cycles)  # the loop reports the latch, and on_fault shows the error colour
            if has_rgb:
                device.set_rgb_all(0)
            print("leds: done")
            return 0
    except KeyboardInterrupt:
        print("emergency stop")
        return 130
    except keel.KeelError as error:
        print(f"leds: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
