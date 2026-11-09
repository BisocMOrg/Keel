// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.outputs;

/**
 * Options for {@code Camera.startOutput}, the fields of {@code keel_camera_output_options}. Every zero means
 * "default"; libkeel-camera checks every value, and an option meant for another kind of output is refused.
 *
 * @param queue chunks queued for the output (for each client of a listening output), 1..256 (0: 64)
 * @param maxClients a listening TCP output's clients at once, 1..16 (0: 4); 0 for the others
 * @param datagramBytes a UDP output's payload bytes per datagram, 512..65507 (0: 1472); 0 for the others
 */
public record OutputOptions(int queue, int maxClients, int datagramBytes) {

    /**
     * Validates the options.
     *
     * @throws IllegalArgumentException for a negative number
     */
    public OutputOptions {
        if (queue < 0 || maxClients < 0 || datagramBytes < 0) {
            throw new IllegalArgumentException("output options must not be negative");
        }
    }

    /**
     * Every default.
     *
     * @return the options
     */
    public static OutputOptions defaults() {
        return new OutputOptions(0, 0, 0);
    }

    /**
     * These options with another queue.
     *
     * @param value see {@link #queue()}
     * @return new options
     */
    public OutputOptions withQueue(int value) {
        return new OutputOptions(value, maxClients, datagramBytes);
    }

    /**
     * These options with another client limit.
     *
     * @param value see {@link #maxClients()}
     * @return new options
     */
    public OutputOptions withMaxClients(int value) {
        return new OutputOptions(queue, value, datagramBytes);
    }

    /**
     * These options with another datagram size.
     *
     * @param value see {@link #datagramBytes()}
     * @return new options
     */
    public OutputOptions withDatagramBytes(int value) {
        return new OutputOptions(queue, maxClients, value);
    }
}
