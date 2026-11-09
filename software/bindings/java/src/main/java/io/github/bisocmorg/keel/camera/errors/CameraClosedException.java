// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_INVALID_HANDLE} or {@code KEEL_E_NOT_OWNER}: the camera is closed.
 *
 * <p>A read that another thread's {@code close()} woke up throws it too.
 */
public final class CameraClosedException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraClosedException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
