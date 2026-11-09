// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.internal.camera.CameraErrorAccess;
import io.github.bisocmorg.keel.internal.camera.CameraErrorRegistry;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.util.LinkedHashMap;
import java.util.Map;
import org.junit.jupiter.api.Test;

/** The camera's exceptions: one type per status that needs its own handling, their messages and serialization. */
class CameraErrorsTest {
    private static final CameraErrorAccess ERRORS = CameraErrorRegistry.errors();

    private static Map<Status, Class<? extends CameraException>> types() {
        Map<Status, Class<? extends CameraException>> types = new LinkedHashMap<>();
        types.put(Status.E_INVALID_ARGUMENT, CameraArgumentException.class);
        types.put(Status.E_UNSUPPORTED, CameraUnsupportedException.class);
        types.put(Status.E_STATE, CameraStateException.class);
        types.put(Status.E_AGAIN, CameraTimeoutException.class);
        types.put(Status.E_CONFLICT, CameraThreadException.class);
        types.put(Status.E_NOT_FOUND, CameraNotFoundException.class);
        types.put(Status.E_IN_USE, CameraBusyException.class);
        types.put(Status.E_PERMISSION, CameraPermissionException.class);
        types.put(Status.E_INVALID_HANDLE, CameraClosedException.class);
        types.put(Status.E_NOT_OWNER, CameraClosedException.class);
        types.put(Status.E_SETUP_IO, CameraException.class);
        types.put(Status.E_PERIPHERAL, CameraException.class);
        types.put(Status.E_NO_MEMORY, CameraException.class);
        return types;
    }

    @Test
    void eachStatusHasItsType() {
        for (Map.Entry<Status, Class<? extends CameraException>> entry : types().entrySet()) {
            Status status = entry.getKey();
            CameraException error = ERRORS.of(status.code(), "keel_camera_start", null);
            assertSame(entry.getValue(), error.getClass(), status.name());
            assertEquals(status, error.status());
            assertEquals(status.code(), error.code());
            assertEquals("keel_camera_start", error.function());
            assertTrue(error.report().isEmpty());
            assertTrue(!error.description().isEmpty());
            assertEquals("keel_camera_start: KEEL_" + status.name() + ": " + error.description(), error.getMessage());
        }
        assertEquals("f: KEEL_E_AGAIN: nothing arrived within the timeout",
                ERRORS.of(Status.E_AGAIN.code(), "f", null).getMessage());
    }

    @Test
    void theReportMessageNamesTheRemedy() {
        ErrorReport report = new ErrorReport(Status.E_PERMISSION.code(), CameraResource.DEVICE_NODE.code(), 13,
                "/dev/media0", "permission denied on /dev/media0: add the user to the video group");
        CameraException error = ERRORS.of(Status.E_PERMISSION.code(), "keel_camera_open", report);
        assertSame(report, error.report().orElseThrow());
        assertEquals("keel_camera_open: KEEL_E_PERMISSION: permission denied on /dev/media0: add the user to the video "
                + "group", error.getMessage());
        ErrorReport empty = new ErrorReport(Status.E_PERMISSION.code(), 0, 0, "", "");
        assertEquals("a device node or file is not accessible",
                ERRORS.of(Status.E_PERMISSION.code(), "f", empty).description());
    }

    @Test
    void anUnknownStatusIsAPlainCameraException() {
        CameraException error = ERRORS.of(-99, "keel_camera_start", null);
        assertSame(CameraException.class, error.getClass());
        assertSame(Status.UNRECOGNIZED, error.status());
        assertEquals("keel_camera_start: status -99: no further details", error.getMessage());
        assertEquals("KEEL_E_INVALID_ARGUMENT", CameraException.statusName(-1));
    }

    @Test
    void errorsSurviveSerialization() throws IOException, ClassNotFoundException {
        ErrorReport report = new ErrorReport(Status.E_IN_USE.code(), CameraResource.CAMERA.code(), 16, "", "busy");
        CameraException error = ERRORS.of(Status.E_IN_USE.code(), "keel_camera_open", report);
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ObjectOutputStream out = new ObjectOutputStream(bytes)) {
            out.writeObject(error);
        }
        try (ObjectInputStream in = new ObjectInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            CameraBusyException copy = assertInstanceOf(CameraBusyException.class, in.readObject());
            assertEquals(error.getMessage(), copy.getMessage());
            assertEquals(report, copy.report().orElseThrow());
            assertEquals(error.code(), copy.code());
            assertEquals(error.function(), copy.function());
        }
    }

    @Test
    void theLibraryExceptionIsALibraryException() {
        CameraLibraryException error = new CameraLibraryException("cannot load x");
        assertInstanceOf(LibraryException.class, error);
        assertEquals("cannot load x", error.getMessage());
        IllegalStateException cause = new IllegalStateException();
        assertSame(cause, new CameraLibraryException("y", cause).getCause());
    }

    @Test
    void theFactoryIsRegisteredOnce() {
        assertThrows(IllegalStateException.class, () -> CameraErrorRegistry.register(ERRORS));
        assertSame(ERRORS, CameraErrorRegistry.errors());
    }
}
