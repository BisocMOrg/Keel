// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import static org.junit.jupiter.api.Assertions.assertEquals;

import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.camera.frames.FrameInfo;
import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import java.nio.ByteBuffer;

/**
 * The pattern of the synthetic camera's frames (libkeel SPEC section 26.9), and the I420 conversion of camera.h,
 * written independently of the binding.
 *
 * <p>With {@code n} the low byte of the sequence number and (x, y) the position before the flips: I420
 * {@code Y = (7x + 3y + n) & 0xFF}, {@code U = (5x + n) & 0xFF} and {@code V = (5y + 2n) & 0xFF} at chroma sample
 * (x, y); RGB888 {@code R = (x + n) & 0xFF}, {@code G = (y + n) & 0xFF}, {@code B = (x + y) & 0xFF}. Padding bytes
 * are 0xA5.
 */
public final class CameraPatterns {
    /** The byte the synthetic camera writes into row padding. */
    public static final int PADDING = 0xA5;

    private CameraPatterns() {
    }

    /**
     * Checks every pixel and padding byte of a frame against the pattern.
     *
     * @param frame the frame
     * @param hflip whether the camera mirrored it left to right
     * @param vflip whether the camera mirrored it top to bottom
     */
    public static void check(Frame frame, boolean hflip, boolean vflip) {
        FrameInfo info = frame.info();
        ByteBuffer data = frame.data();
        int n = (int) (info.sequence() & 0xFF);
        int width = frame.width();
        int height = frame.height();
        for (int y = 0; y < height; y++) {
            int sy = vflip ? height - 1 - y : y;
            int row = (int) (info.offset0() + y * info.stride0());
            for (int x = 0; x < width; x++) {
                int sx = hflip ? width - 1 - x : x;
                if (frame.pixelFormat() == PixelFormat.I420) {
                    assertEquals((7 * sx + 3 * sy + n) & 0xFF, data.get(row + x) & 0xFF, "Y " + x + "," + y);
                } else {
                    int red = (sx + n) & 0xFF;
                    int green = (sy + n) & 0xFF;
                    int blue = (sx + sy) & 0xFF;
                    boolean bgr = frame.pixelFormat() == PixelFormat.BGR888;
                    assertEquals(bgr ? blue : red, data.get(row + 3 * x) & 0xFF, "pixel " + x + "," + y);
                    assertEquals(green, data.get(row + 3 * x + 1) & 0xFF);
                    assertEquals(bgr ? red : blue, data.get(row + 3 * x + 2) & 0xFF);
                }
            }
            int used = frame.pixelFormat() == PixelFormat.I420 ? width : width * 3;
            for (int x = used; x < info.stride0(); x++) {
                assertEquals(PADDING, data.get(row + x) & 0xFF, "padding");
            }
        }
        if (frame.pixelFormat() != PixelFormat.I420) {
            return;
        }
        for (int y = 0; y < height / 2; y++) {
            int sy = vflip ? height / 2 - 1 - y : y;
            for (int x = 0; x < width / 2; x++) {
                int sx = hflip ? width / 2 - 1 - x : x;
                assertEquals((5 * sx + n) & 0xFF, data.get((int) (info.offset1() + y * info.stride1() + x)) & 0xFF);
                assertEquals((5 * sy + 2 * n) & 0xFF, data.get((int) (info.offset2() + y * info.stride2() + x)) & 0xFF);
            }
        }
    }

    /**
     * Converts an I420 frame to packed RGB with the formula of camera.h.
     *
     * @param frame an I420 frame
     * @return width x height x 3 bytes, R, G, B
     */
    public static byte[] referenceRgb(Frame frame) {
        FrameInfo info = frame.info();
        ByteBuffer data = frame.data();
        int width = frame.width();
        int height = frame.height();
        byte[] rgb = new byte[width * height * 3];
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int luma = data.get((int) (info.offset0() + y * info.stride0() + x)) & 0xFF;
                int u = (data.get((int) (info.offset1() + (y / 2) * info.stride1() + x / 2)) & 0xFF) - 128;
                int v = (data.get((int) (info.offset2() + (y / 2) * info.stride2() + x / 2)) & 0xFF) - 128;
                int at = (y * width + x) * 3;
                rgb[at] = clamp(luma + ((91881 * v + 32768) >> 16));
                rgb[at + 1] = clamp(luma + ((-22554 * u - 46802 * v + 32768) >> 16));
                rgb[at + 2] = clamp(luma + ((116130 * u + 32768) >> 16));
            }
        }
        return rgb;
    }

    private static byte clamp(int value) {
        return (byte) Math.max(0, Math.min(255, value));
    }
}
