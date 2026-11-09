#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Feed camera frames to vision code: brightness and motion for every frame, and edges with OpenCV.

Usage: camera_frames.py [--camera N] [--frames N] [--width W] [--height H] [--framerate FPS]

Each frame arrives in I420, so its Y plane is a greyscale image with no conversion. With numpy, Frame.plane(0) is a
view of that plane (no copy) for the brightness and the change from the previous frame; with OpenCV as well, the
example counts edge pixels with cv2.Canny, and Frame.array() with cv2.COLOR_YUV2BGR_I420 would give a colour image
(or open the camera with pixel_format=PixelFormat.BGR888). Without numpy the same numbers come from Frame.to_bytes().
A slow model never slows the camera: the queue keeps the newest frame and counts the ones it dropped. The camera
needs no Keel board. Exit status: 0 success, 1 failure, 2 usage, 130 after Ctrl-C.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from typing import Any

from keel import camera


def numpy_or_none() -> Any:
    """Return numpy if it is installed, else None."""
    if importlib.util.find_spec("numpy") is None:
        return None
    import numpy

    return numpy


def opencv_or_none() -> Any:
    """Return OpenCV (cv2) if it is installed, else None."""
    if importlib.util.find_spec("cv2") is None:
        return None
    import cv2

    return cv2


def main(argv: list[str] | None = None) -> int:
    """Read the frames and print what the vision code sees; return the exit status."""
    parser = argparse.ArgumentParser(description="Feed camera frames to vision code.")
    parser.add_argument("--camera", type=int, default=0, metavar="N", help="camera index (default 0)")
    parser.add_argument("--frames", type=int, default=60, metavar="N", help="frames to read (1..100000)")
    parser.add_argument("--width", type=int, default=640, metavar="W", help="frame width (default 640)")
    parser.add_argument("--height", type=int, default=480, metavar="H", help="frame height (default 480)")
    parser.add_argument("--framerate", type=float, default=30.0, metavar="FPS", help="frames per second (default 30)")
    args = parser.parse_args(argv)
    if not 1 <= args.frames <= 100_000:
        parser.error("--frames needs N in 1..100000")
    np, cv2 = numpy_or_none(), opencv_or_none()
    print(f"frames: numpy {'yes' if np else 'no'}, OpenCV {'yes' if cv2 else 'no'}", flush=True)
    try:
        with camera.open(args.camera, width=args.width, height=args.height, framerate=args.framerate) as cam:
            print(f"frames: {cam.info.model} ({cam.info.sensor})", flush=True)
            cam.start()
            previous = None
            for count, frame in enumerate(cam.frames(timeout=3), start=1):
                if np is not None:
                    grey = frame.plane(0)
                    brightness = float(grey.mean())
                    motion = 0.0 if previous is None else float(np.abs(grey.astype(np.int16) - previous).mean())
                    previous = grey.astype(np.int16)
                    edges = f", edges {np.count_nonzero(cv2.Canny(grey, 100, 200)) / grey.size:.1%}" if cv2 else ""
                else:
                    luma = frame.to_bytes()[: frame.width * frame.height]
                    brightness = sum(luma) / len(luma)
                    motion = 0.0 if previous is None else abs(brightness - previous)
                    previous, edges = brightness, ""
                if count % 10 == 0 or count == args.frames:
                    print(
                        f"frame {frame.sequence}: brightness {brightness:.1f}, motion {motion:.1f}{edges}", flush=True
                    )
                if count == args.frames:
                    break
            stats = cam.stats()
            print(f"frames: done, {stats.frames_read} read, {stats.frames_dropped} dropped", flush=True)
            return 0
    except camera.CameraError as error:
        print(f"frames: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
