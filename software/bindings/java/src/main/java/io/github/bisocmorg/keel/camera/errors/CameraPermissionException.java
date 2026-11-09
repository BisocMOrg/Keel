// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_PERMISSION}: a camera, encoder or frame-buffer device node is not accessible, or a file cannot be
 * made.
 *
 * <p>The message names the remedy: the user joins the video group with {@code sudo keel-setup --user NAME}, then logs
 * in again.
 */
public final class CameraPermissionException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraPermissionException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
