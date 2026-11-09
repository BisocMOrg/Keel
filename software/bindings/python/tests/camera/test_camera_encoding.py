# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The hardware encoders, through the synthetic encoder: H.264 access units and MJPEG images read by the program."""

from __future__ import annotations

from collections.abc import Callable, Iterator

import pytest

from keel import camera
from keel.errors import Status


def nal_types(data: bytes) -> list[int]:
    """The NAL unit types of an Annex-B byte stream, in order."""
    types = []
    index = data.find(b"\x00\x00\x01")
    while index >= 0:
        types.append(data[index + 3] & 0x1F)
        index = data.find(b"\x00\x00\x01", index + 3)
    return types


def read_until_it_fails(cam: camera.Camera) -> None:
    """Read chunks until the encoder raises; fail if ten chunks arrive."""
    for _ in range(10):
        cam.read_chunk(timeout=2)
    pytest.fail("the encoder did not fail")


@pytest.fixture
def h264() -> Iterator[camera.Camera]:
    with camera.open(width=320, height=240, framerate=120) as cam:
        cam.set_encoder(camera.Codec.H264, intra_period=4)
        yield cam


def test_the_encoder_sets_the_chunk_buffer_size(h264: camera.Camera) -> None:
    stream = h264.stream_info()
    assert stream.codec == camera.Codec.H264
    assert stream.encoded_bytes_max > 0


def test_h264_chunks_start_with_a_keyframe_and_its_parameter_sets(h264: camera.Camera) -> None:
    h264.start()
    chunks = [h264.read_chunk(timeout=2) for _ in range(9)]
    first = chunks[0]
    assert first.codec == camera.Codec.H264
    assert first.keyframe
    assert first.info.headers
    assert first.info.flags == camera.ChunkFlag.KEYFRAME | camera.ChunkFlag.HEADERS
    assert nal_types(first.data)[:2] == [7, 8]
    assert 5 in nal_types(first.data)
    assert first.data.startswith(b"\x00\x00\x00\x01")
    assert len(first) == first.info.bytes == len(bytes(first))
    keyframes = [chunk.keyframe for chunk in chunks]
    assert keyframes == [True, False, False, False, True, False, False, False, True]
    assert not chunks[4].info.headers  # parameter sets only at the start without INLINE_HEADERS
    assert [chunk.sequence for chunk in chunks] == sorted(chunk.sequence for chunk in chunks)
    assert chunks[1].timestamp_ns > first.timestamp_ns
    stats = h264.stats()
    assert stats.chunks_read == 9
    assert stats.chunks_encoded >= 9


def test_inline_headers_repeat_the_parameter_sets_at_every_keyframe() -> None:
    with camera.open(width=320, height=240, framerate=120) as cam:
        cam.set_encoder(camera.Codec.H264, intra_period=3, inline_headers=True, profile=camera.H264Profile.BASELINE)
        cam.start()
        chunks = [cam.read_chunk(timeout=2) for _ in range(7)]
    assert all(chunk.info.headers for chunk in chunks if chunk.keyframe)
    assert [chunk.keyframe for chunk in chunks] == [True, False, False, True, False, False, True]


def test_chunks_iterates_until_the_camera_is_closed(h264: camera.Camera) -> None:
    h264.start()
    count = 0
    for chunk in h264.chunks():
        assert chunk.codec == camera.Codec.H264
        count += 1
        if count == 4:
            h264.close()
    assert count == 4


def test_mjpeg_chunks_are_whole_jpeg_images() -> None:
    with camera.open(width=320, height=240, framerate=120) as cam:
        cam.set_encoder(camera.Codec.MJPEG, quality=70)
        cam.start()
        for _ in range(3):
            chunk = cam.read_chunk(timeout=2)
            assert chunk.codec == camera.Codec.MJPEG
            assert chunk.keyframe
            assert chunk.data.startswith(b"\xff\xd8")
            assert chunk.data.endswith(b"\xff\xd9")


def test_the_encoder_can_be_removed_again() -> None:
    with camera.open(width=320, height=240, framerate=120) as cam:
        cam.set_encoder(camera.Codec.MJPEG)
        cam.set_encoder(camera.Codec.NONE)
        assert cam.stream_info().codec == camera.Codec.NONE
        cam.start()
        with pytest.raises(camera.CameraStateError, match="encoder"):
            cam.read_chunk(timeout=0.2)


def test_reading_chunks_without_an_encoder_is_a_state_error(cam: camera.Camera) -> None:
    cam.start()
    with pytest.raises(camera.CameraStateError):
        cam.read_chunk()


@pytest.mark.parametrize(
    ("codec", "options"),
    [
        (camera.Codec.H264, {"quality": 50}),
        (camera.Codec.MJPEG, {"bitrate": 1_000_000}),
        (camera.Codec.MJPEG, {"intra_period": 10}),
        (camera.Codec.H264, {"bitrate": 1000}),
        (camera.Codec.H264, {"profile": 9}),
        (9, {}),
    ],
)
def test_options_for_the_other_codec_or_out_of_range_are_refused(
    cam: camera.Camera, codec: int, options: dict[str, int]
) -> None:
    with pytest.raises(camera.CameraArgumentError) as caught:
        cam.set_encoder(codec, **options)  # type: ignore[arg-type]
    assert caught.value.function == "keel_camera_set_encoder"


def test_the_encoder_cannot_change_while_capturing(cam: camera.Camera) -> None:
    cam.start()
    with pytest.raises(camera.CameraStateError):
        cam.set_encoder(camera.Codec.H264)


def test_a_stream_larger_than_the_hardware_encoder_takes_is_unsupported() -> None:
    with camera.open(width=2304, height=1296) as cam, pytest.raises(camera.CameraUnsupportedError, match="1920"):
        cam.set_encoder(camera.Codec.H264)


def test_a_bitrate_beyond_uint32_is_refused_before_the_call(cam: camera.Camera) -> None:
    with pytest.raises(OverflowError, match="bitrate"):
        cam.set_encoder(camera.Codec.H264, bitrate=1 << 32)


def test_a_missing_encoder_is_not_found(fault: Callable[[str], None]) -> None:
    fault("encoder-missing")
    with camera.open(width=320, height=240) as cam, pytest.raises(camera.CameraNotFoundError):
        cam.set_encoder(camera.Codec.H264)


def test_an_encoder_that_fails_while_running(fault: Callable[[str], None]) -> None:
    fault("encoder-fail")
    with camera.open(width=320, height=240, framerate=120) as cam:
        cam.set_encoder(camera.Codec.H264)
        cam.start()
        with pytest.raises(camera.CameraError) as caught:
            read_until_it_fails(cam)
        assert caught.value.status == Status.E_PERIPHERAL
        assert cam.stats().state == camera.CaptureState.FAILED
