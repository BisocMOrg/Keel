// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.stills;

import java.time.Duration;
import java.util.Objects;

/**
 * Options for {@code Camera.captureStill} and {@code Camera.captureStillFile}, the fields of
 * {@code keel_camera_still_options}. Every zero means "default"; libkeel-camera checks every value.
 *
 * @param width the width, even, 64..16384; 0 with a height of 0 is the full sensor resolution
 * @param height the height, even; 0 with a width of 0 is the full sensor resolution
 * @param quality the JPEG quality 1..100 (0: 85)
 * @param timeout the longest time the capture may take, up to 60 s ({@link Duration#ZERO}: 5 s)
 */
public record StillOptions(int width, int height, int quality, Duration timeout) {

    /**
     * Validates the options.
     *
     * @throws NullPointerException if the timeout is {@code null}
     * @throws IllegalArgumentException for a negative number or timeout
     */
    public StillOptions {
        Objects.requireNonNull(timeout, "timeout");
        if (width < 0 || height < 0 || quality < 0 || timeout.isNegative()) {
            throw new IllegalArgumentException("still options must not be negative");
        }
    }

    /**
     * A full-resolution still with every default.
     *
     * @return the options
     */
    public static StillOptions defaults() {
        return new StillOptions(0, 0, 0, Duration.ZERO);
    }

    /**
     * A still of a size, with every other option at its default.
     *
     * @param width the width
     * @param height the height
     * @return the options
     */
    public static StillOptions of(int width, int height) {
        return new StillOptions(width, height, 0, Duration.ZERO);
    }

    /**
     * These options with another quality.
     *
     * @param value see {@link #quality()}
     * @return new options
     */
    public StillOptions withQuality(int value) {
        return new StillOptions(width, height, value, timeout);
    }

    /**
     * These options with another timeout.
     *
     * @param value see {@link #timeout()}
     * @return new options
     */
    public StillOptions withTimeout(Duration value) {
        return new StillOptions(width, height, quality, value);
    }
}
