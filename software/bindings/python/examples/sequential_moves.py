#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Run a student-style sequence of bounded moves without managing a control loop.

Usage: sequential_moves.py --simulator | --allow-motion
Motion is refused on real hardware unless --allow-motion is explicit. The first
hardware run should have the wheels clear. No OpenCV or target tracking is built
into KeelTask: students implement their own behaviours using these primitives.
"""

from __future__ import annotations

import argparse
import sys
import time

import keel


class SequentialMoves(keel.KeelTask):
    """A complete student program: move, pause, turn, then finish."""

    def run(self) -> None:
        """Execute the sequence; each movement returns with zero motor drive."""
        self.keel.drive_for(20, 20, 500)
        time.sleep(0.5)
        self.keel.turn_left_for(300)
        self.keel.drive_for(15, 15, 300)


def main(argv: list[str] | None = None) -> int:
    """Parse explicit motion permission and run the task; return an exit code."""
    parser = argparse.ArgumentParser(description="Run a bounded sequential Keel task.")
    parser.add_argument("--simulator", action="store_true", help="use the real-time simulator")
    parser.add_argument("--allow-motion", action="store_true", help="permit motion on the real robot")
    args = parser.parse_args(argv)
    if not args.simulator and not args.allow_motion:
        print("sequential_moves: pass --simulator or --allow-motion", file=sys.stderr)
        return 2
    backend = keel.Backend.SIMULATOR if args.simulator else keel.Backend.AUTO
    options = keel.TaskOptions(board_kwargs={"backend": backend})
    try:
        keel.run(SequentialMoves(), options)
        return 0
    except KeyboardInterrupt:
        return 130
    except (keel.TaskError, keel.KeelError) as error:
        print(f"sequential_moves: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
