// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_IN_USE}: another process or camera handle has the camera, or a listening port is taken.
 */
public final class CameraBusyException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraBusyException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
