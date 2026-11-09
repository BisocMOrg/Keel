// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import io.github.bisocmorg.keel.OpenFlag;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import java.util.Objects;

/**
 * Resource configuration and wall-clock limits for a sequential task. All times are milliseconds.
 *
 * @param openOptions board options; the virtual clock is not supported by this wall-clock API
 * @param cameraOptions lazily opened camera options; BGR888 is required for consistent image analysis
 * @param maxMotionMs largest accepted duration of one movement, 1..86400000
 * @param armTimeoutMs maximum wait for the first arming handshake, 1..86400000
 * @param taskTimeoutMs maximum hardware-authority lifetime of the complete task, 1..86400000
 * @param cameraTimeoutMs maximum wait for one camera frame, 1..86400000, also capped by remaining task time
 */
public record TaskOptions(OpenOptions openOptions, CameraOptions cameraOptions, long maxMotionMs,
        long armTimeoutMs, long taskTimeoutMs, long cameraTimeoutMs) {
    /** Largest supported limit, one day in milliseconds. */
    public static final long MAX_LIMIT_MS = 86_400_000L;

    /**
     * Validates resource options and positive finite limits.
     *
     * @throws NullPointerException if resource options are null
     * @throws IllegalArgumentException if a limit is outside 1..86400000 or the board uses a virtual clock
     */
    public TaskOptions {
        Objects.requireNonNull(openOptions, "openOptions");
        Objects.requireNonNull(cameraOptions, "cameraOptions");
        if (cameraOptions.pixelFormat() != PixelFormat.BGR888) {
            throw new IllegalArgumentException("task camera frames must use BGR888; use the low-level camera API for other formats");
        }
        if (openOptions.flags().contains(OpenFlag.VIRTUAL_CLOCK)) {
            throw new IllegalArgumentException("sequential tasks require the real-time clock; VIRTUAL_CLOCK is unsupported");
        }
        limit("maxMotionMs", maxMotionMs);
        limit("armTimeoutMs", armTimeoutMs);
        limit("taskTimeoutMs", taskTimeoutMs);
        limit("cameraTimeoutMs", cameraTimeoutMs);
    }

    /**
     * Creates options with the real board defaults, BGR888 camera frames, 30 s motions, 3 s arming,
     * 5 min task lifetime and 1 s camera reads. Opening a camera is deferred until takeImage is used.
     *
     * @return immutable default options
     */
    public static TaskOptions defaults() {
        return new TaskOptions(OpenOptions.defaults(),
                CameraOptions.builder().pixelFormat(PixelFormat.BGR888).build(), 30_000, 3_000, 300_000, 1_000);
    }

    /**
     * Returns a copy with different board options, for example Backend.SIMULATOR on its real-time clock.
     *
     * @param value the board options
     * @return the updated options
     * @throws NullPointerException if value is null
     * @throws IllegalArgumentException if value requests the virtual clock
     */
    public TaskOptions withOpenOptions(OpenOptions value) {
        return new TaskOptions(value, cameraOptions, maxMotionMs, armTimeoutMs, taskTimeoutMs, cameraTimeoutMs);
    }

    /** Validates a positive millisecond limit before it can be converted to nanoseconds. */
    private static void limit(String name, long value) {
        if (value < 1 || value > MAX_LIMIT_MS) {
            throw new IllegalArgumentException(name + " must be 1.." + MAX_LIMIT_MS + " milliseconds");
        }
    }
}
