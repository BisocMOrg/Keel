// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_NOT_FOUND}: no camera, the IPA module or the hardware encoder missing, or an unreachable destination.
 *
 * <p>A camera index past the last camera, and a receiver that refused the connection or did not answer, are not found
 * as well.
 */
public final class CameraNotFoundException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraNotFoundException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
