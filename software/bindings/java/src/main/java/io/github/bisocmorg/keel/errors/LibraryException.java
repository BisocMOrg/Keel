// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * libkeel could not be loaded or used: not Linux, no library, a missing symbol, an incompatible ABI, native access
 * denied, or a library that broke its contract. It carries no status; the message names the cause and the remedy.
 */
public class LibraryException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    /**
     * Creates the exception.
     *
     * @param message what failed and how to fix it
     */
    public LibraryException(String message) {
        super(message);
    }

    /**
     * Creates the exception with the failure that caused it.
     *
     * @param message what failed and how to fix it
     * @param cause the JVM's or loader's exception
     */
    public LibraryException(String message, Throwable cause) {
        super(message, cause);
    }
}
