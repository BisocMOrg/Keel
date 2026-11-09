// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal.camera;

import io.github.bisocmorg.keel.camera.errors.CameraException;
import io.github.bisocmorg.keel.camera.errors.ErrorReport;
import java.lang.foreign.MemorySegment;

/**
 * Builds the camera exceptions, whose constructors are package-private, for the camera packages.
 *
 * <p>The implementation lives in {@code io.github.bisocmorg.keel.camera.errors} and registers itself in
 * {@link CameraErrorRegistry} when {@link CameraException} is initialised.
 */
public interface CameraErrorAccess {
    /**
     * Returns the exception for a non-OK status: the subclass of its status, else {@link CameraException}.
     *
     * @param code the status
     * @param function the C function that returned it
     * @param report the error record, or {@code null} when there is none
     * @return the exception to throw
     */
    CameraException of(int code, String function, ErrorReport report);

    /**
     * Returns the exception for a failed call on an open camera, with the camera's latest error record when it has
     * one ({@code keel_camera_last_error}, which must run on the owning thread, as the failed call did).
     *
     * @param library the loaded camera library
     * @param camera the camera handle
     * @param code the status
     * @param function the C function that returned it
     * @return the exception to throw
     */
    CameraException failed(CameraNativeLibrary library, MemorySegment camera, int code, String function);
}
