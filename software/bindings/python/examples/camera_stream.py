#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Stream the camera to the network or a file, and report the receivers once a second.

Usage: camera_stream.py [DESTINATION] [--camera N] [--seconds S] [--mjpeg] [--width W] [--height H]

DESTINATION is anything Camera.start_output() takes; the default, tcp://:5000?listen, lets up to four viewers
connect and leave when they like:

    ffplay -f h264 -fflags nobuffer -flags low_delay -framedrop tcp://ROBOT:5000
    vlc tcp/h264://ROBOT:5000
    python3 -c "import cv2; c = cv2.VideoCapture('tcp://ROBOT:5000', cv2.CAP_FFMPEG); print(c.read()[1].shape)"

udp://COMPUTER:5000 sends to one computer (ffplay -f h264 udp://0.0.0.0:5000), tcp://COMPUTER:5000 connects to a
viewer that listens, and a path ending in .h264 or .mjpeg records. The library sends from its own threads: the
program only reports. Runs for S seconds (default: until Ctrl-C). The camera needs no Keel board. Exit status:
0 success, 1 failure, 2 usage, 130 after Ctrl-C.
"""

from __future__ import annotations

import argparse
import sys
import time

from keel import camera


def main(argv: list[str] | None = None) -> int:
    """Stream until the time is up or Ctrl-C; return the exit status."""
    parser = argparse.ArgumentParser(description="Stream the camera to the network or a file.")
    parser.add_argument("destination", nargs="?", default="tcp://:5000?listen", help="where to send the video")
    parser.add_argument("--camera", type=int, default=0, metavar="N", help="camera index (default 0)")
    parser.add_argument("--seconds", type=float, default=0.0, metavar="S", help="stop after S seconds (0: never)")
    parser.add_argument("--mjpeg", action="store_true", help="send MJPEG instead of H.264")
    parser.add_argument("--width", type=int, default=640, metavar="W", help="frame width (default 640)")
    parser.add_argument("--height", type=int, default=480, metavar="H", help="frame height (default 480)")
    args = parser.parse_args(argv)
    if args.seconds < 0:
        parser.error("--seconds needs S >= 0")
    codec = camera.Codec.MJPEG if args.mjpeg else camera.Codec.H264
    try:
        with camera.open(args.camera, width=args.width, height=args.height) as cam:
            cam.set_encoder(codec)
            output = cam.start_output(args.destination)
            cam.start()
            print(f"stream: {codec.name} {args.width}x{args.height} to {args.destination}", flush=True)
            deadline = time.monotonic() + args.seconds if args.seconds else None
            while deadline is None or time.monotonic() < deadline:
                time.sleep(1.0 if deadline is None else max(0.0, min(1.0, deadline - time.monotonic())))
                stats = cam.output_stats(output)
                print(
                    f"stream: {stats.state.name}, {stats.clients} receiver(s), {stats.chunks} chunks sent, "
                    f"{stats.chunks_dropped} dropped",
                    flush=True,
                )
                if stats.state == camera.OutputState.FAILED:
                    print(f"stream: {stats.message}", file=sys.stderr)
                    return 1
            cam.stop_output(output)
            print("stream: done", flush=True)
            return 0
    except camera.CameraError as error:
        print(f"stream: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("stream: stopped", flush=True)
        return 130


if __name__ == "__main__":
    sys.exit(main())
