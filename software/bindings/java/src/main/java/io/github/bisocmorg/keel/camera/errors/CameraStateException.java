// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_STATE}: not allowed in the camera's state, such as a read before {@code start()} or a still while
 * capturing.
 */
public final class CameraStateException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraStateException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
