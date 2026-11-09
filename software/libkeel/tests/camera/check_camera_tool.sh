#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# keel-camera against the synthetic camera (KEEL_CAMERA_TEST_SOURCE=1): its
# usage text and exit statuses, list, still, record, stream (file, TCP, UDP
# and standard output), raw frames, a clean stop on SIGINT, and the exit
# status of every failure class.
#
# Usage: check_camera_tool.sh <keel-camera> <case>
# Environment: PYTHON (python3).
set -u

[ "$#" -eq 2 ] || { echo "usage: $0 <keel-camera> <case>" >&2; exit 2; }
tool=$1
case_name=$2
PYTHON=${PYTHON:-python3}
KEEL_CAMERA_TEST_SOURCE=1
export KEEL_CAMERA_TEST_SOURCE
unset KEEL_CAMERA_TEST_FAULT
fault=""
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

fail() {
    echo "FAIL  $case_name: $*"
    exit 1
}

# expect STATUS COMMAND...: runs the tool (with the test fault $fault, which
# it then clears), keeps its output in $work/out and
# $work/err, and fails unless it exits with STATUS.
expect() {
    wanted=$1
    shift
    if [ -n "$fault" ]; then
        KEEL_CAMERA_TEST_FAULT=$fault "$tool" "$@" >"$work/out" 2>"$work/err"
    else
        "$tool" "$@" >"$work/out" 2>"$work/err"
    fi
    got=$?
    fault=""
    if [ "$got" -ne "$wanted" ]; then
        sed 's/^/      /' "$work/err"
        fail "keel-camera $* exited $got, expected $wanted"
    fi
}

# First bytes of a file in hex.
head_hex() {
    od -A n -t x1 -N "$2" "$1" | tr -d ' \n'
}

free_port() {
    "$PYTHON" -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

case $case_name in
usage)
    expect 0 --help
    grep -q '^Exit status:' "$work/out" || fail "--help lists no exit statuses"
    grep -q 'keel-setup --user NAME' "$work/out" || fail "--help does not name the video-group remedy"
    expect 0 --version
    grep -q '^keel-camera 1\.0\.0 (' "$work/out" || fail "--version printed: $(cat "$work/out")"
    expect 2
    expect 2 fly
    expect 2 record
    expect 2 stream
    expect 2 list extra
    expect 2 record -o x.h264 --width 641
    expect 2 record -o x.h264 --framerate 0
    expect 2 record -o x.h264 --rotation 90
    expect 2 record -o x.h264 --codec vp8
    expect 2 record -o x.h264 --codec mjpeg --bitrate 1000000
    expect 2 record -o x.h264 --lens-position 2
    expect 2 still -o x.jpg -W 640
    expect 2 --bogus
    ;;
list)
    expect 0 list
    grep -q '^0: Camera Module 2 (imx219), fixed focus$' "$work/out" || fail "list printed: $(cat "$work/out")"
    grep -q '^1: Camera Module 3 (imx708), autofocus$' "$work/out" || fail "Camera Module 3 is missing"
    grep -q '640x480 8-bit, up to 200.16 fps' "$work/out" || fail "the modes are missing"
    expect 0 list --json
    "$PYTHON" - "$work/out" <<'EOF' || fail "list --json is not the expected JSON"
import json, sys
cameras = json.load(open(sys.argv[1]))["cameras"]
assert [c["model"] for c in cameras] == ["Camera Module 2", "Camera Module 3"], cameras
assert [c["autofocus"] for c in cameras] == [False, True]
assert cameras[1]["modes"][2] == {"width": 4608, "height": 2592, "bit_depth": 10, "max_fps": 14.35,
                                  "crop": [0, 0, 4608, 2592]}, cameras[1]["modes"]
EOF
    fault=no-camera; expect 0 list
    grep -q '^No camera detected' "$work/out" || fail "an empty list printed: $(cat "$work/out")"
    grep -q 'camera_auto_detect=1' "$work/out" || fail "an empty list names no boot setting: $(cat "$work/out")"
    ;;
still)
    expect 0 still -W 640 -H 480 -q 90 -o "$work/a.jpg"
    [ "$(head_hex "$work/a.jpg" 2)" = ffd8 ] || fail "the still is not a JPEG image"
    grep -q '640x480 still' "$work/err" || fail "still reported: $(cat "$work/err")"
    expect 0 still -c 1 --autofocus auto -W 1280 -H 720 -o "$work/b.jpg"
    if "$tool" --version | grep -q jpeg; then
        expect 0 still -o "$work/full.jpg"
        grep -q '3280x2464 still.*(libjpeg)' "$work/err" || fail "a full-resolution still reported: $(cat "$work/err")"
    else
        expect 6 still -o "$work/full.jpg"
    fi
    expect 2 still
    expect 3 still -o /nonexistent-directory/x.jpg
    ;;
record)
    expect 0 record -t 1 -o "$work/a.h264"
    [ "$(head_hex "$work/a.h264" 5)" = 0000000167 ] || fail "the recording does not start with an SPS"
    grep -q 'chunks, .* bytes written, 0 dropped' "$work/err" || fail "record reported: $(cat "$work/err")"
    expect 0 record --codec mjpeg -q 50 -t 1 -o "$work/a.mjpeg"
    [ "$(head_hex "$work/a.mjpeg" 2)" = ffd8 ] || fail "the MJPEG recording does not start with an image"
    expect 0 record --bitrate 2000000 --intra 15 --profile baseline -W 320 -H 240 -f 15 --hflip --vflip \
        --exposure-us 10000 --gain 2.5 --ev -0.5 --awb-gains 1.6,1.9 -t 1 -o "$work/b.h264"
    expect 2 record -t 1 -o "$work/a.avi"
    if ! "$tool" --version | grep -q libav; then
        expect 6 record -t 1 -o "$work/a.mp4"
        grep -q 'libavformat' "$work/err" || fail "the .mp4 refusal does not name libavformat"
    fi
    ;;
stream_tcp)
    port=$(free_port)
    "$PYTHON" - "$port" "$work/received" <<'EOF' &
import socket, sys
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", int(sys.argv[1])))
server.listen(1)
server.settimeout(10)
connection, _ = server.accept()
with open(sys.argv[2], "wb") as out:
    while True:
        data = connection.recv(65536)
        if not data:
            break
        out.write(data)
EOF
    receiver=$!
    sleep 0.5
    expect 0 stream "tcp://127.0.0.1:$port" -t 1
    wait "$receiver" || fail "the receiver failed"
    [ "$(head_hex "$work/received" 5)" = 0000000167 ] || fail "the receiver did not get the stream"
    expect 3 stream "tcp://127.0.0.1:$port" -t 1
    grep -q 'refused the connection' "$work/err" || fail "a refused connection reported: $(cat "$work/err")"
    ;;
stream_udp)
    port=$(free_port)
    "$PYTHON" - "$port" "$work/received" <<'EOF' &
import socket, sys
receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
receiver.bind(("127.0.0.1", int(sys.argv[1])))
receiver.settimeout(5)
with open(sys.argv[2], "wb") as out:
    out.write(receiver.recv(65536))
EOF
    receiver=$!
    sleep 0.5
    expect 0 stream "udp://127.0.0.1:$port" --codec mjpeg -t 1
    wait "$receiver" || fail "the receiver got nothing"
    [ "$(head_hex "$work/received" 2)" = ffd8 ] || fail "the datagram does not start an image"
    ;;
stream_stdout)
    "$tool" stream - -t 1 >"$work/stdout.h264" 2>"$work/err" || fail "stream - failed: $(cat "$work/err")"
    [ "$(head_hex "$work/stdout.h264" 5)" = 0000000167 ] || fail "standard output does not carry the stream"
    # A reader that stops early ends the stream cleanly.
    status=$("$PYTHON" - "$tool" <<'EOF'
import subprocess, sys
p = subprocess.Popen([sys.argv[1], "stream", "-"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
p.stdout.read(200)
p.stdout.close()
print(p.wait(timeout=10))
EOF
)
    [ "$status" = 0 ] || fail "a closed pipe ended the stream with status $status"
    ;;
frames)
    "$tool" frames --format rgb888 -n 3 >"$work/rgb" 2>"$work/err" || fail "frames failed: $(cat "$work/err")"
    [ "$(wc -c <"$work/rgb")" -eq $((3 * 640 * 480 * 3)) ] || fail "3 RGB frames are $(wc -c <"$work/rgb") bytes"
    grep -q 'pixel_format rgb24 -video_size 640x480' "$work/err" || fail "frames printed: $(cat "$work/err")"
    # Rows are written without padding: 100 pixels in a 128-byte stride.
    "$tool" frames -W 100 -H 64 -n 2 >"$work/i420" 2>/dev/null || fail "I420 frames failed"
    [ "$(wc -c <"$work/i420")" -eq $((2 * 100 * 64 * 3 / 2)) ] || fail "2 I420 frames are $(wc -c <"$work/i420") bytes"
    "$PYTHON" - "$work/i420" <<'EOF' || fail "the I420 frames do not carry the synthetic pattern"
import sys
data = open(sys.argv[1], "rb").read()
y = data[: 100 * 64]
n = None
for sequence in range(256):
    if all(y[r * 100 + c] == (7 * c + 3 * r + sequence) & 0xFF for r in range(64) for c in range(100)):
        n = sequence
assert n is not None
EOF
    ;;
interrupt)
    "$tool" record -o "$work/a.h264" >/dev/null 2>"$work/err" &
    recorder=$!
    sleep 1
    kill -INT "$recorder"
    wait "$recorder"
    status=$?
    [ "$status" -eq 0 ] || fail "SIGINT ended record with status $status"
    [ -s "$work/a.h264" ] || fail "the recording is empty"
    ;;
failures)
    fault=no-camera; expect 3 still -o "$work/x.jpg"
    grep -q 'ribbon cable' "$work/err" || fail "no camera reported: $(cat "$work/err")"
    fault=busy; expect 4 record -t 1 -o "$work/x.h264"
    fault=permission; expect 5 list
    grep -q 'video group' "$work/err" || fail "permission reported: $(cat "$work/err")"
    fault=ipa-missing; expect 3 list
    expect 6 record --autofocus continuous -t 1 -o "$work/x.h264"
    grep -q 'fixed-focus' "$work/err" || fail "autofocus reported: $(cat "$work/err")"
    fault=encoder-missing; expect 3 record -t 1 -o "$work/x.h264"
    fault=capture-fail; expect 1 record -t 2 -o "$work/x.h264"
    grep -q 'stopped delivering' "$work/err" || fail "a capture failure reported: $(cat "$work/err")"
    fault=capture-fail; expect 1 frames -n 10
    expect 3 record -c 5 -t 1 -o "$work/x.h264"
    ;;
*)
    echo "unknown case $case_name" >&2
    exit 2
    ;;
esac
echo "PASS  $case_name"
