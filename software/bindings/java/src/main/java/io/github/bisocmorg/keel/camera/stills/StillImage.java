// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.stills;

import java.nio.ByteBuffer;
import java.util.Objects;

/**
 * One JPEG still from {@code Camera.captureStill} and its {@link StillInfo} (size, encoder and timestamp). Write
 * {@link #toByteArray()} to a {@code .jpg} file, or decode {@link #data()} with {@code javax.imageio.ImageIO}.
 */
public final class StillImage {
    private final StillInfo info;
    private final byte[] jpeg;

    /**
     * Wraps a JPEG image; the array is copied.
     *
     * @param info the size, flags and timestamp
     * @param jpeg the JPEG file contents
     */
    public StillImage(StillInfo info, byte[] jpeg) {
        this.info = Objects.requireNonNull(info, "info");
        this.jpeg = Objects.requireNonNull(jpeg, "jpeg").clone();
    }

    /**
     * Returns the size, flags and timestamp the still capture wrote.
     *
     * @return the still info
     */
    public StillInfo info() {
        return info;
    }

    /**
     * Returns the width.
     *
     * @return pixels
     */
    public int width() {
        return (int) info.width();
    }

    /**
     * Returns the height.
     *
     * @return lines
     */
    public int height() {
        return (int) info.height();
    }

    /**
     * Returns the JPEG file contents, with no copy.
     *
     * @return a read-only buffer, position 0
     */
    public ByteBuffer data() {
        return ByteBuffer.wrap(jpeg).asReadOnlyBuffer();
    }

    /**
     * Returns a copy of the JPEG file contents.
     *
     * @return a new array
     */
    public byte[] toByteArray() {
        return jpeg.clone();
    }

    @Override
    public String toString() {
        return "StillImage[" + info.width() + "x" + info.height() + ", " + jpeg.length + " bytes"
                + (info.software() ? ", libjpeg]" : "]");
    }
}
