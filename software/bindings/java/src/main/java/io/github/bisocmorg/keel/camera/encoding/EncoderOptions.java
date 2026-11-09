// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.encoding;

import java.util.Objects;

/**
 * Options for {@code Camera.setEncoder}, the fields of {@code keel_camera_encoder_options}. Every zero means
 * "default"; libkeel-camera checks every value.
 *
 * <p>{@code EncoderOptions.h264()} and {@code EncoderOptions.mjpeg()} are the defaults of each codec;
 * {@code EncoderOptions.builder(Codec.H264).bitrate(2_000_000).intraPeriod(15).build()} sets more.
 *
 * @param codec {@link Codec#H264} or {@link Codec#MJPEG}; {@link Codec#NONE} removes the encoder
 * @param bitrate H.264 bits per second, 25000..25000000 (0: 0.15 bit per pixel and frame, 1.4 Mbit/s at 640x480 and
 *     30 fps); 0 for MJPEG
 * @param intraPeriod H.264 frames from one keyframe to the next, 1..1000 (0: one second of frames); 0 for MJPEG
 * @param profile the H.264 profile (default High); {@link H264Profile#DEFAULT} for MJPEG
 * @param inlineHeaders H.264: repeat the parameter sets before every keyframe of the chunks the program reads
 *     (outputs always get them)
 * @param quality MJPEG quality 1..100 (0: 85); 0 for H.264
 * @param encodedQueue chunks kept for {@code Camera.readChunk}, 1..256 (0: 16)
 */
public record EncoderOptions(Codec codec, int bitrate, int intraPeriod, H264Profile profile, boolean inlineHeaders,
        int quality, int encodedQueue) {

    /**
     * Validates the options.
     *
     * @throws NullPointerException if the codec or the profile is {@code null}
     * @throws IllegalArgumentException for an {@code UNRECOGNIZED} constant or a negative number
     */
    public EncoderOptions {
        Objects.requireNonNull(codec, "codec");
        Objects.requireNonNull(profile, "profile");
        if (codec == Codec.UNRECOGNIZED || profile == H264Profile.UNRECOGNIZED) {
            throw new IllegalArgumentException("UNRECOGNIZED is not an encoder option");
        }
        if (bitrate < 0 || intraPeriod < 0 || quality < 0 || encodedQueue < 0) {
            throw new IllegalArgumentException("encoder options must not be negative");
        }
    }

    /**
     * H.264 with every default.
     *
     * @return the options
     */
    public static EncoderOptions h264() {
        return builder(Codec.H264).build();
    }

    /**
     * MJPEG with every default.
     *
     * @return the options
     */
    public static EncoderOptions mjpeg() {
        return builder(Codec.MJPEG).build();
    }

    /**
     * Starts a builder for a codec, with every other option at its default.
     *
     * @param codec the codec
     * @return a new builder
     */
    public static Builder builder(Codec codec) {
        return new Builder(codec);
    }

    /** Builds {@link EncoderOptions}; every setter returns the builder. */
    public static final class Builder {
        private final Codec codec;
        private int bitrate;
        private int intraPeriod;
        private H264Profile profile = H264Profile.DEFAULT;
        private boolean inlineHeaders;
        private int quality;
        private int encodedQueue;

        private Builder(Codec codec) {
            this.codec = codec;
        }

        /**
         * Sets the H.264 bit rate.
         *
         * @param value see {@link EncoderOptions#bitrate()}
         * @return this builder
         */
        public Builder bitrate(int value) {
            bitrate = value;
            return this;
        }

        /**
         * Sets the H.264 keyframe interval.
         *
         * @param value see {@link EncoderOptions#intraPeriod()}
         * @return this builder
         */
        public Builder intraPeriod(int value) {
            intraPeriod = value;
            return this;
        }

        /**
         * Sets the H.264 profile.
         *
         * @param value see {@link EncoderOptions#profile()}
         * @return this builder
         */
        public Builder profile(H264Profile value) {
            profile = value;
            return this;
        }

        /**
         * Sets whether the H.264 parameter sets precede every keyframe the program reads.
         *
         * @param value see {@link EncoderOptions#inlineHeaders()}
         * @return this builder
         */
        public Builder inlineHeaders(boolean value) {
            inlineHeaders = value;
            return this;
        }

        /**
         * Sets the MJPEG quality.
         *
         * @param value see {@link EncoderOptions#quality()}
         * @return this builder
         */
        public Builder quality(int value) {
            quality = value;
            return this;
        }

        /**
         * Sets the encoded queue.
         *
         * @param value see {@link EncoderOptions#encodedQueue()}
         * @return this builder
         */
        public Builder encodedQueue(int value) {
            encodedQueue = value;
            return this;
        }

        /**
         * Builds the options.
         *
         * @return the options
         * @throws NullPointerException if the codec or the profile is {@code null}
         * @throws IllegalArgumentException if a value is refused (see the record constructor)
         */
        public EncoderOptions build() {
            return new EncoderOptions(codec, bitrate, intraPeriod, profile, inlineHeaders, quality, encodedQueue);
        }
    }
}
