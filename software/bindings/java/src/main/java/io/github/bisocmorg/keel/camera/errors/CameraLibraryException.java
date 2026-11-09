// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

import io.github.bisocmorg.keel.errors.LibraryException;

/**
 * libkeel-camera.so.1 could not be loaded or used: not Linux, no library (the libkeel-camera1 package is not
 * installed), a missing symbol, an incompatible ABI or native access denied. It is a {@link LibraryException}, so one
 * catch clause covers both libraries; the message names the cause and the remedy. Class initialisation never throws
 * it: the camera library is loaded on the first camera call.
 */
public class CameraLibraryException extends LibraryException {
    private static final long serialVersionUID = 1L;

    /**
     * Creates the exception.
     *
     * @param message what failed and how to fix it
     */
    public CameraLibraryException(String message) {
        super(message);
    }

    /**
     * Creates the exception with the failure that caused it.
     *
     * @param message what failed and how to fix it
     * @param cause the JVM's or loader's exception
     */
    public CameraLibraryException(String message, Throwable cause) {
        super(message, cause);
    }
}
