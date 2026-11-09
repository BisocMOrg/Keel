// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_AGAIN}: nothing arrived within the timeout (frames, chunks, stills, autofocus).
 *
 * <p>Also thrown by {@code startOutput} when all {@code KEEL_CAMERA_OUTPUTS_MAX} outputs exist.
 */
public final class CameraTimeoutException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraTimeoutException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
