// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

/**
 * {@code KEEL_E_UNSUPPORTED}: a feature this camera or build lacks.
 *
 * <p>Examples: a focus option on a fixed-focus camera (Camera Module 2), an .mp4 or rtsp:// output in a build without
 * FFmpeg's libavformat, an encoder for a stream larger than 1920x1920.
 */
public final class CameraUnsupportedException extends CameraException {
    private static final long serialVersionUID = 1L;

    CameraUnsupportedException(int code, String function, ErrorReport report) {
        super(code, function, report);
    }
}
