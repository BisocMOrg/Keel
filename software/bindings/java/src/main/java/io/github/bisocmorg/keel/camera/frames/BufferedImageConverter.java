// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.frames;

import java.awt.image.BufferedImage;
import java.awt.image.DataBufferByte;
import java.nio.ByteBuffer;

/**
 * Frames as {@code java.awt} images. The only class of the binding that uses {@code java.desktop}, which the module
 * requires statically: {@link Frame#toBufferedImage()} reports its absence.
 */
final class BufferedImageConverter {
    private BufferedImageConverter() {
    }

    /**
     * A {@code TYPE_3BYTE_BGR} image of the frame: its raster stores each pixel as the bytes B, G, R.
     *
     * @param frame any frame
     * @return a new image
     */
    static BufferedImage convert(Frame frame) {
        Frame bgr = frame.pixelFormat() == PixelFormat.I420 ? frame.convert(PixelFormat.BGR888) : frame;
        int width = bgr.width();
        int height = bgr.height();
        BufferedImage image = new BufferedImage(width, height, BufferedImage.TYPE_3BYTE_BGR);
        byte[] pixels = ((DataBufferByte) image.getRaster().getDataBuffer()).getData();
        ByteBuffer data = bgr.data();
        int stride = (int) bgr.info().stride0();
        int offset = (int) bgr.info().offset0();
        int rowBytes = width * 3;
        for (int row = 0; row < height; row++) {
            data.get(offset + row * stride, pixels, row * rowBytes, rowBytes);
        }
        if (bgr.pixelFormat() == PixelFormat.RGB888) {
            for (int at = 0; at < pixels.length; at += 3) {
                byte red = pixels[at];
                pixels[at] = pixels[at + 2];
                pixels[at + 2] = red;
            }
        }
        return image;
    }
}
