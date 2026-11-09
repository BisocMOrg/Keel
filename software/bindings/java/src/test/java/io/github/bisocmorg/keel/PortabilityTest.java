// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraLimits;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.errors.CameraLibraryException;
import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.internal.LibraryLoader;
import io.github.bisocmorg.keel.internal.camera.CameraLibraryLoader;
import io.github.bisocmorg.keel.peripherals.Button;
import java.util.Locale;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;

/**
 * The binding on every platform: class initialisation never touches either library, and loading libkeel or
 * libkeel-camera fails with a clear {@link LibraryException} anywhere but Linux. CI runs this class alone on Windows
 * and macOS.
 */
@Tag("portability")
class PortabilityTest {
    private static final String OS = System.getProperty("os.name");

    @Test
    void theApiInitialisesWithoutTheLibrary() {
        assertTrue(Keel.VERSION.matches("\\d+\\.\\d+\\.\\d+"), Keel.VERSION);
        assertSame(Status.E_CONFLICT, Status.of(-15));
        assertEquals(Limits.TICK_PERIOD_MS, 10);
        assertEquals(Backend.AUTO, OpenOptions.defaults().backend());
        assertEquals(Button.maskOf(Button.A, Button.B), 3);
        assertEquals(1920, CameraLimits.ENCODER_SIZE_MAX);
        assertSame(Codec.H264, Codec.of(1));
        assertEquals(640, CameraOptions.defaults().width());
    }

    @Test
    void loadingRequiresLinux() {
        if (OS.toLowerCase(Locale.ROOT).startsWith("linux")) {
            LibraryException windows = assertThrows(LibraryException.class,
                    () -> LibraryLoader.checkPlatform("Windows 11"));
            assertEquals("libkeel requires Linux; found Windows 11", windows.getMessage());
            assertThrows(LibraryException.class, () -> LibraryLoader.checkPlatform("Mac OS X"));
        } else {
            LibraryException error = assertThrows(LibraryException.class, Keel::load);
            assertEquals("libkeel requires Linux; found " + OS, error.getMessage());
            assertThrows(LibraryException.class, Keel::open);
            assertThrows(LibraryException.class, Keel::versionString);
        }
    }

    @Test
    void loadingTheCameraRequiresLinux() {
        if (OS.toLowerCase(Locale.ROOT).startsWith("linux")) {
            CameraLibraryException windows = assertThrows(CameraLibraryException.class,
                    () -> CameraLibraryLoader.checkPlatform("Windows 11"));
            assertEquals("libkeel-camera requires Linux; found Windows 11", windows.getMessage());
        } else {
            CameraLibraryException error = assertThrows(CameraLibraryException.class, Camera::load);
            assertEquals("libkeel-camera requires Linux; found " + OS, error.getMessage());
            assertThrows(LibraryException.class, Camera::listCameras);
        }
    }
}
