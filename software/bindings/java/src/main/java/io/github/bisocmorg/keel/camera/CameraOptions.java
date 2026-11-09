// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import java.util.Objects;

/**
 * Options for {@link Camera#open(CameraOptions)}, the fields of {@code keel_camera_options} with fractions as
 * {@code double}. Zero values of the numeric options mean "default", as in the C struct; libkeel-camera checks every
 * value against the camera. The binding refuses here only what the C types cannot hold: negative or non-finite
 * numbers, and values beyond the 32-bit fields.
 *
 * <p>{@code CameraOptions.builder().width(1280).height(720).pixelFormat(PixelFormat.BGR888).build()} opens camera 0
 * at 1280x720 with frames in OpenCV's order.
 *
 * @param index the camera index from {@link Camera#listCameras()}; 0 is the first camera
 * @param width the frame width, even, 64..16384 (0: 640); the camera may adjust it ({@link Camera#streamInfo()})
 * @param height the frame height, even, 64..16384 (0: 480)
 * @param framerate frames per second, 1..240 (0: 30), capped by the sensor mode
 * @param pixelFormat I420, or RGB888 or BGR888 converted by the image signal processor at no CPU cost
 * @param hflip mirror the image left to right
 * @param vflip mirror the image top to bottom
 * @param rotation 0 or 180 degrees (180 equals both flips)
 * @param afMode the autofocus mode; anything but {@link AutofocusMode#DEFAULT} needs an autofocus camera (Camera
 *     Module 3)
 * @param afRange the autofocus range; anything but {@link AutofocusRange#DEFAULT} needs an autofocus camera
 * @param lensPosition with {@link AutofocusMode#MANUAL}: dioptres, 0..32 (0 is infinity, 1 is 1 m, 10 is 10 cm);
 *     must be 0 otherwise
 * @param exposureUs the exposure time in microseconds, 1..1000000 (0: automatic)
 * @param gain the analogue gain, 1..64 (0: automatic)
 * @param ev the exposure compensation of automatic exposure, -8..8 EV
 * @param awbMode the white balance; {@link WhiteBalance#MANUAL} uses {@code redGain} and {@code blueGain}
 * @param redGain with {@link WhiteBalance#MANUAL}: the red colour gain, 0.001..32; must be 0 otherwise
 * @param blueGain with {@link WhiteBalance#MANUAL}: the blue colour gain, 0.001..32; must be 0 otherwise
 * @param frameQueue raw frames kept for {@link Camera#readFrame()}, 1..8 (0: 1, always the newest frame)
 */
public record CameraOptions(int index, int width, int height, double framerate, PixelFormat pixelFormat, boolean hflip,
        boolean vflip, int rotation, AutofocusMode afMode, AutofocusRange afRange, double lensPosition, int exposureUs,
        double gain, double ev, WhiteBalance awbMode, double redGain, double blueGain, int frameQueue) {

    /**
     * Validates the options.
     *
     * @throws NullPointerException if an enum component is {@code null}
     * @throws IllegalArgumentException for an {@code UNRECOGNIZED} constant, a negative integer, a number that is not
     *     finite, or a value that does not fit its 32-bit field
     */
    public CameraOptions {
        Objects.requireNonNull(pixelFormat, "pixelFormat");
        Objects.requireNonNull(afMode, "afMode");
        Objects.requireNonNull(afRange, "afRange");
        Objects.requireNonNull(awbMode, "awbMode");
        if (pixelFormat == PixelFormat.UNRECOGNIZED || afMode == AutofocusMode.UNRECOGNIZED
                || afRange == AutofocusRange.UNRECOGNIZED || awbMode == WhiteBalance.UNRECOGNIZED) {
            throw new IllegalArgumentException("UNRECOGNIZED is not an option value");
        }
        requireNatural("index", index);
        requireNatural("width", width);
        requireNatural("height", height);
        requireNatural("rotation", rotation);
        requireNatural("exposureUs", exposureUs);
        requireNatural("frameQueue", frameQueue);
        milli("framerate", framerate, false);
        milli("lensPosition", lensPosition, false);
        milli("gain", gain, false);
        milli("ev", ev, true);
        milli("redGain", redGain, false);
        milli("blueGain", blueGain, false);
    }

    private static void requireNatural(String name, int value) {
        if (value < 0) {
            throw new IllegalArgumentException(name + " must not be negative, not " + value);
        }
    }

    /**
     * A fractional option in the header's x 1000 scale, as the bit pattern of its 32-bit field.
     *
     * @param name the option, for the message
     * @param value the value
     * @param signed whether the field is {@code int32_t}
     * @return {@code round(value * 1000)}
     */
    static int milli(String name, double value, boolean signed) {
        if (!Double.isFinite(value)) {
            throw new IllegalArgumentException(name + " must be a finite number, not " + value);
        }
        long scaled = Math.round(value * 1000);
        long low = signed ? Integer.MIN_VALUE : 0;
        long high = signed ? Integer.MAX_VALUE : 0xFFFF_FFFFL;
        if (scaled < low || scaled > high) {
            throw new IllegalArgumentException(name + " = " + value + " does not fit "
                    + (signed ? "int32_t" : "uint32_t") + " x 1000");
        }
        return (int) scaled;
    }

    /**
     * The defaults: camera 0, 640x480 I420 at 30 fps, no flips, the camera's own focus, exposure and white balance,
     * and a queue of one frame. Equal to {@code keel_camera_open(NULL, ...)}.
     *
     * @return the default options
     */
    public static CameraOptions defaults() {
        return new CameraOptions(0, CameraLimits.WIDTH_DEFAULT, CameraLimits.HEIGHT_DEFAULT,
                CameraLimits.FRAMERATE_DEFAULT_MILLI / 1000.0, PixelFormat.I420, false, false, 0, AutofocusMode.DEFAULT,
                AutofocusRange.DEFAULT, 0, 0, 0, 0, WhiteBalance.AUTO, 0, 0, CameraLimits.FRAME_QUEUE_DEFAULT);
    }

    /**
     * Starts a builder with the defaults.
     *
     * @return a new builder
     */
    public static Builder builder() {
        return new Builder(defaults());
    }

    /**
     * Starts a builder with these options.
     *
     * @return a new builder
     */
    public Builder toBuilder() {
        return new Builder(this);
    }

    /** Builds {@link CameraOptions}; every setter returns the builder. */
    public static final class Builder {
        private int index;
        private int width;
        private int height;
        private double framerate;
        private PixelFormat pixelFormat;
        private boolean hflip;
        private boolean vflip;
        private int rotation;
        private AutofocusMode afMode;
        private AutofocusRange afRange;
        private double lensPosition;
        private int exposureUs;
        private double gain;
        private double ev;
        private WhiteBalance awbMode;
        private double redGain;
        private double blueGain;
        private int frameQueue;

        private Builder(CameraOptions from) {
            index = from.index;
            width = from.width;
            height = from.height;
            framerate = from.framerate;
            pixelFormat = from.pixelFormat;
            hflip = from.hflip;
            vflip = from.vflip;
            rotation = from.rotation;
            afMode = from.afMode;
            afRange = from.afRange;
            lensPosition = from.lensPosition;
            exposureUs = from.exposureUs;
            gain = from.gain;
            ev = from.ev;
            awbMode = from.awbMode;
            redGain = from.redGain;
            blueGain = from.blueGain;
            frameQueue = from.frameQueue;
        }

        /**
         * Sets the camera index.
         *
         * @param value see {@link CameraOptions#index()}
         * @return this builder
         */
        public Builder index(int value) {
            index = value;
            return this;
        }

        /**
         * Sets the frame width.
         *
         * @param value see {@link CameraOptions#width()}
         * @return this builder
         */
        public Builder width(int value) {
            width = value;
            return this;
        }

        /**
         * Sets the frame height.
         *
         * @param value see {@link CameraOptions#height()}
         * @return this builder
         */
        public Builder height(int value) {
            height = value;
            return this;
        }

        /**
         * Sets the frame rate.
         *
         * @param value see {@link CameraOptions#framerate()}
         * @return this builder
         */
        public Builder framerate(double value) {
            framerate = value;
            return this;
        }

        /**
         * Sets the pixel format.
         *
         * @param value see {@link CameraOptions#pixelFormat()}
         * @return this builder
         */
        public Builder pixelFormat(PixelFormat value) {
            pixelFormat = value;
            return this;
        }

        /**
         * Sets the horizontal flip.
         *
         * @param value see {@link CameraOptions#hflip()}
         * @return this builder
         */
        public Builder hflip(boolean value) {
            hflip = value;
            return this;
        }

        /**
         * Sets the vertical flip.
         *
         * @param value see {@link CameraOptions#vflip()}
         * @return this builder
         */
        public Builder vflip(boolean value) {
            vflip = value;
            return this;
        }

        /**
         * Sets the rotation.
         *
         * @param value see {@link CameraOptions#rotation()}
         * @return this builder
         */
        public Builder rotation(int value) {
            rotation = value;
            return this;
        }

        /**
         * Sets the autofocus mode.
         *
         * @param value see {@link CameraOptions#afMode()}
         * @return this builder
         */
        public Builder afMode(AutofocusMode value) {
            afMode = value;
            return this;
        }

        /**
         * Sets the autofocus range.
         *
         * @param value see {@link CameraOptions#afRange()}
         * @return this builder
         */
        public Builder afRange(AutofocusRange value) {
            afRange = value;
            return this;
        }

        /**
         * Sets the manual lens position.
         *
         * @param value see {@link CameraOptions#lensPosition()}
         * @return this builder
         */
        public Builder lensPosition(double value) {
            lensPosition = value;
            return this;
        }

        /**
         * Sets the exposure time.
         *
         * @param value see {@link CameraOptions#exposureUs()}
         * @return this builder
         */
        public Builder exposureUs(int value) {
            exposureUs = value;
            return this;
        }

        /**
         * Sets the analogue gain.
         *
         * @param value see {@link CameraOptions#gain()}
         * @return this builder
         */
        public Builder gain(double value) {
            gain = value;
            return this;
        }

        /**
         * Sets the exposure compensation.
         *
         * @param value see {@link CameraOptions#ev()}
         * @return this builder
         */
        public Builder ev(double value) {
            ev = value;
            return this;
        }

        /**
         * Sets the white balance.
         *
         * @param value see {@link CameraOptions#awbMode()}
         * @return this builder
         */
        public Builder awbMode(WhiteBalance value) {
            awbMode = value;
            return this;
        }

        /**
         * Sets the manual red gain.
         *
         * @param value see {@link CameraOptions#redGain()}
         * @return this builder
         */
        public Builder redGain(double value) {
            redGain = value;
            return this;
        }

        /**
         * Sets the manual blue gain.
         *
         * @param value see {@link CameraOptions#blueGain()}
         * @return this builder
         */
        public Builder blueGain(double value) {
            blueGain = value;
            return this;
        }

        /**
         * Sets the raw frame queue.
         *
         * @param value see {@link CameraOptions#frameQueue()}
         * @return this builder
         */
        public Builder frameQueue(int value) {
            frameQueue = value;
            return this;
        }

        /**
         * Builds the options.
         *
         * @return the options
         * @throws NullPointerException if an enum option is {@code null}
         * @throws IllegalArgumentException if a value is refused (see the record constructor)
         */
        public CameraOptions build() {
            return new CameraOptions(index, width, height, framerate, pixelFormat, hflip, vflip, rotation, afMode,
                    afRange, lensPosition, exposureUs, gain, ev, awbMode, redGain, blueGain, frameQueue);
        }
    }
}
