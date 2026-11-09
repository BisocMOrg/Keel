// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.EnumSet;
import java.util.Objects;
import java.util.Set;

/**
 * Options for {@link Keel#open(OpenOptions)}, the fields of {@code keel_open_options}. Every zero or empty value means
 * "default", as in the C struct.
 *
 * <p>{@code OpenOptions.builder().backend(Backend.SIMULATOR).flags(OpenFlag.VIRTUAL_CLOCK).build()} opens the
 * simulator on its virtual clock. The paths are validated here, before any call: each must fit, encoded as UTF-8, in
 * {@link Limits#PATH_MAX} bytes including the NUL, and contain no NUL. libkeel validates every other value.
 *
 * @param backend the backend; {@link Backend#AUTO} uses {@code KEEL_BACKEND}, else the board if the library has its
 *     backend, and never falls back to the simulator silently. {@link Backend#PORT} is refused by libkeel
 * @param flags the open flags; empty enables everything
 * @param commandTimeoutMs the motion-command lifetime in ms as its {@code uint32_t} bit pattern: 0 for
 *     {@link Limits#COMMAND_TIMEOUT_DEFAULT_MS}, else {@link Limits#COMMAND_TIMEOUT_MIN_MS} to
 *     {@link Limits#COMMAND_TIMEOUT_MAX_MS}
 * @param gpioChipPath the board's GPIO chip path; empty selects the chip labelled {@code pinctrl-bcm2711}. Must be
 *     empty for the simulator
 * @param i2cPath the board's I2C adapter path; empty selects {@code /dev/i2c-1}. Must be empty for the simulator
 */
public record OpenOptions(Backend backend, Set<OpenFlag> flags, int commandTimeoutMs, String gpioChipPath,
        String i2cPath) {

    /**
     * Validates the options.
     *
     * @throws NullPointerException if a component is {@code null}
     * @throws IllegalArgumentException if the backend is {@link Backend#UNRECOGNIZED}, or a path does not fit or
     *     contains a NUL
     */
    public OpenOptions {
        Objects.requireNonNull(backend, "backend");
        Objects.requireNonNull(flags, "flags");
        Objects.requireNonNull(gpioChipPath, "gpioChipPath");
        Objects.requireNonNull(i2cPath, "i2cPath");
        if (backend == Backend.UNRECOGNIZED) {
            throw new IllegalArgumentException("Backend.UNRECOGNIZED is not a backend that can be opened");
        }
        flags = flags.isEmpty() ? Set.of() : Collections.unmodifiableSet(EnumSet.copyOf(flags));
        checkPath("gpioChipPath", gpioChipPath);
        checkPath("i2cPath", i2cPath);
    }

    private static void checkPath(String name, String path) {
        if (path.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(name + " contains a NUL character");
        }
        int length = path.getBytes(StandardCharsets.UTF_8).length;
        if (length >= Limits.PATH_MAX) {
            throw new IllegalArgumentException(name + " is " + length + " bytes in UTF-8; at most "
                    + (Limits.PATH_MAX - 1) + " fit");
        }
    }

    /**
     * The defaults: {@link Backend#AUTO}, no flags, the default command timeout and the default paths. Equal to
     * {@code keel_open(NULL, ...)}.
     *
     * @return the default options
     */
    public static OpenOptions defaults() {
        return new OpenOptions(Backend.AUTO, Set.of(), 0, "", "");
    }

    /**
     * Starts a builder with the defaults.
     *
     * @return a new builder
     */
    public static Builder builder() {
        return new Builder();
    }

    /**
     * Returns the flags as the mask of {@code keel_open_options.flags}.
     *
     * @return the bitwise OR of the flags
     */
    public int flagsMask() {
        return OpenFlag.maskOf(flags.toArray(OpenFlag[]::new));
    }

    /** Builds {@link OpenOptions}; every setter returns the builder. */
    public static final class Builder {
        private Backend backend = Backend.AUTO;
        private Set<OpenFlag> flags = Set.of();
        private int commandTimeoutMs;
        private String gpioChipPath = "";
        private String i2cPath = "";

        private Builder() {
        }

        /**
         * Sets the backend.
         *
         * @param value the backend; {@link Backend#SIMULATOR} for tests and desktops
         * @return this builder
         */
        public Builder backend(Backend value) {
            backend = Objects.requireNonNull(value, "backend");
            return this;
        }

        /**
         * Sets the open flags, replacing any set before.
         *
         * @param values the flags
         * @return this builder
         */
        public Builder flags(OpenFlag... values) {
            flags = values.length == 0 ? Set.of() : EnumSet.copyOf(Arrays.asList(values));
            return this;
        }

        /**
         * Sets the motion-command lifetime.
         *
         * @param value the lifetime in ms; 0 for the default
         * @return this builder
         */
        public Builder commandTimeoutMs(int value) {
            commandTimeoutMs = value;
            return this;
        }

        /**
         * Sets the board's GPIO chip path.
         *
         * @param value the path; empty for the default
         * @return this builder
         */
        public Builder gpioChipPath(String value) {
            gpioChipPath = Objects.requireNonNull(value, "gpioChipPath");
            return this;
        }

        /**
         * Sets the board's I2C adapter path.
         *
         * @param value the path; empty for the default
         * @return this builder
         */
        public Builder i2cPath(String value) {
            i2cPath = Objects.requireNonNull(value, "i2cPath");
            return this;
        }

        /**
         * Builds the options.
         *
         * @return the options
         * @throws IllegalArgumentException if a path does not fit or contains a NUL
         */
        public OpenOptions build() {
            return new OpenOptions(backend, flags, commandTimeoutMs, gpioChipPath, i2cPath);
        }
    }
}
