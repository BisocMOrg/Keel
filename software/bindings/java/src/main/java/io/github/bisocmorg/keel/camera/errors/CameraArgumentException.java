// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_INVALID_ARGUMENT}: an option or argument out of range, or a buffer too small; nothing happened.
 */
public final class CameraArgumentException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraArgumentException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
