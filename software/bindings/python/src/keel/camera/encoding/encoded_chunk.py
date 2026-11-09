# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""EncodedChunk: one encoded frame from the camera's hardware encoder."""

from __future__ import annotations

from dataclasses import dataclass

from ._generated import ChunkInfo, Codec

__all__ = ["EncodedChunk"]


@dataclass(frozen=True, slots=True)
class EncodedChunk:
    """One encoded frame: an H.264 access unit in Annex-B form, or one JPEG image of an MJPEG stream.

    Chunks concatenated in order are a valid stream: H.264 that FFmpeg reads with -f h264, or MJPEG that it reads with
    -f mjpeg. The first chunk after start() is a keyframe with the H.264 parameter sets.
    """

    info: ChunkInfo
    """The codec, flags, size, sequence number and timestamp keel_camera_read_encoded wrote."""

    data: bytes
    """The encoded bytes (info.bytes of them)."""

    def __bytes__(self) -> bytes:
        """Return the encoded bytes."""
        return self.data

    def __len__(self) -> int:
        """Return the number of encoded bytes."""
        return len(self.data)

    @property
    def codec(self) -> Codec:
        """H264 or MJPEG."""
        return self.info.codec

    @property
    def keyframe(self) -> bool:
        """True when the chunk decodes on its own (an H.264 IDR picture; every MJPEG image)."""
        return self.info.keyframe

    @property
    def sequence(self) -> int:
        """Sequence number of the frame it encodes."""
        return self.info.sequence

    @property
    def timestamp_ns(self) -> int:
        """Start of exposure of that frame, CLOCK_MONOTONIC nanoseconds."""
        return self.info.timestamp_ns
