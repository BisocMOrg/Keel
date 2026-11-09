// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.encoding;

import java.nio.ByteBuffer;
import java.util.Objects;

/**
 * One encoded frame from the camera's hardware encoder: an H.264 access unit in Annex-B form, or one JPEG image of an
 * MJPEG stream.
 *
 * <p>Chunks concatenated in order are a valid stream: H.264 that FFmpeg reads with {@code -f h264}, or MJPEG that it
 * reads with {@code -f mjpeg}. The first chunk after {@code start()} is a keyframe with the H.264 parameter sets.
 */
public final class EncodedChunk {
    private final ChunkInfo info;
    private final byte[] data;

    /**
     * Wraps encoded bytes; the array is copied.
     *
     * @param info the codec, flags, size, sequence number and timestamp
     * @param data the encoded bytes
     */
    public EncodedChunk(ChunkInfo info, byte[] data) {
        this.info = Objects.requireNonNull(info, "info");
        this.data = Objects.requireNonNull(data, "data").clone();
    }

    /**
     * Returns the codec, flags, size, sequence number and timestamp {@code keel_camera_read_encoded} wrote.
     *
     * @return the chunk info
     */
    public ChunkInfo info() {
        return info;
    }

    /**
     * Returns the codec.
     *
     * @return {@link Codec#H264} or {@link Codec#MJPEG}
     */
    public Codec codec() {
        return info.codec();
    }

    /**
     * Returns whether the chunk decodes on its own: an H.264 IDR picture, or any MJPEG image.
     *
     * @return {@code true} for a keyframe
     */
    public boolean keyframe() {
        return info.keyframe();
    }

    /**
     * Returns the sequence number of the frame the chunk encodes.
     *
     * @return the sequence number
     */
    public long sequence() {
        return info.sequence();
    }

    /**
     * Returns the start of exposure of that frame on {@code CLOCK_MONOTONIC}.
     *
     * @return nanoseconds
     */
    public long timestampNs() {
        return info.timestampNs();
    }

    /**
     * Returns the number of encoded bytes.
     *
     * @return the size
     */
    public int size() {
        return data.length;
    }

    /**
     * Returns the encoded bytes, with no copy.
     *
     * @return a read-only buffer, position 0
     */
    public ByteBuffer data() {
        return ByteBuffer.wrap(data).asReadOnlyBuffer();
    }

    /**
     * Returns a copy of the encoded bytes.
     *
     * @return a new array
     */
    public byte[] toByteArray() {
        return data.clone();
    }

    @Override
    public String toString() {
        return "EncodedChunk[" + info.codec() + ", " + data.length + " bytes, sequence " + info.sequence()
                + (keyframe() ? ", keyframe]" : "]");
    }
}
