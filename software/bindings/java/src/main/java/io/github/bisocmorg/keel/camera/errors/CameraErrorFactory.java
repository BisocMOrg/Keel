// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.internal.camera.CameraErrorAccess;
import io.github.bisocmorg.keel.internal.camera.CameraLayouts;
import io.github.bisocmorg.keel.internal.camera.CameraNativeLibrary;
import io.github.bisocmorg.keel.internal.camera.CameraStructCodec;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

/** Builds the camera exceptions from statuses; {@link CameraException} registers it. */
final class CameraErrorFactory implements CameraErrorAccess {
    @Override
    public CameraException of(int code, String function, ErrorReport report) {
        return switch (Status.of(code)) {
            case E_INVALID_ARGUMENT -> new CameraArgumentException(code, function, report);
            case E_UNSUPPORTED -> new CameraUnsupportedException(code, function, report);
            case E_STATE -> new CameraStateException(code, function, report);
            case E_AGAIN -> new CameraTimeoutException(code, function, report);
            case E_CONFLICT -> new CameraThreadException(code, function, report);
            case E_NOT_FOUND -> new CameraNotFoundException(code, function, report);
            case E_IN_USE -> new CameraBusyException(code, function, report);
            case E_PERMISSION -> new CameraPermissionException(code, function, report);
            case E_INVALID_HANDLE, E_NOT_OWNER -> new CameraClosedException(code, function, report);
            default -> new CameraException(code, function, report);
        };
    }

    @Override
    public CameraException failed(CameraNativeLibrary library, MemorySegment camera, int code, String function) {
        ErrorReport report = null;
        if (code != Status.E_CONFLICT.code() && code != Status.E_INVALID_HANDLE.code()
                && code != Status.E_NOT_OWNER.code()) {
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment record = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_ERROR);
                if (library.lastError(camera, record) == Status.OK.code()) {
                    ErrorReport latest = CameraStructCodec.cameraError(record);
                    report = latest.statusValue() == code ? latest : null;
                }
            }
        }
        return of(code, function, report);
    }
}
