#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""A local-network robot workbench, using the real Python keel bindings.

The HTTP threads never own the board control loop or the camera. The mock mode
is an explicit, portable UI demonstration, not a fallback for hardware failures.
"""
from __future__ import annotations

import argparse
from collections import OrderedDict, deque
from concurrent.futures import Future, TimeoutError as FutureTimeout
from dataclasses import dataclass
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import mimetypes
import os
from pathlib import Path
import queue
import re
import secrets
import signal
import sys
import threading
import time
from urllib.parse import parse_qs, unquote, urlsplit

ROOT = Path(__file__).resolve().parent
MAX_DUTY = 350
DRIVE_MS = 350
LEASE_MS = 1500
MAX_BODY = 16 * 1024


def clock_ms() -> float:
    """Use the native camera's CLOCK_MONOTONIC epoch on Linux."""
    if hasattr(time, 'CLOCK_MONOTONIC'):
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC) / 1_000_000
    return time.monotonic_ns() / 1_000_000


class ApiError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


def integer(value, name, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ApiError(400, f'{name} must be an integer in {low}..{high}')
    return value


class MockBoard:
    def __init__(self):
        self.state = 'DISARMED'
        self.arm_at = 0
        self.left = self.right = self.button_leds = 0
        self.rgb = [0] * 6
        self.distance = None
        self.fault = None
        self.capabilities = ['MOTORS', 'RGB', 'BUTTONS', 'BUTTON_LEDS', 'RANGE', 'SIMULATED']
        self.cycles = 0

    def apply(self, command):
        kind = command['type']
        if kind == 'arm':
            if self.state == 'FAULTED':
                raise ApiError(409, 'Clear the fault before arming')
            self.state = 'ARMING'
            self.arm_at = clock_ms() + 650
        elif kind == 'clearFault':
            self.state, self.fault = 'DISARMED', None
        elif kind == 'rgb':
            if command['index'] == -1:
                self.rgb = [command['color']] * 6
            else:
                self.rgb[command['index']] = command['color']
        elif kind == 'buttonLeds':
            self.button_leds = command['mask']
        elif kind == 'range':
            self.distance = 750  # Deliberately synthetic, only in explicit mock mode.

    def stop(self, emergency=False):
        self.left = self.right = 0
        if emergency:
            self.state, self.fault = 'FAULTED', 'USER_STOP'
        elif self.state != 'FAULTED':
            self.state = 'DISARMED'

    def advance(self, left, right, cycle=None):
        self.cycles += 1
        if self.state == 'ARMING' and clock_ms() >= self.arm_at:
            self.state = 'ARMED'
        self.left, self.right = (left, right) if self.state == 'ARMED' else (0, 0)

    def snapshot(self):
        return dict(state=self.state, left=self.left, right=self.right,
                    motorEnabled=self.state == 'ARMED', rgb=list(self.rgb),
                    buttonLeds=self.button_leds, buttons=0, distanceMm=self.distance,
                    fault=self.fault, capabilities=self.capabilities)

    def close(self):
        self.stop()


class NativeBoard:
    def __init__(self, mode):
        # Source checkout use is supported, but an installed package takes priority.
        try:
            import keel
        except ImportError:
            sys.path.insert(0, str(ROOT.parents[1] / 'bindings/python/src'))
            import keel
        self.keel = keel
        self.device = keel.open(backend=keel.Backend.SIMULATOR if mode == 'simulator' else keel.Backend.AUTO)
        self.capabilities = [flag.name for flag in keel.Capability if flag in self.device.capabilities()]
        self.loop = None
        self.distance = None
        self.range_pending = False
        self.rgb = [0] * 6

    def start(self, callback):
        self.loop = self.keel.ControlLoop(self.device, callback)
        self.loop.start()

    def apply(self, command):
        d, kind = self.device, command['type']
        if kind == 'arm':
            d.request_arm()
        elif kind == 'clearFault':
            d.clear_fault()
        elif kind == 'rgb':
            if command['index'] == -1:
                d.set_rgb_all(command['color'])
                self.rgb = [command['color']] * 6
            else:
                d.set_rgb(command['index'], command['color'])
                self.rgb[command['index']] = command['color']
        elif kind == 'buttonLeds':
            d.set_button_leds(15, command['mask'])
        elif kind == 'range':
            d.range_start()
            self.range_pending = True

    def stop(self, emergency=False):
        (self.device.emergency_stop if emergency else self.device.disarm)()

    def advance(self, left, right, cycle=None):
        if cycle.snapshot.state == self.keel.State.ARMED:
            cycle.drive(left, right)
        if self.range_pending:
            result = self.device.range_poll()
            if result.status != self.keel.RangeStatus.PENDING:
                self.distance = result.distance_mm if result.valid else None
                self.range_pending = False

    def snapshot(self):
        s = self.loop.snapshot if self.loop else self.device.snapshot()
        return dict(state=s.state.name, left=s.output_left, right=s.output_right,
                    motorEnabled=bool(s.inputs & self.keel.SafetyInput.ARMED),
                    rgb=list(self.rgb), buttonLeds=int(s.button_leds), buttons=int(s.buttons_held),
                    distanceMm=self.distance, fault=s.fault_cause.name if s.state == self.keel.State.FAULTED else None,
                    capabilities=self.capabilities)

    def close(self):
        self.device.disarm()
        if self.loop:
            self.loop.stop()
            self.loop.join(timeout=2)
        self.device.close()


@dataclass(frozen=True)
class Frame:
    sequence: int
    captured_ms: float
    basis: str
    data: bytes


class CameraWorker:
    """Only this thread lists, opens, starts, reads or captures on a native camera."""
    def __init__(self, mode, authorize):
        self.mode, self.authorize = mode, authorize
        self.lock = threading.Lock()
        self.requests = queue.Queue(maxsize=8)
        self.closed = threading.Event()
        self.camera = None
        self.binding = None
        self.latest = None
        self.photos = OrderedDict()
        self.photo_bytes = 0
        self.sequence = 0
        self.last_frame_ms = None
        self.timeouts = 0
        self.stats = dict(available=mode == 'mock', state='stopped', width=640, height=480,
                          fps=0, frames=0, dropped=0, bytes=0, source='mock' if mode == 'mock' else 'libcamera',
                          autofocus=mode == 'mock', error=None)
        self.thread = threading.Thread(target=self.run, name='keel-camera-owner', daemon=True)
        self.thread.start()

    def snapshot(self):
        with self.lock:
            return dict(self.stats)

    def request(self, command, generation, client):
        future = Future()
        try:
            self.requests.put_nowait((command, generation, client, future))
        except queue.Full:
            raise ApiError(429, 'Camera command queue is full') from None
        try:
            return future.result(timeout=12)
        except FutureTimeout:
            future.cancel()
            raise ApiError(504, 'Camera operation timed out') from None

    def get_frame(self, after):
        with self.lock:
            return self.latest if self.latest and self.latest.sequence > after else None

    def get_photo(self, name):
        with self.lock:
            data = self.photos.get(name)
        if data is None:
            raise ApiError(404, 'Photo expired or not found')
        return data

    def _status(self, **values):
        with self.lock:
            self.stats.update(values)

    def _open(self):
        if self.camera is None:
            self.camera = self.binding.open(width=640, height=480, framerate=20, frame_queue=1)
            self.camera.set_encoder(self.binding.Codec.MJPEG, quality=80, encoded_queue=1)
            info = self.camera.stream_info()
            self._status(width=info.width, height=info.height, available=True,
                         autofocus=self.camera.info.autofocus, source=self.camera.info.model)

    def _apply(self, kind):
        mock = self.mode == 'mock'
        if not mock:
            self._open()
        if kind == 'cameraStart':
            if self.stats['state'] != 'streaming':
                if not mock:
                    self.camera.start()
                self.last_frame_ms = None
                self._status(state='streaming', error=None)
        elif kind == 'cameraStop':
            if not mock:
                self.camera.stop()
            with self.lock:
                self.latest = None
                self.stats.update(state='stopped', fps=0, error=None)
        elif kind == 'autofocus':
            if not self.stats['autofocus']:
                raise ApiError(409, 'This camera has a fixed-focus lens; autofocus is unavailable')
            if not mock:
                if self.stats['state'] != 'streaming':
                    raise ApiError(409, 'Start the camera before requesting autofocus')
                self.camera.trigger_autofocus(timeout=2)
        elif kind == 'photo':
            was_streaming = self.stats['state'] == 'streaming'
            self._status(state='capturing')
            with self.lock:
                self.latest = None
            try:
                if mock:
                    data = (ROOT / 'mock-camera.jpg').read_bytes()
                else:
                    if was_streaming:
                        self.camera.stop()
                    data = self.camera.capture_still(quality=90, timeout=6).data
                if len(data) > 24 * 1024 * 1024:
                    raise ApiError(507, 'Photo is too large for the demonstration capture store')
                name = secrets.token_hex(12) + '.jpg'
                with self.lock:
                    while self.photos and (len(self.photos) >= 4 or self.photo_bytes + len(data) > 32 * 1024 * 1024):
                        _, old = self.photos.popitem(last=False)
                        self.photo_bytes -= len(old)
                    self.photos[name] = data
                    self.photo_bytes += len(data)
                return {'ok': True, 'photo': '/api/photos/' + name}
            finally:
                if not mock and was_streaming:
                    self.camera.start()
                self.last_frame_ms = None
                self._status(state='streaming' if was_streaming else 'stopped')
        return {'ok': True}

    def _publish(self, data, timestamp, basis):
        if not data.startswith(b'\xff\xd8') or len(data) > 8 * 1024 * 1024:
            raise ApiError(502, 'Camera did not supply a bounded JPEG frame')
        now = clock_ms()
        interval = now - self.last_frame_ms if self.last_frame_ms is not None else None
        self.last_frame_ms = now
        with self.lock:
            self.sequence += 1
            self.latest = Frame(self.sequence, timestamp, basis, data)
            self.stats['frames'] += 1
            self.stats['bytes'] += len(data)
            if interval and interval > 0:
                measured = 1000 / interval
                self.stats['fps'] = measured if not self.stats['fps'] else .85 * self.stats['fps'] + .15 * measured

    def run(self):
        mock_jpeg = (ROOT / 'mock-camera.jpg').read_bytes() if self.mode == 'mock' else None
        if self.mode != 'mock':
            try:
                from keel import camera
                self.binding = camera
                listed = camera.list_cameras()
                self._status(available=bool(listed), autofocus=bool(listed and listed[0].info.autofocus),
                             error=None if listed else 'No camera detected')
            except Exception as error:
                self._status(available=False, error=str(error))
        next_mock = clock_ms()
        while not self.closed.is_set():
            try:
                item = self.requests.get(timeout=.005 if self.stats['state'] == 'streaming' else .1)
            except queue.Empty:
                item = None
            if item:
                kind, generation, client, future = item
                if not future.set_running_or_notify_cancel():
                    continue
                try:
                    self.authorize(generation, client)
                    if self.mode != 'mock' and self.binding is None:
                        raise ApiError(503, self.stats['error'] or 'Camera library unavailable')
                    future.set_result(self._apply(kind))
                except Exception as error:
                    if not isinstance(error, ApiError):
                        self._status(error=str(error), state='failed', fps=0)
                        with self.lock:
                            self.latest = None
                        if self.camera:
                            try:
                                self.camera.close()
                            except Exception:
                                pass
                            self.camera = None
                    future.set_exception(error if isinstance(error, ApiError) else ApiError(503, str(error)))
            if self.stats['state'] != 'streaming':
                continue
            try:
                if self.mode == 'mock':
                    now = clock_ms()
                    if now < next_mock:
                        self.closed.wait(min(.02, (next_mock - now) / 1000))
                        continue
                    next_mock = now + 50
                    self._publish(mock_jpeg, now, 'server-receipt')
                else:
                    chunk = self.camera.read_chunk(timeout=.05)
                    receipt = clock_ms()
                    timestamp = chunk.timestamp_ns / 1_000_000
                    # Native API promises MONOTONIC. Reject impossible/stale timestamps
                    # rather than fabricate a sensor latency with the wrong epoch.
                    sensor = 0 < timestamp <= receipt + 1 and receipt - timestamp < 60_000
                    self._publish(chunk.data, timestamp if sensor else receipt,
                                  'sensor' if sensor else 'server-receipt')
                    self.timeouts = 0
                    stats = self.camera.stats()
                    self._status(dropped=stats.encoder_frames_dropped + stats.chunks_dropped)
            except Exception as error:
                if self.binding and isinstance(error, self.binding.CameraTimeoutError):
                    self.timeouts += 1
                    if self.timeouts < 60:
                        continue
                with self.lock:
                    self.latest = None
                    self.stats.update(state='failed', error=str(error), fps=0)
                if self.camera:
                    try:
                        self.camera.stop()
                    except Exception:
                        pass
        if self.camera:
            self.camera.close()
        while True:
            try:
                *_, future = self.requests.get_nowait()
                if not future.done():
                    future.set_exception(ApiError(503, 'Server shutting down'))
            except queue.Empty:
                break

    def close(self):
        self.closed.set()
        self.thread.join(timeout=8)


class Controller:
    def __init__(self, mode='mock', allow_motion=False):
        self.mode = mode
        self.motion_allowed = mode != 'hardware' or allow_motion
        self.started = clock_ms()
        self.lock = threading.RLock()
        self.closed = threading.Event()
        self.client = None
        self.lease_deadline = self.drive_deadline = 0
        self.left = self.right = 0
        self.generation = 0
        self.sequences = {}
        self.pending = deque()
        self.loop_error = None
        self.last_faulted = False
        self.max_gap = self.max_step = 0
        self.cycles = self.overruns = 0
        self.last_tick = clock_ms()
        self.board = MockBoard() if mode == 'mock' else NativeBoard(mode)
        self.camera = CameraWorker(mode, self.authorize)
        if isinstance(self.board, NativeBoard):
            self.board.start(self.tick)
            self.thread = None
        else:
            self.thread = threading.Thread(target=self.run, name='keel-control-owner', daemon=True)
            self.thread.start()

    def authorize(self, generation, client):
        with self.lock:
            self.expire(clock_ms())
            if self.closed.is_set() or generation != self.generation or client != self.client:
                raise ApiError(409, 'Controller lease changed or expired; acquire control again')

    def _stop(self, emergency=False):
        self.left = self.right = self.drive_deadline = 0
        self.generation += 1
        self.board.stop(emergency)
        if emergency:
            self.last_faulted = True

    def expire(self, now):
        if self.client is not None and now >= self.lease_deadline:
            self._stop()
            self.client = None
        elif self.drive_deadline and now >= self.drive_deadline:
            self._stop()

    def command(self, data):
        if not isinstance(data, dict):
            raise ApiError(400, 'Command must be a JSON object')
        kind = data.get('type')
        allowed = {'claim', 'heartbeat', 'release', 'arm', 'disarm', 'stop', 'estop', 'clearFault',
                   'drive', 'rgb', 'buttonLeds', 'range', 'cameraStart', 'cameraStop', 'autofocus', 'photo'}
        if type(kind) is not str or kind not in allowed:
            raise ApiError(400, 'Unknown command type')
        client = data.get('clientId')
        if not isinstance(client, str) or not re.fullmatch(r'[A-Za-z0-9_.:-]{1,128}', client):
            raise ApiError(400, 'clientId must contain 1..128 letters, numbers, dots, colons, underscores or hyphens')
        seq = integer(data.get('seq'), 'seq', 1, 2**53 - 1)
        if kind == 'drive':
            integer(data.get('left'), 'left', -MAX_DUTY, MAX_DUTY)
            integer(data.get('right'), 'right', -MAX_DUTY, MAX_DUTY)
        elif kind == 'rgb':
            integer(data.get('index'), 'index', -1, 5)
            integer(data.get('color'), 'color', 0, 0xFFFFFF)
        elif kind == 'buttonLeds':
            integer(data.get('mask'), 'mask', 0, 15)
        camera_request = None
        future = None
        with self.lock:
            now = clock_ms()
            self.expire(now)
            if self.closed.is_set():
                raise ApiError(503, 'Server shutting down')
            safety_stop = kind in ('stop', 'disarm', 'estop')
            safe_sequence = safety_stop or kind == 'release'
            if not safe_sequence and client not in self.sequences and len(self.sequences) >= 256:
                raise ApiError(429, 'Too many client identities; restart the demonstration server')
            if not safe_sequence and seq <= self.sequences.get(client, 0):
                raise ApiError(409, 'Stale command sequence')
            # Stops always win: a concurrent heartbeat may have overtaken this
            # request in transit. Never lower the watermark for future motion.
            if client in self.sequences or len(self.sequences) < 256:
                self.sequences[client] = max(seq, self.sequences.get(client, 0))
            if safety_stop:
                # A different browser's sequence cannot fence the owner's
                # in-flight packets. Require a fresh claim to establish that fence.
                if self.client is not None and self.client != client:
                    self.client = None
                self._stop(kind == 'estop')
            elif kind == 'claim':
                if self.client is not None and self.client != client:
                    raise ApiError(409, 'Another browser currently controls the robot')
                if self.client != client:
                    self._stop()
                    self.client = client
                self.lease_deadline = now + LEASE_MS
            else:
                if self.client != client:
                    raise ApiError(409, 'Acquire control before sending commands')
                if kind == 'heartbeat':
                    self.lease_deadline = now + LEASE_MS
                elif kind == 'release':
                    self._stop()
                    self.client = None
                elif kind == 'drive':
                    if not self.motion_allowed:
                        raise ApiError(403, 'Real motion requires server --allow-motion')
                    if self.board.snapshot()['state'] != 'ARMED':
                        raise ApiError(409, 'Robot must be armed before driving')
                    self.left, self.right = data['left'], data['right']
                    self.drive_deadline = now + DRIVE_MS if self.left or self.right else 0
                elif kind in ('cameraStart', 'cameraStop', 'autofocus', 'photo'):
                    camera_request = (kind, self.generation, client)
                else:
                    if kind == 'arm' and not self.motion_allowed:
                        raise ApiError(403, 'Real motion requires server --allow-motion')
                    if kind == 'arm' and self.board.snapshot()['state'] != 'DISARMED':
                        raise ApiError(409, 'Disarm or clear the fault before requesting a new arm')
                    if len(self.pending) >= 16:
                        raise ApiError(429, 'Control queue full; try again')
                    if kind in ('arm', 'clearFault'):
                        self.left = self.right = self.drive_deadline = 0
                        self.generation += 1
                    future = Future()
                    self.pending.append((dict(data), self.generation, client, future))
        if camera_request:
            return self.camera.request(*camera_request)
        if future:
            try:
                return future.result(timeout=1)
            except FutureTimeout:
                future.cancel()
                raise ApiError(504, 'Board control operation timed out') from None
        return {'ok': True}

    def tick(self, cycle=None):
        start = clock_ms()
        with self.lock:
            self.max_gap = max(self.max_gap, start - self.last_tick)
            self.last_tick = start
            self.expire(start)
            faulted = self.board.snapshot()['state'] == 'FAULTED'
            if faulted and not self.last_faulted:
                self.left = self.right = self.drive_deadline = 0
                self.generation += 1
            self.last_faulted = faulted
            # At most one ancillary operation per real-time tick, never a burst.
            if self.pending:
                command, generation, client, future = self.pending.popleft()
                if future.set_running_or_notify_cancel():
                    try:
                        self.authorize(generation, client)
                        self.board.apply(command)
                        future.set_result({'ok': True})
                    except Exception as error:
                        future.set_exception(error if isinstance(error, ApiError) else ApiError(409, str(error)))
            self.board.advance(self.left, self.right, cycle)
            self.cycles += 1
            elapsed = clock_ms() - start
            self.max_step = max(self.max_step, elapsed)
            if elapsed > 10:
                self.overruns += 1

    def run(self):
        while not self.closed.is_set():
            start = clock_ms()
            try:
                self.tick()
            except Exception as error:
                with self.lock:
                    self.loop_error = str(error)
                    self._stop(True)
                break
            self.closed.wait(max(0, (10 - (clock_ms() - start)) / 1000))

    def state(self):
        now = clock_ms()
        with self.lock:
            board = self.board.snapshot()
            loop = dict(running=not self.closed.is_set() and self.loop_error is None,
                        cycles=self.cycles, overruns=self.overruns, maxGapMs=self.max_gap, maxStepMs=self.max_step)
            if isinstance(self.board, NativeBoard):
                stats = self.board.loop.stats()
                loop.update(running=self.board.loop.running, cycles=stats.cycles, overruns=stats.overruns,
                            maxGapMs=stats.max_gap_us / 1000, maxStepMs=stats.max_step_us / 1000)
                if self.board.loop.error:
                    loop['error'] = str(self.board.loop.error)
            owner = dict(clientId=self.client, expiresInMs=max(0, self.lease_deadline - now) if self.client else 0)
        temperature = None
        try:
            temperature = int(Path('/sys/class/thermal/thermal_zone0/temp').read_text()) / 1000
        except (OSError, ValueError):
            pass
        return dict(language='Python', mode=self.mode, serverTimeMs=now, motionAllowed=self.motion_allowed,
                    maxDuty=MAX_DUTY, driveTimeoutMs=DRIVE_MS, controller=owner, robot=board,
                    loop=loop, camera=self.camera.snapshot(), system=dict(uptimeS=(now-self.started)/1000, cpuTempC=temperature))

    def close(self):
        with self.lock:
            self.closed.set()
            self._stop()
            while self.pending:
                *_, future = self.pending.popleft()
                if not future.done():
                    future.set_exception(ApiError(503, 'Server shutting down'))
        if self.thread:
            self.thread.join(timeout=2)
        self.board.close()
        self.camera.close()


class DemoServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, controller, token, web_root):
        self.controller, self.token, self.web_root = controller, token, Path(web_root).resolve()
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(address, Handler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            try:
                request.sendall(b'HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n')
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    # Headers and body are separate writes. Send both promptly on keep-alive
    # sockets instead of letting Nagle wait for the client's delayed ACK.
    disable_nagle_algorithm = True

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, format, *args):
        # Request targets can contain secrets supplied by a mistaken client.
        pass

    def send_bytes(self, status, data=b'', content_type='application/json', headers=None):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store' if self.path.startswith('/api/') else 'no-cache')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('X-Frame-Options', 'DENY')
        self.send_header('Referrer-Policy', 'no-referrer')
        if headers:
            for key, value in headers.items():
                self.send_header(key, str(value))
        self.end_headers()
        if self.command != 'HEAD' and data:
            self.wfile.write(data)

    def send_json(self, status, value):
        self.send_bytes(status, json.dumps(value, allow_nan=False, separators=(',', ':')).encode())

    def auth(self):
        expected = 'Bearer ' + self.server.token
        actual = self.headers.get('Authorization', '')
        if not hmac.compare_digest(actual.encode(), expected.encode()):
            raise ApiError(401, 'Enter the access token shown by the server')

    def check_origin(self):
        origin = self.headers.get('Origin')
        if self.headers.get('Sec-Fetch-Site') == 'cross-site':
            raise ApiError(403, 'Cross-origin writes are not accepted')
        if origin:
            parsed = urlsplit(origin)
            if parsed.scheme not in ('http', 'https') or parsed.netloc != self.headers.get('Host') or parsed.path:
                raise ApiError(403, 'Cross-origin writes are not accepted')

    def do_GET(self):
        self.handle_safely(self.get)

    def do_HEAD(self):
        self.handle_safely(self.get)

    def do_POST(self):
        self.handle_safely(self.post)

    def handle_safely(self, action):
        try:
            action()
        except ApiError as error:
            if self.command == 'POST':
                self.close_connection = True
            self.send_json(error.status, {'error': str(error)})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True
        except (ValueError, UnicodeError) as error:
            self.send_json(400, {'error': str(error)})
        except Exception as error:
            self.send_json(500, {'error': 'Operation failed: ' + str(error)})

    def get(self):
        received = clock_ms()
        parsed = urlsplit(self.path)
        path = unquote(parsed.path)
        if path.startswith('/api/'):
            self.auth()
            if path == '/api/state':
                self.send_json(200, self.server.controller.state())
            elif path == '/api/ping':
                self.send_json(200, dict(serverReceiveMs=received, serverSendMs=clock_ms()))
            elif path == '/api/camera/frame':
                try:
                    after = int(parse_qs(parsed.query).get('after', ['-1'])[0])
                except ValueError:
                    raise ApiError(400, 'after must be an integer') from None
                frame = self.server.controller.camera.get_frame(after)
                if frame is None:
                    self.send_bytes(204)
                else:
                    sent = clock_ms()
                    self.send_bytes(200, frame.data, 'image/jpeg', {
                        'X-Sequence': frame.sequence, 'X-Captured-Ms': frame.captured_ms,
                        'X-Sent-Ms': sent, 'X-Timestamp-Basis': frame.basis,
                        'X-Capture-Age-Ms': max(0, sent-frame.captured_ms)})
            elif re.fullmatch(r'/api/photos/[a-f0-9]{24}\.jpg', path):
                name = path.rsplit('/', 1)[1]
                self.send_bytes(200, self.server.controller.camera.get_photo(name), 'image/jpeg',
                                {'Content-Disposition': f'attachment; filename="keel-{name}"'})
            else:
                raise ApiError(404, 'API route not found')
            return
        relative = path.lstrip('/') or 'index.html'
        target = (self.server.web_root / relative).resolve()
        if not target.is_relative_to(self.server.web_root) or not target.is_file():
            raise ApiError(404, 'Asset not found; build the shared web app first')
        data = target.read_bytes()
        content_type = mimetypes.guess_type(target.name)[0] or 'application/octet-stream'
        if target.suffix == '.js':
            content_type = 'text/javascript'
        self.send_bytes(200, data, content_type)

    def post(self):
        if urlsplit(self.path).path != '/api/command':
            raise ApiError(404, 'API route not found')
        self.auth()
        self.check_origin()
        if self.headers.get_content_type() != 'application/json':
            raise ApiError(415, 'Commands require application/json')
        if self.headers.get('Transfer-Encoding'):
            self.close_connection = True
            raise ApiError(400, 'Chunked command bodies are not accepted')
        try:
            length = int(self.headers.get('Content-Length', '-1'))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_BODY:
            self.close_connection = True
            raise ApiError(413, 'Command body must contain 1..16384 bytes')
        body = self.rfile.read(length)
        if len(body) != length:
            raise ApiError(400, 'Incomplete command body')
        def reject_constant(value):
            raise ValueError('Nonfinite JSON numbers are not accepted')
        data = json.loads(body, parse_constant=reject_constant)
        self.send_json(200, self.server.controller.command(data))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('mock', 'simulator', 'hardware'), default='mock')
    parser.add_argument('--allow-motion', action='store_true')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8787)
    parser.add_argument('--web-root', type=Path, default=ROOT.parent/'web/dist')
    parser.add_argument('--token-file', type=Path)
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error('--port must be 1..65535')
    generated = False
    try:
        token = args.token_file.read_text(encoding='utf-8').strip() if args.token_file else os.getenv('KEEL_DEMO_TOKEN')
        if not token:
            token, generated = secrets.token_urlsafe(24), True
        if len(token) < 16 or not token.isascii() or any(c.isspace() for c in token):
            parser.error('access token must be at least 16 ASCII characters without whitespace')
        controller = Controller(args.mode, args.allow_motion)
    except Exception as error:
        print(f'Cannot start {args.mode} backend: {error}', file=sys.stderr)
        return 1
    try:
        server = DemoServer((args.host, args.port), controller, token, args.web_root)
    except Exception:
        controller.close()
        raise
    def stop(signum, frame):
        # shutdown must run outside serve_forever's thread.
        threading.Thread(target=server.shutdown, daemon=True).start()
    for name in ('SIGINT', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), stop)
    print(f'Keel Python demonstrator [{args.mode}] http://{args.host}:{args.port}', flush=True)
    print('Real motor arming: ' + ('enabled' if args.mode == 'hardware' and args.allow_motion else 'disabled'), flush=True)
    print('Access token: ' + token if generated else 'Access token loaded from configuration.', flush=True)
    try:
        server.serve_forever(poll_interval=.1)
    finally:
        controller.close()
        server.server_close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
