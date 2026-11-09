// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.frames;

import static java.lang.foreign.MemoryLayout.PathElement.groupElement;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.internal.camera.CameraErrorRegistry;
import io.github.bisocmorg.keel.internal.camera.CameraLayouts;
import io.github.bisocmorg.keel.internal.camera.CameraNativeLibrary;
import io.github.bisocmorg.keel.internal.camera.CameraStructCodec;
import java.awt.image.BufferedImage;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.util.Objects;

/**
 * One raw frame: its {@link FrameInfo} (format, size, strides, offsets, sequence number and timestamp) and its bytes.
 *
 * <p>Rows may be padded: plane {@code i} starts at {@code info().offsetI()} and its rows are {@code info().strideI()}
 * bytes apart. {@link #data()} and {@link #segment()} give the bytes as libkeel-camera wrote them, with no copy;
 * {@link #toByteArray()} gives the rows without padding; {@link #toBufferedImage()} gives an image for
 * {@code java.awt} and {@code javax.imageio}. The colour space is full-range BT.601 (sYCC), the JPEG colour space.
 *
 * <p>A frame from {@code Camera.readFrame} owns a direct buffer of its own: it stays valid after the camera is closed,
 * and later frames never overwrite it.
 */
public final class Frame {
    private final FrameInfo info;
    private final ByteBuffer buffer;

    /**
     * Wraps the bytes of a frame laid out as {@code info} describes.
     *
     * @param info the layout
     * @param data the bytes, from the buffer's position; at least {@code info.bytes()} of them
     * @throws IllegalArgumentException if {@code data} holds fewer bytes than {@code info.bytes()}
     */
    public Frame(FrameInfo info, ByteBuffer data) {
        this.info = Objects.requireNonNull(info, "info");
        Objects.requireNonNull(data, "data");
        if (data.remaining() < info.bytes()) {
            throw new IllegalArgumentException("the frame needs " + info.bytes() + " bytes, but data has "
                    + data.remaining());
        }
        this.buffer = data.slice();
    }

    /**
     * Returns the layout, sequence number and timestamp {@code keel_camera_read_frame} wrote.
     *
     * @return the frame info
     */
    public FrameInfo info() {
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
     * Returns the pixel format.
     *
     * @return {@link PixelFormat#I420}, {@link PixelFormat#RGB888} or {@link PixelFormat#BGR888}
     */
    public PixelFormat pixelFormat() {
        return info.format();
    }

    /**
     * Returns the sensor's frame sequence number; a gap is a frame the camera dropped.
     *
     * @return the sequence number
     */
    public long sequence() {
        return info.sequence();
    }

    /**
     * Returns the start of exposure on {@code CLOCK_MONOTONIC}, the clock of the robot's control loop.
     *
     * @return nanoseconds
     */
    public long timestampNs() {
        return info.timestampNs();
    }

    /**
     * Returns the frame's bytes as libkeel-camera wrote them, padding included, with no copy.
     *
     * @return a read-only buffer of {@code info().bytes()} bytes, position 0
     */
    public ByteBuffer data() {
        return buffer.slice(0, (int) info.bytes()).asReadOnlyBuffer();
    }

    /**
     * Returns the frame's bytes as a read-only memory segment, padding included, with no copy.
     *
     * @return a segment of {@code info().bytes()} bytes
     */
    public MemorySegment segment() {
        return MemorySegment.ofBuffer(data());
    }

    /**
     * Returns the pixels without row padding: the planes one after the other, each row exactly as wide as the
     * image, in the layout of FFmpeg's rawvideo {@code yuv420p}, {@code rgb24} and {@code bgr24}.
     *
     * @return a new array
     */
    public byte[] toByteArray() {
        long[][] planes = planes();
        int size = 0;
        for (long[] plane : planes) {
            size += (int) (plane[2] * plane[3]);
        }
        byte[] packed = new byte[size];
        int at = 0;
        for (long[] plane : planes) {
            int rowBytes = (int) plane[2];
            for (int row = 0; row < plane[3]; row++) {
                buffer.get((int) (plane[0] + row * plane[1]), packed, at, rowBytes);
                at += rowBytes;
            }
        }
        return packed;
    }

    /** {offset, stride, row bytes, rows} of each plane. */
    private long[][] planes() {
        PixelFormat format = info.format();
        if (format == PixelFormat.I420) {
            long halfWidth = (info.width() + 1) / 2;
            long halfHeight = (info.height() + 1) / 2;
            return new long[][] {{info.offset0(), info.stride0(), info.width(), info.height()},
                {info.offset1(), info.stride1(), halfWidth, halfHeight},
                {info.offset2(), info.stride2(), halfWidth, halfHeight}};
        }
        if (format == PixelFormat.RGB888 || format == PixelFormat.BGR888) {
            return new long[][] {{info.offset0(), info.stride0(), info.width() * 3, info.height()}};
        }
        throw new IllegalStateException("the pixel format " + info.formatValue() + " is not one this binding knows");
    }

    /**
     * Converts an I420 frame to RGB888 or BGR888 on the CPU ({@code keel_camera_convert_i420}).
     *
     * <p>Opening the camera with {@link PixelFormat#RGB888} or {@link PixelFormat#BGR888} instead lets the image
     * signal processor convert at no CPU cost; this is for a program that needs both. The new frame has the same size,
     * sequence number and timestamp, and rows without padding.
     *
     * @param target {@link PixelFormat#RGB888} or {@link PixelFormat#BGR888}
     * @return the converted frame
     * @throws IllegalArgumentException unless this frame is I420 and the target RGB888 or BGR888
     * @throws io.github.bisocmorg.keel.camera.errors.CameraArgumentException if the library refuses the layout
     */
    public Frame convert(PixelFormat target) {
        Objects.requireNonNull(target, "target");
        if (info.format() != PixelFormat.I420) {
            throw new IllegalArgumentException("convert() needs an I420 frame; this one is " + info.format());
        }
        if (target != PixelFormat.RGB888 && target != PixelFormat.BGR888) {
            throw new IllegalArgumentException("convert() converts to RGB888 or BGR888, not " + target);
        }
        CameraNativeLibrary library = CameraNativeLibrary.get();
        int stride = width() * 3;
        int size = stride * height();
        ByteBuffer output = ByteBuffer.allocateDirect(size);
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment source;
            if (buffer.isDirect() && !buffer.isReadOnly()) {
                source = MemorySegment.ofBuffer(buffer);
            } else {
                source = arena.allocate(info.bytes());
                MemorySegment.copy(MemorySegment.ofBuffer(data()), 0, source, 0, info.bytes());
            }
            MemorySegment layout = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_FRAME_INFO);
            write(layout, "format", info.formatValue());
            write(layout, "width", info.width());
            write(layout, "height", info.height());
            write(layout, "plane_count", info.planeCount());
            write(layout, "stride0", info.stride0());
            write(layout, "stride1", info.stride1());
            write(layout, "stride2", info.stride2());
            write(layout, "offset0", info.offset0());
            write(layout, "offset1", info.offset1());
            write(layout, "offset2", info.offset2());
            write(layout, "bytes", info.bytes());
            layout.set(JAVA_LONG, offset("sequence"), info.sequence());
            layout.set(JAVA_LONG, offset("timestamp_ns"), info.timestampNs());
            int code = library.convertI420(source, layout, MemorySegment.ofBuffer(output), size, target.code(),
                    stride);
            if (code != Status.OK.code()) {
                throw CameraErrorRegistry.errors().of(code, "keel_camera_convert_i420", null);
            }
        }
        FrameInfo converted = new FrameInfo(target.code(), info.width(), info.height(), 1, stride, 0, 0, 0, 0, 0, size,
                info.sequence(), info.timestampNs());
        return new Frame(converted, output);
    }

    private static long offset(String field) {
        return CameraLayouts.CAMERA_FRAME_INFO.byteOffset(groupElement(field));
    }

    private static void write(MemorySegment layout, String field, long value) {
        layout.set(JAVA_INT, offset(field), (int) value);
    }

    /**
     * Returns the frame as a {@code TYPE_3BYTE_BGR} image, for {@code java.awt}, Swing and {@code javax.imageio}.
     *
     * <p>BGR888 frames are copied as they are, RGB888 frames with their channels swapped, and I420 frames are
     * converted with {@link #convert(PixelFormat)} first. On the module path the application needs the
     * {@code java.desktop} module ({@code --add-modules java.desktop}, or {@code requires java.desktop} in its own
     * module); on the class path it is always there.
     *
     * @return a new image
     * @throws UnsupportedOperationException if the {@code java.desktop} module is not present
     */
    public BufferedImage toBufferedImage() {
        try {
            return BufferedImageConverter.convert(this);
        } catch (NoClassDefFoundError missing) {
            throw new UnsupportedOperationException(
                    "toBufferedImage() needs the java.desktop module: add --add-modules java.desktop", missing);
        }
    }

    @Override
    public String toString() {
        return "Frame[" + info.width() + "x" + info.height() + " " + info.format() + ", sequence " + info.sequence()
                + ", " + info.bytes() + " bytes]";
    }
}
