#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Take one JPEG photo, by default at the sensor's full resolution.

Usage: camera_still.py [-o FILE] [--camera N] [--width W --height H] [--quality Q]

Exposure, white balance and, on Camera Module 3, focus settle first, so a photo takes about a second. Up to 1920x1920
the Raspberry Pi's JPEG encoder does the work; larger photos (the full 3280x2464 of Camera Module 2) are encoded with
libjpeg. The camera needs no Keel board. Exit status: 0 success, 1 failure, 2 usage, 130 after Ctrl-C.
"""

from __future__ import annotations

import argparse
import sys

from keel import camera


def main(argv: list[str] | None = None) -> int:
    """Take the photo and write it; return the exit status."""
    parser = argparse.ArgumentParser(description="Take one JPEG photo.")
    parser.add_argument("-o", "--output", default="photo.jpg", metavar="FILE", help="the JPEG file (photo.jpg)")
    parser.add_argument("--camera", type=int, default=0, metavar="N", help="camera index (default 0)")
    parser.add_argument("--width", type=int, default=0, metavar="W", help="width (default: full resolution)")
    parser.add_argument("--height", type=int, default=0, metavar="H", help="height (default: full resolution)")
    parser.add_argument("--quality", type=int, default=0, metavar="Q", help="JPEG quality 1..100 (default 85)")
    args = parser.parse_args(argv)
    if (args.width == 0) != (args.height == 0):
        parser.error("give both --width and --height, or neither")
    try:
        with camera.open(args.camera) as cam:
            info = cam.capture_still_file(args.output, width=args.width, height=args.height, quality=args.quality)
            encoder = "libjpeg" if info.software else "the hardware encoder"
            print(f"still: {info.width}x{info.height}, {info.bytes} bytes, by {encoder}, in {args.output}")
            return 0
    except camera.CameraError as error:
        print(f"still: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
