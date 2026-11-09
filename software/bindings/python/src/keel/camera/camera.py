# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""keel.camera.open(), list_cameras() and Camera: one open camera of libkeel-camera.

Every Camera method passes its arguments to libkeel-camera and raises by status (keel.camera.errors); the library
checks every option against the camera. The binding itself checks only what the library cannot see: integers that
ctypes would truncate, numbers that are not finite and arguments of the wrong type (TypeError, ValueError or
OverflowError, before any call).

Threads: the thread that opens a camera owns it, and every method but close() must be called from that thread. From
another thread libkeel-camera refuses the call with no effect (CameraThreadError). close() works from any thread: it
wakes a read blocked in the owning thread, which then raises CameraClosedError, or ends its frames() or chunks()
loop. A program can therefore run its control loop and its camera on separate threads, each owning its own handle.
"""

from __future__ import annotations

import ctypes
import logging
import math
import numbers
import os
import threading
import warnings
import weakref
from collections.abc import Iterator
from types import TracebackType
from typing import TYPE_CHECKING, Self, SupportsIndex

from keel._internal.camera import _generated as g
from keel._internal.camera import native_library
from keel._internal.camera.native_library import NativeCameraLibrary
from keel.errors._generated import Status

from . import limits
from ._generated import AutofocusMode, AutofocusRange, AutofocusState, CameraFlag, CameraInfo, CameraStats, WhiteBalance
from .encoding import Codec, EncodedChunk, EncoderFlag, H264Profile
from .errors import CameraClosedError, ErrorReport
from .errors.error_factory import camera_error, check, error_from_record
from .errors.exceptions import status_name
from .frames import Frame, PixelFormat, StreamInfo
from .listed_camera import ListedCamera
from .outputs import OutputStats
from .stills import StillImage, StillInfo

__all__ = ["Camera", "list_cameras", "open"]

_log = logging.getLogger("keel.camera")


def _number(name: str, value: object) -> float:
    """Return value as a finite float, or raise TypeError or ValueError naming the argument."""
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise TypeError(f"{name} must be a number, not {type(value).__name__}; no call was made")
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"{name} must be a finite number, not {number}; no call was made")
    return number


def _milli(name: str, value: object) -> int:
    """Return a fractional option in the header's x 1000 scale (for example framerate as framerate_milli)."""
    return round(_number(name, value) * 1000)


def _milliseconds(name: str, seconds: object) -> int:
    """Return a timeout in seconds as whole milliseconds, rounded up; ValueError for a negative one."""
    number = _number(name, seconds)
    if number < 0:
        raise ValueError(f"{name} must not be negative, not {number}; no call was made")
    return math.ceil(number * 1000)


def _still_buffer_size(width: int, height: int) -> int:
    """Return the size that always holds a JPEG still: width and height rounded up to 16, times 3, plus 2048."""
    return ((width + 15) // 16 * 16) * ((height + 15) // 16 * 16) * 3 + 2048


class _CameraState:
    """What close() and the finalizer share: the token, the close claim, the close result and its completion.

    The finalizer holds this object, never the Camera, so a dropped camera can be collected.
    """

    __slots__ = ("claim", "done", "opener_pid", "status", "token")

    def __init__(self, token: int) -> None:
        self.token = token
        self.claim: dict[str, tuple[int, object]] = {}
        self.done = threading.Event()
        self.status: Status | None = None
        self.opener_pid = os.getpid()


def _close(state: _CameraState, natives: g.Natives) -> bool:
    """Close the camera once, from any thread, and return whether this call issued the close.

    The first claim wins and issues keel_camera_close; a later closer waits until it has returned, unless it is the
    closing thread itself (a signal handler that runs inside close()), which cannot wait for itself. Never raises
    for the camera's status: a failed close is logged.
    """
    ticket = (threading.get_ident(), object())
    holder = state.claim.setdefault("holder", ticket)
    if holder is not ticket:
        if holder[0] != threading.get_ident():
            state.done.wait()
        return False
    try:
        if os.getpid() != state.opener_pid:
            state.status = Status.E_NOT_OWNER  # a forked child: the parent's camera is not touched
            return True
        state.status = Status(natives["keel_camera_close"](state.token))
    finally:
        state.done.set()
    if state.status != Status.OK:
        _log.warning("keel_camera_close: %s", status_name(state.status))
    return True


def _finalize(state: _CameraState, natives: g.Natives) -> None:
    """Close a camera that was collected, or still open at interpreter exit, without having been closed."""
    try:
        if os.getpid() != state.opener_pid or not _close(state, natives):
            return
        _log.warning("keel.camera.Camera was not closed; its finalizer closed it")
        warnings.warn("keel.camera.Camera was not closed", ResourceWarning, stacklevel=2)
    except Exception:  # noqa: BLE001  (a finalizer must never raise, also during interpreter shutdown)
        pass


class Camera:
    """One open camera. Open it with keel.camera.open() and close it with close() or a with block.

    A camera opens stopped: configure its encoder and outputs, then start() it. Its frames feed, at the same time,
    the raw frame queue (read_frame(), frames()), one hardware encoder (set_encoder()) whose chunks feed the encoded
    queue (read_chunk(), chunks()) and every output the library writes itself (start_output()). Every queue is
    bounded and drops instead of growing; stats() counts each drop.

    A camera that is dropped without being closed is closed by a finalizer, which also emits a ResourceWarning. Use
    every method but close() from the thread that opened the camera.
    """

    __slots__ = (
        "__weakref__",
        "_chunk_buffer",
        "_finalizer",
        "_frame_bytes",
        "_info",
        "_natives",
        "_state",
    )

    _state: _CameraState
    _natives: g.Natives
    _info: CameraInfo
    _frame_bytes: int
    _chunk_buffer: bytearray
    if TYPE_CHECKING:  # weakref.finalize is generic only to type checkers
        _finalizer: weakref.finalize[[_CameraState, g.Natives], Camera]

    def __init__(self, *_: object) -> None:
        """Refuse direct construction: cameras come from keel.camera.open()."""
        raise TypeError("use keel.camera.open() to open a camera")

    @classmethod
    def _create(cls, library: NativeCameraLibrary, token: int) -> Self:
        camera = object.__new__(cls)
        camera._state = _CameraState(token)
        camera._natives = library.natives
        camera._finalizer = weakref.finalize(camera, _finalize, camera._state, library.natives)
        camera._frame_bytes = 0
        camera._chunk_buffer = bytearray(1)
        try:
            raw = g.CameraInfoStruct()
            camera._check(g.keel_camera_get_info(camera._natives, token, ctypes.byref(raw)), "keel_camera_get_info")
            camera._info = g.convert_camera_info(raw)
            camera._refresh_buffers()
        except BaseException:
            camera.close()
            raise
        return camera

    def _check(self, code: int, function: str) -> None:
        check(self._natives, self._state.token, code, function)

    def _refresh_buffers(self) -> None:
        """Size the frame and chunk buffers from the stream the camera has configured."""
        stream = self.stream_info()
        self._frame_bytes = stream.frame_bytes
        if len(self._chunk_buffer) != max(stream.encoded_bytes_max, 1):
            self._chunk_buffer = bytearray(max(stream.encoded_bytes_max, 1))

    def __repr__(self) -> str:
        """Describe the camera and whether it is closed."""
        state = "closed" if self.closed else "open"
        return f"<keel.camera.Camera {self._info.index}: {self._info.model} ({self._info.sensor}), {state}>"

    def __enter__(self) -> Self:
        """Return the camera; the with block closes it."""
        return self

    def __exit__(
        self, kind: type[BaseException] | None, value: BaseException | None, traceback: TracebackType | None
    ) -> None:
        """Close the camera."""
        self.close()

    # ---------------------------------------------------------------- lifetime ----

    @property
    def info(self) -> CameraInfo:
        """The camera this handle opened (keel_camera_get_info, read at open)."""
        return self._info

    @property
    def index(self) -> int:
        """The camera's index."""
        return self._info.index

    @property
    def closed(self) -> bool:
        """True once close() has begun, from any thread."""
        return bool(self._state.claim)

    def close(self) -> None:
        """Stop capture and every output, finish their files, end the library's threads and release the camera.

        Safe from any thread and idempotent; a read blocked in the owning thread wakes up and raises
        CameraClosedError. Never raises for the camera: a failed close (an output that could not finish its file) is
        logged at WARNING. Takes up to about 3 s when an output is finishing a file or a network peer.
        """
        self._finalizer.detach()
        _close(self._state, self._natives)

    def stream_info(self) -> StreamInfo:
        """Return the layout of the configured stream and the buffer sizes the reads need (keel_camera_get_stream).

        The camera may have adjusted the requested size: width and height here are the ones it delivers.
        """
        raw = g.StreamInfoStruct()
        self._check(
            g.keel_camera_get_stream(self._natives, self._state.token, ctypes.byref(raw)), "keel_camera_get_stream"
        )
        return g.convert_stream_info(raw)

    def set_encoder(
        self,
        codec: SupportsIndex = Codec.H264,
        *,
        bitrate: SupportsIndex = 0,
        intra_period: SupportsIndex = 0,
        profile: SupportsIndex = H264Profile.DEFAULT,
        inline_headers: bool = False,
        quality: SupportsIndex = 0,
        encoded_queue: SupportsIndex = 0,
    ) -> None:
        """Choose the hardware encoder for read_chunk(), chunks() and the outputs (keel_camera_set_encoder).

        codec is Codec.H264 or Codec.MJPEG; Codec.NONE removes the encoder. Every zero means the default: H.264 at
        0.15 bit per pixel and frame (1.4 Mbit/s at 640x480 and 30 fps) with a keyframe every second, in High
        profile; MJPEG at quality 85. bitrate (bits per second), intra_period and profile are for H.264 only, quality
        (1..100) for MJPEG only. inline_headers repeats the H.264 parameter sets before every keyframe of the chunks
        the program reads (outputs always get them). Allowed while stopped and without outputs.
        """
        flags = EncoderFlag.INLINE_HEADERS if inline_headers else EncoderFlag(0)
        options = g.fill_encoder_options(
            g.EncoderOptionsStruct(),
            codec=codec,
            bitrate=bitrate,
            intra_period=intra_period,
            profile=profile,
            flags=flags,
            quality=quality,
            encoded_queue=encoded_queue,
        )
        code = g.keel_camera_set_encoder(self._natives, self._state.token, ctypes.byref(options))
        self._check(code, "keel_camera_set_encoder")
        self._refresh_buffers()

    # ----------------------------------------------------------------- capture ----

    def start(self) -> None:
        """Start capture, the encoder and the data flow of every output (keel_camera_start; up to about 500 ms)."""
        self._check(g.keel_camera_start(self._natives, self._state.token), "keel_camera_start")
        self._refresh_buffers()

    def stop(self) -> None:
        """Stop capture and the encoder; queued frames and chunks are discarded, outputs stay open (keel_camera_stop).

        The outputs resume at the next keyframe after start().
        """
        self._check(g.keel_camera_stop(self._natives, self._state.token), "keel_camera_stop")

    def read_frame(self, timeout: float = 1.0) -> Frame:
        """Take the oldest queued raw frame (keel_camera_read_frame), waiting up to timeout seconds (at most 60).

        The queue keeps frame_queue frames (by default 1, so a slow reader always gets the newest frame). Each frame
        has its own buffer; Frame.array() is a numpy view of it. Raises CameraTimeoutError when no frame arrived in
        time, CameraStateError before start() and CameraClosedError once another thread closed the camera.
        """
        milliseconds = _milliseconds("timeout", timeout)
        size = self._frame_bytes
        buffer = bytearray(size)
        raw = g.FrameInfoStruct()
        code = g.keel_camera_read_frame(
            self._natives,
            self._state.token,
            (ctypes.c_ubyte * size).from_buffer(buffer),
            size,
            milliseconds,
            ctypes.byref(raw),
        )
        self._check(code, "keel_camera_read_frame")
        return Frame(g.convert_frame_info(raw), buffer)

    def frames(self, timeout: float = 1.0) -> Iterator[Frame]:
        """Yield raw frames until the camera is closed: read_frame(timeout) in a loop.

        The loop ends without an error when close() is called, from this thread or another one. A frame that does not
        arrive within timeout raises CameraTimeoutError.
        """
        while True:
            try:
                frame = self.read_frame(timeout)
            except CameraClosedError:
                if self.closed:
                    return
                raise
            yield frame

    def read_chunk(self, timeout: float = 1.0) -> EncodedChunk:
        """Take the oldest queued encoded chunk (keel_camera_read_encoded), waiting up to timeout seconds.

        A chunk is one encoded frame: an H.264 access unit or one JPEG image. The first chunk after start() is a
        keyframe with the H.264 parameter sets; after the queue overflowed, H.264 resumes at a keyframe. Needs an
        encoder (set_encoder()); raises as read_frame() does.
        """
        milliseconds = _milliseconds("timeout", timeout)
        buffer = self._chunk_buffer
        raw = g.ChunkInfoStruct()
        code = g.keel_camera_read_encoded(
            self._natives,
            self._state.token,
            (ctypes.c_ubyte * len(buffer)).from_buffer(buffer),
            len(buffer),
            milliseconds,
            ctypes.byref(raw),
        )
        self._check(code, "keel_camera_read_encoded")
        info = g.convert_chunk_info(raw)
        return EncodedChunk(info, bytes(memoryview(buffer)[: info.bytes]))

    def chunks(self, timeout: float = 1.0) -> Iterator[EncodedChunk]:
        """Yield encoded chunks until the camera is closed: read_chunk(timeout) in a loop, ending as frames() does."""
        while True:
            try:
                chunk = self.read_chunk(timeout)
            except CameraClosedError:
                if self.closed:
                    return
                raise
            yield chunk

    def trigger_autofocus(self, timeout: float = 2.0) -> AutofocusState:
        """Run one autofocus scan and wait up to timeout seconds for its result (keel_camera_trigger_autofocus).

        In AutofocusMode.AUTO it starts one scan; in DEFAULT or CONTINUOUS it waits until the continuous search
        reports focus. Returns FOCUSED, FAILED, or SCANNING when the timeout ended first. Raises
        CameraUnsupportedError on a camera without autofocus (Camera Module 2) and in MANUAL mode.
        """
        milliseconds = _milliseconds("timeout", timeout)
        state = ctypes.c_uint32()
        code = g.keel_camera_trigger_autofocus(self._natives, self._state.token, milliseconds, ctypes.byref(state))
        self._check(code, "keel_camera_trigger_autofocus")
        return AutofocusState(state.value)

    def stats(self) -> CameraStats:
        """Return the counters since open: frames captured, read and dropped, chunks encoded, read and dropped."""
        raw = g.CameraStatsStruct()
        self._check(
            g.keel_camera_get_stats(self._natives, self._state.token, ctypes.byref(raw)), "keel_camera_get_stats"
        )
        return g.convert_camera_stats(raw)

    def last_error(self) -> ErrorReport:
        """Return the latest failure on the handle, with its message (status OK when no call failed)."""
        raw = g.ErrorReportStruct()
        self._check(
            g.keel_camera_last_error(self._natives, self._state.token, ctypes.byref(raw)), "keel_camera_last_error"
        )
        return g.convert_error_report(raw)

    # ----------------------------------------------------------------- outputs ----

    def start_output(
        self,
        destination: str | os.PathLike[str],
        *,
        queue: SupportsIndex = 0,
        max_clients: SupportsIndex = 0,
        datagram_bytes: SupportsIndex = 0,
    ) -> int:
        """Make the library write the encoded stream to a destination, and return the output's id.

        Destinations: "tcp://HOST:PORT" connects to a receiver; "tcp://:PORT?listen" waits for up to max_clients
        receivers (4 by default) and serves each from its next keyframe; "udp://HOST:PORT" sends datagrams of at
        most datagram_bytes (1472 by default); a path ending in .h264 or .264 (H.264), or .mjpeg or .mjpg (MJPEG).
        With FFmpeg's libavformat built in, also .mp4, .mkv, .mov and .ts files and rtsp://, rtmp:// and srt:// URLs;
        without it these raise CameraUnsupportedError. queue is the chunks queued for the output (64 by default).

        Needs an encoder. Opens the file, connects (up to 3 s) or binds the port at once, so those errors raise here; a
        failure later puts the output in OutputState.FAILED (output_stats()) without stopping the capture.
        """
        url = g.encode_path("keel_camera_output_start", "url", os.fspath(destination), limits.PATH_MAX)
        options = g.fill_output_options(
            g.OutputOptionsStruct(),
            flags=0,
            queue=queue,
            max_clients=max_clients,
            datagram_bytes=datagram_bytes,
            reserved0=0,
        )
        output = ctypes.c_uint32()
        code = g.keel_camera_output_start(
            self._natives, self._state.token, url, ctypes.byref(options), ctypes.byref(output)
        )
        self._check(code, "keel_camera_output_start")
        return output.value

    def stop_output(self, output: SupportsIndex) -> None:
        """Stop an output: send or write what it has queued, finish its file, close its sockets (up to about 3 s).

        Raises CameraError (KEEL_E_PERIPHERAL) when the output had failed or its file could not be finished; it is
        stopped anyway.
        """
        self._check(g.keel_camera_output_stop(self._natives, self._state.token, output), "keel_camera_output_stop")

    def output_stats(self, output: SupportsIndex) -> OutputStats:
        """Return the state and counters of one output: clients, bytes and chunks sent, chunks dropped, any failure."""
        raw = g.OutputStatsStruct()
        code = g.keel_camera_get_output_stats(self._natives, self._state.token, output, ctypes.byref(raw))
        self._check(code, "keel_camera_get_output_stats")
        return g.convert_output_stats(raw)

    # ------------------------------------------------------------------ stills ----

    def _still_options(
        self, width: SupportsIndex, height: SupportsIndex, quality: SupportsIndex, timeout: float
    ) -> g.StillOptionsStruct:
        return g.fill_still_options(
            g.StillOptionsStruct(),
            width=width,
            height=height,
            quality=quality,
            timeout_ms=_milliseconds("timeout", timeout),
            reserved0=0,
        )

    def capture_still(
        self, *, width: SupportsIndex = 0, height: SupportsIndex = 0, quality: SupportsIndex = 0, timeout: float = 0.0
    ) -> StillImage:
        """Capture one JPEG image (keel_camera_capture_still) and return it.

        width and height are both 0 (the full sensor resolution) or both set; quality is 1..100 (0 = 85); timeout
        limits the whole capture in seconds (0 = 5 s). Exposure, white balance and, on an autofocus camera, focus
        settle first, so a still takes about a second. Up to 1920x1920 the hardware JPEG encoder does the work, above
        it libjpeg (StillInfo.software). Needs the capture stopped (CameraStateError otherwise); the next start()
        returns to the video stream.
        """
        options = self._still_options(width, height, quality, timeout)
        size = _still_buffer_size(
            options.width or self._info.pixel_array_width, options.height or self._info.pixel_array_height
        )
        buffer = bytearray(size)
        raw = g.StillInfoStruct()
        code = g.keel_camera_capture_still(
            self._natives,
            self._state.token,
            ctypes.byref(options),
            (ctypes.c_ubyte * size).from_buffer(buffer),
            size,
            ctypes.byref(raw),
        )
        self._check(code, "keel_camera_capture_still")
        info = g.convert_still_info(raw)
        return StillImage(info, bytes(memoryview(buffer)[: info.bytes]))

    def capture_still_file(
        self,
        path: str | os.PathLike[str],
        *,
        width: SupportsIndex = 0,
        height: SupportsIndex = 0,
        quality: SupportsIndex = 0,
        timeout: float = 0.0,
    ) -> StillInfo:
        """Capture one JPEG image into a file, created or replaced (keel_camera_capture_still_file).

        The options are those of capture_still().
        """
        options = self._still_options(width, height, quality, timeout)
        encoded = g.encode_path("keel_camera_capture_still_file", "path", os.fspath(path), limits.PATH_MAX)
        raw = g.StillInfoStruct()
        code = g.keel_camera_capture_still_file(
            self._natives, self._state.token, ctypes.byref(options), encoded, ctypes.byref(raw)
        )
        self._check(code, "keel_camera_capture_still_file")
        return g.convert_still_info(raw)


def list_cameras() -> list[ListedCamera]:
    """Return the cameras present, with their models, capabilities and sensor modes (keel_camera_list_*).

    Each camera that no process holds is briefly acquired to read the frame-rate limit and crop of every mode; a held
    camera has CameraInfo.in_use and zero rates. No camera at all is an empty list; a camera that is connected but
    cannot be used raises with the reason (CameraPermissionError for a user outside the video group,
    CameraNotFoundError for a missing IPA module). Takes up to about 2 s with a camera connected. Loads the camera
    library first if necessary (CameraLibraryError).
    """
    library = native_library.get()
    natives = library.natives
    handle = ctypes.c_void_p()
    record = g.ErrorReportStruct()
    code = g.keel_camera_list_open(natives, ctypes.byref(handle), ctypes.byref(record))
    if code != Status.OK:
        raise error_from_record(code, "keel_camera_list_open", record)
    token = handle.value
    try:
        count = ctypes.c_uint32()
        check(natives, 0, g.keel_camera_list_count(natives, token, ctypes.byref(count)), "keel_camera_list_count")
        cameras = []
        for index in range(count.value):
            raw = g.CameraInfoStruct()
            check(natives, 0, g.keel_camera_list_get(natives, token, index, ctypes.byref(raw)), "keel_camera_list_get")
            info = g.convert_camera_info(raw)
            modes = []
            for mode in range(info.mode_count):
                raw_mode = g.SensorModeStruct()
                code = g.keel_camera_list_get_mode(natives, token, index, mode, ctypes.byref(raw_mode))
                check(natives, 0, code, "keel_camera_list_get_mode")
                modes.append(g.convert_sensor_mode(raw_mode))
            cameras.append(ListedCamera(info, tuple(modes)))
        return cameras
    finally:
        code = g.keel_camera_list_close(natives, token)
        if code != Status.OK:
            _log.warning("keel_camera_list_close: %s", status_name(code))


def open(  # noqa: A001  (keel.camera.open, like the C API's keel_camera_open)
    index: SupportsIndex = 0,
    *,
    width: SupportsIndex = limits.WIDTH_DEFAULT,
    height: SupportsIndex = limits.HEIGHT_DEFAULT,
    framerate: float = limits.FRAMERATE_DEFAULT_MILLI / 1000,
    pixel_format: SupportsIndex = PixelFormat.I420,
    hflip: bool = False,
    vflip: bool = False,
    rotation: SupportsIndex = 0,
    af_mode: SupportsIndex = AutofocusMode.DEFAULT,
    af_range: SupportsIndex = AutofocusRange.DEFAULT,
    lens_position: float = 0.0,
    exposure_us: SupportsIndex = 0,
    gain: float = 0.0,
    ev: float = 0.0,
    awb_mode: SupportsIndex = WhiteBalance.AUTO,
    red_gain: float = 0.0,
    blue_gain: float = 0.0,
    frame_queue: SupportsIndex = limits.FRAME_QUEUE_DEFAULT,
) -> Camera:
    """Open camera index (keel_camera_open; up to about 1 s; the calling thread owns the camera).

    The options are the fields of keel_camera_options, with fractions as floats: width and height (even, 64..16384;
    the camera may adjust them, see Camera.stream_info()), framerate in frames per second (1..240, capped by the
    sensor mode), pixel_format (I420, or RGB888 and BGR888 converted by the image signal processor at no CPU cost),
    hflip, vflip and rotation (0 or 180), exposure_us (0 = automatic), gain (analogue gain, 1..64; 0 = automatic), ev
    (exposure compensation, -8..8), awb_mode with red_gain and blue_gain for WhiteBalance.MANUAL, and frame_queue
    (raw frames kept, 1..8; 1 always gives the newest frame).

    Focus, on cameras with autofocus (Camera Module 3) only: af_mode, af_range and, with AutofocusMode.MANUAL,
    lens_position in dioptres (0 = infinity, 1 = 1 m, 10 = 10 cm). On a fixed-focus camera such as Camera Module 2
    any of them raises CameraUnsupportedError.

    Raises CameraNotFoundError (no camera with this index), CameraBusyError (another process or handle has it),
    CameraPermissionError (the user is not in the video group), CameraArgumentError (an option out of range) or
    CameraError. Loads the camera library first if necessary (CameraLibraryError).
    """
    library = native_library.get()
    flags = (CameraFlag.HFLIP if hflip else CameraFlag(0)) | (CameraFlag.VFLIP if vflip else CameraFlag(0))
    options = g.fill_camera_options(
        g.CameraOptionsStruct(),
        index=index,
        width=width,
        height=height,
        framerate_milli=_milli("framerate", framerate),
        format_=pixel_format,
        flags=flags,
        rotation=rotation,
        af_mode=af_mode,
        af_range=af_range,
        lens_position_milli=_milli("lens_position", lens_position),
        exposure_us=exposure_us,
        gain_milli=_milli("gain", gain),
        ev_milli=_milli("ev", ev),
        awb_mode=awb_mode,
        red_gain_milli=_milli("red_gain", red_gain),
        blue_gain_milli=_milli("blue_gain", blue_gain),
        frame_queue=frame_queue,
    )
    handle = ctypes.c_void_p()
    record = g.ErrorReportStruct()
    code = g.keel_camera_open(library.natives, ctypes.byref(options), ctypes.byref(handle), ctypes.byref(record))
    if code != Status.OK:
        raise error_from_record(code, "keel_camera_open", record)
    if not handle.value:
        raise camera_error(Status.E_INVALID_HANDLE, "keel_camera_open")
    return Camera._create(library, handle.value)
