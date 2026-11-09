#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Portable smoke checks for either Keel demonstrator server (stdlib only).

The default checks are read-only, including when connected to real hardware.
--exercise-mock is refused unless every command observes mode == 'mock'.
Tokens are read from a file and are never printed or placed in URLs.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from html.parser import HTMLParser
import json
import math
from pathlib import Path
import secrets
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


class CheckFailed(RuntimeError):
    """A server response did not meet the shared contract."""


class ApiFailure(CheckFailed):
    def __init__(self, status: int, message: str):
        self.status = status
        super().__init__(f"HTTP {status}: {message}")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailed(message)


def finite(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


@dataclass
class Response:
    status: int
    headers: object
    body: bytes

    def json(self):
        try:
            return json.loads(self.body)
        except (UnicodeError, ValueError) as error:
            raise CheckFailed("Response is not valid JSON") from error


class LocalAssets(HTMLParser):
    def __init__(self):
        super().__init__()
        self.scripts = []
        self.styles = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "script" and attrs.get("src"):
            self.scripts.append(attrs["src"])
        if tag == "link" and attrs.get("rel") == "stylesheet" and attrs.get("href"):
            self.styles.append(attrs["href"])


class Client:
    def __init__(self, url: str, token: str, timeout: float):
        parsed = urllib.parse.urlsplit(url)
        check(parsed.scheme in ("http", "https") and parsed.hostname, "Use an HTTP(S) server URL")
        check(not parsed.username and not parsed.password and not parsed.query and not parsed.fragment,
              "URL must not contain credentials, a query string or a fragment")
        self.url = url.rstrip("/")
        self.origin = (parsed.scheme, parsed.hostname, parsed.port)
        self.token, self.timeout = token, timeout
        self.client_id = "api-smoke-" + secrets.token_hex(8)
        self.seq = 0
        self.seq_lock = threading.Lock()

    def safe_url(self, path: str) -> str:
        url = urllib.parse.urljoin(self.url + "/", path)
        parsed = urllib.parse.urlsplit(url)
        check((parsed.scheme, parsed.hostname, parsed.port) == self.origin,
              "Refusing an API/static resource URL on another origin")
        check(not parsed.username and not parsed.password, "Resource URL contains credentials")
        return url

    def request(self, path: str, body=None, *, auth=True) -> Response:
        headers = {"Accept": "*/*"}
        if auth:
            headers["Authorization"] = "Bearer " + self.token
        encoded = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            encoded = json.dumps(body, allow_nan=False).encode("utf-8")
        request = urllib.request.Request(self.safe_url(path), data=encoded, headers=headers)
        try:
            response = urllib.request.urlopen(request, timeout=self.timeout)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read(24 * 1024 * 1024 + 1)
            check(len(data) <= 24 * 1024 * 1024, "Response exceeds the smoke client's 24 MiB bound")
            return Response(response.status, response.headers, data)

    def json(self, path: str):
        response = self.request(path)
        if response.status != 200:
            self.fail_response(response)
        return response.json()

    def fail_response(self, response: Response):
        try:
            value = response.json()
            message = str(value.get("error", "Unexpected response")) if isinstance(value, dict) else "Unexpected response"
        except CheckFailed:
            message = "Unexpected response"
        raise ApiFailure(response.status, message.replace(self.token, "[redacted]"))

    def state(self):
        return self.json("/api/state")

    def mock_guard(self):
        check(self.state().get("mode") == "mock",
              "Refusing mutation: --exercise-mock only permits a server reporting mode='mock'")

    def command_response(self, kind: str, *, sequence=None, **fields):
        # Guard each individual command, including background heartbeats and cleanup.
        self.mock_guard()
        with self.seq_lock:
            if sequence is None:
                self.seq += 1
                sequence = self.seq
        return self.request("/api/command", dict(type=kind, clientId=self.client_id, seq=sequence, **fields))

    def command(self, kind: str, **fields):
        # Heartbeats and a slow photo response can overlap. A genuinely reordered
        # HTTP request may be rejected safely; retry with a fresh sequence.
        for attempt in range(3):
            response = self.command_response(kind, **fields)
            if response.status == 200:
                value = response.json()
                check(value.get("ok") is True, f"{kind}: missing ok=true")
                return value
            error = str(response.json().get("error", ""))
            if response.status != 409 or "stale" not in error.lower() or attempt == 2:
                self.fail_response(response)
        raise CheckFailed("Unreachable command retry state")


def wait_state(client: Client, predicate, description: str, timeout=4.0):
    deadline = time.monotonic() + timeout
    while True:
        state = client.state()
        if predicate(state):
            return state
        if time.monotonic() >= deadline:
            raise CheckFailed("Timed out waiting for " + description)
        time.sleep(.04)


def jpeg_dimensions(data: bytes):
    check(data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9", "JPEG start/end markers missing")
    index = 2
    while index + 4 <= len(data):
        if data[index] != 0xff:
            index += 1
            continue
        while index < len(data) and data[index] == 0xff:
            index += 1
        check(index < len(data), "Truncated JPEG marker")
        marker = data[index]
        index += 1
        if marker in (0xd8, 0xd9) or 0xd0 <= marker <= 0xd7:
            continue
        check(index + 2 <= len(data), "Truncated JPEG segment")
        size = int.from_bytes(data[index:index+2], "big")
        check(size >= 2 and index + size <= len(data), "Invalid JPEG segment length")
        if marker in (0xc0, 0xc1, 0xc2):
            check(size >= 8, "Invalid JPEG frame header")
            height = int.from_bytes(data[index+3:index+5], "big")
            width = int.from_bytes(data[index+5:index+7], "big")
            check(width > 0 and height > 0, "JPEG has invalid dimensions")
            return width, height
        if marker == 0xda:
            break
        index += size
    raise CheckFailed("JPEG has no supported SOF dimension marker")


def readonly_checks(client: Client):
    state = client.state()
    check(state.get("language") in ("Java", "Python"), "Unrecognised language")
    check(state.get("mode") in ("hardware", "simulator", "mock"), "Unrecognised operating mode")
    check(finite(state.get("serverTimeMs")), "State has no finite server clock")
    for name in ("controller", "robot", "loop", "camera", "system"):
        check(isinstance(state.get(name), dict), f"Missing {name} object")
    rgb = state["robot"].get("rgb")
    check(isinstance(rgb, list) and len(rgb) == 6 and all(isinstance(c, int) and 0 <= c <= 0xffffff for c in rgb),
          "State must expose six RGB values")
    check(client.request("/api/state", auth=False).status in (401, 403), "API accepted an unauthenticated state request")

    samples = []
    for _ in range(4):
        before = time.monotonic_ns() / 1e6
        ping = client.json("/api/ping")
        after = time.monotonic_ns() / 1e6
        receive, send = ping.get("serverReceiveMs"), ping.get("serverSendMs")
        check(finite(receive) and finite(send) and send >= receive, "Invalid server ping clocks")
        rtt = (after-before) - (send-receive)
        check(rtt >= -.5, "Server ping duration exceeds client round trip")
        check(abs(send-state["serverTimeMs"]) < 60000, "State and ping clocks use different origins")
        samples.append(max(0, rtt))

    page = client.request("/", auth=False)
    check(page.status == 200 and "text/html" in page.headers.get("Content-Type", ""), "Dashboard HTML is unavailable")
    assets = LocalAssets()
    assets.feed(page.body.decode("utf-8"))
    check(assets.scripts and assets.styles, "Dashboard lacks a script or stylesheet")
    for path in assets.scripts + assets.styles:
        resource = client.request(path, auth=False)
        check(resource.status == 200 and resource.body and "text/html" not in resource.headers.get("Content-Type", ""),
              "Dashboard script/stylesheet is missing or resolves to HTML")

    model = client.request("/assets/robot.glb", auth=False)
    check(model.status == 200 and model.body[:4] == b"glTF", "Robot GLB is missing")
    check(len(model.body) >= 20, "Robot GLB is truncated")
    version, length, json_length, chunk_type = struct.unpack_from("<IIII", model.body, 4)
    check(version == 2 and length == len(model.body) and chunk_type == 0x4e4f534a, "Invalid GLB header")
    gltf = json.loads(model.body[20:20+json_length])
    names = {node.get("name") for node in gltf.get("nodes", [])}
    required = {"WheelLeft", "WheelRight", "CameraModule", "RangeSensor", "Baseboard"}
    required.update(f"RGB{i}_Emitter" for i in range(1, 7))
    required.update(f"SW{i}" for i in range(1, 5))
    check(required <= names, "Robot model is missing interactive groups")
    check(all("uri" not in image for image in gltf.get("images", [])), "GLB textures are not self-contained")
    print(f"READ_ONLY_PASS {state['language']} / {state['mode']}; clock RTT {min(samples):.1f} ms; "
          f"dashboard assets and {len(model.body):,}-byte interactive GLB verified")
    return state


def wait_frame(client: Client, after: int, timeout=8.0):
    deadline = time.monotonic() + timeout
    while True:
        response = client.request(f"/api/camera/frame?after={after}")
        if response.status == 200:
            check("image/jpeg" in response.headers.get("Content-Type", ""), "Camera frame is not image/jpeg")
            jpeg_dimensions(response.body)
            sequence = int(response.headers.get("X-Sequence", "-1"))
            check(sequence > after, "Camera returned an old frame")
            captured = float(response.headers.get("X-Captured-Ms", "nan"))
            sent = float(response.headers.get("X-Sent-Ms", "nan"))
            age = float(response.headers.get("X-Capture-Age-Ms", "nan"))
            check(all(map(math.isfinite, (captured, sent, age))) and 0 <= age < 8000,
                  "Invalid frame timing headers")
            check(abs((sent-captured)-age) < 5, "Frame age does not match timestamp headers")
            check(response.headers.get("X-Timestamp-Basis") == "server-receipt",
                  "Mock frame must not claim a physical sensor timestamp")
            check("no-store" in response.headers.get("Cache-Control", ""), "Frames must not be cached")
            return sequence, response
        check(response.status == 204, f"Unexpected camera frame status: {response.status}")
        if time.monotonic() >= deadline:
            raise CheckFailed("Timed out waiting for a fresh camera JPEG")
        time.sleep(.04)


def exercise_mock(client: Client):
    client.mock_guard()
    claimed = False
    done = threading.Event()
    heartbeat_errors = []
    worker = None

    def heartbeats():
        while not done.wait(.25):
            try:
                client.command("heartbeat")
            except Exception as error:
                heartbeat_errors.append(type(error).__name__)

    try:
        client.command("claim")
        claimed = True
        worker = threading.Thread(target=heartbeats, name="api-smoke-heartbeat", daemon=True)
        worker.start()
        client.command("stop")
        client.command("clearFault")
        client.command("arm")
        wait_state(client, lambda s: s["robot"]["state"] == "ARMED", "mock arming")
        client.command("drive", left=200, right=-150)
        wait_state(client, lambda s: s["robot"]["left"] == 200 and s["robot"]["right"] == -150,
                   "mock differential drive", timeout=.3)
        started = time.monotonic()
        stopped = wait_state(client, lambda s: s["robot"]["state"] == "DISARMED" and
                             s["robot"]["left"] == 0 and s["robot"]["right"] == 0,
                             "drive deadman while heartbeats continue", timeout=1.2)
        check(time.monotonic()-started < stopped["driveTimeoutMs"]/1000 + .65,
              "Drive timeout exceeded its tolerance")
        old_drive = client.command_response("drive", sequence=1, left=200, right=200)
        check(old_drive.status == 409, "A stale drive request was accepted")
        client.command("arm")
        wait_state(client, lambda s: s["robot"]["state"] == "ARMED", "second mock arming")
        # A newer heartbeat must not prevent a reordered emergency stop.
        stale_stop = client.command_response("estop", sequence=1)
        check(stale_stop.status == 200, "A stale-sequence emergency stop was rejected")
        wait_state(client, lambda s: s["robot"]["state"] == "FAULTED" and not s["robot"]["motorEnabled"],
                   "latched mock emergency stop")
        client.command("clearFault")
        wait_state(client, lambda s: s["robot"]["state"] == "DISARMED", "fault clear without rearming")
        print("MOCK_MOTION_PASS arm, differential drive, deadman, stale-drive rejection, reordered estop and fault clear")

        client.command("rgb", index=-1, color=0x102030)
        client.command("buttonLeds", mask=5)
        client.command("range")
        wait_state(client, lambda s: s["robot"]["rgb"] == [0x102030]*6 and s["robot"]["buttonLeds"] == 5,
                   "mock LED state round trip")
        client.command("cameraStart")
        first, frame = wait_frame(client, 0)
        second, _ = wait_frame(client, first)
        photo = client.command("photo")
        path = photo.get("photo", "")
        check(isinstance(path, str) and path.startswith("/api/photos/"), "Invalid photo download URL")
        still = client.request(path)
        check(still.status == 200 and "image/jpeg" in still.headers.get("Content-Type", ""), "Photo download failed")
        dimensions = jpeg_dimensions(still.body)
        check("attachment" in still.headers.get("Content-Disposition", ""), "Photo is missing an attachment filename")
        resumed, _ = wait_frame(client, second)
        wait_state(client, lambda s: s["camera"]["state"] == "streaming", "stream resume after photo")
        client.command("cameraStop")
        check(client.request("/api/camera/frame?after=0").status == 204, "Stopped camera retained a stale frame")
        client.command("cameraStart")
        restarted, _ = wait_frame(client, resumed)
        check(restarted > resumed, "Restarted stream sequence did not advance")
        check(not heartbeat_errors, "A heartbeat failed during the mock exercise")
        print(f"MOCK_CAMERA_PASS fresh JPEGs, truthful timing, {dimensions[0]}x{dimensions[1]} photo, resume and restart")
    finally:
        cleanup_errors = []
        if claimed:
            # Cleanup still checks mode before each operation. It cannot accidentally
            # operate a hardware service restarted at the same address.
            for kind, fields in (("stop", {}), ("rgb", {"index":-1,"color":0}),
                                 ("buttonLeds", {"mask":0}), ("cameraStop", {})):
                try:
                    client.command(kind, **fields)
                except Exception as error:
                    cleanup_errors.append(f"{kind}: {type(error).__name__}")
        done.set()
        if worker:
            worker.join(timeout=client.timeout+1)
        if claimed:
            try:
                client.command("release")
                final = client.state()
                check(final["controller"]["clientId"] is None, "Controller was not released")
                check(not final["robot"]["motorEnabled"] and final["robot"]["left"] == 0 and
                      final["robot"]["right"] == 0, "Mock motors were not stopped during cleanup")
            except Exception as error:
                cleanup_errors.append(f"release: {type(error).__name__}")
        check(not cleanup_errors, "Cleanup failed: " + ", ".join(cleanup_errors))
    print("MOCK_EXERCISE_PASS controller released; motors stopped; LEDs and camera off")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Server base URL, e.g. http://192.168.4.160:8788")
    parser.add_argument("--token-file", type=Path, required=True, help="Local token file; token is never printed")
    parser.add_argument("--exercise-mock", action="store_true", help="Exercise outputs only after verifying mock mode")
    parser.add_argument("--timeout", type=float, default=15, help="Per-request timeout in seconds (default 15)")
    args = parser.parse_args(argv)
    token = ""
    try:
        check(math.isfinite(args.timeout) and 0 < args.timeout <= 60, "Timeout must be between 0 and 60 seconds")
        token = args.token_file.read_text(encoding="utf-8").strip()
        check(bool(token) and "\n" not in token and "\r" not in token, "Token file is empty or contains multiple lines")
        client = Client(args.url, token, args.timeout)
        readonly_checks(client)
        if args.exercise_mock:
            exercise_mock(client)
        return 0
    except KeyboardInterrupt:
        print("CHECK_INTERRUPTED", file=sys.stderr)
        return 130
    except Exception as error:
        message = str(error).replace(token, "[redacted]") if token else str(error)
        print("CHECK_FAILED:", message, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
