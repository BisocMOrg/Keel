// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_CONFLICT}: called from a thread other than the one that opened the camera.
 *
 * <p>The platform thread that opens a camera owns it; only {@code close()} works from any thread.
 */
public final class CameraThreadException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraThreadException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
