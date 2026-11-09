// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal.camera;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.AbiVersion;
import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraAbi;
import io.github.bisocmorg.keel.camera.errors.CameraLibraryException;
import io.github.bisocmorg.keel.support.CameraSourceExtension;
import java.lang.foreign.Arena;
import java.lang.foreign.SymbolLookup;
import java.nio.file.Path;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;

/** Loading libkeel-camera: resolution, the ABI check, the platform check and the symbols. */
@ExtendWith(CameraSourceExtension.class)
class CameraLibraryTest {
    private static final String LIBRARY = System.getenv(CameraLibraryLoader.ENVIRONMENT);

    @Test
    void theTestBuildLoadsAndReportsItsVersion() {
        Camera.load();
        Camera.load(Path.of(LIBRARY));
        assertEquals(new AbiVersion(CameraAbi.MAJOR, CameraAbi.MINOR), Camera.abiVersion());
        assertEquals(0x0001_0000, CameraAbi.VERSION);
        assertTrue(Camera.versionString().startsWith("1.0.0 ("), Camera.versionString());
        assertTrue(Camera.versionString().contains("test-hooks"), Camera.versionString());
        assertSame(Camera.versionString(), CameraNativeLibrary.get().versionString());
        assertEquals(LIBRARY, CameraNativeLibrary.loaded().orElseThrow().library());
    }

    @Test
    void anotherLibraryCannotReplaceTheLoadedOne() {
        Camera.load();
        CameraLibraryException error = assertThrows(CameraLibraryException.class,
                () -> Camera.load(Path.of("/usr/lib/other/libkeel-camera.so.1")));
        assertTrue(error.getMessage().contains("already loaded from " + LIBRARY), error.getMessage());
        assertThrows(NullPointerException.class, () -> Camera.load(null));
    }

    @Test
    void resolutionTreatsEmptyAsUnsetAndRefusesRelativePaths() {
        assertNull(CameraLibraryLoader.resolve(null));
        assertNull(CameraLibraryLoader.resolve(""));
        assertEquals(Path.of("/opt/libkeel-camera.so.1"), CameraLibraryLoader.resolve("/opt/libkeel-camera.so.1"));
        CameraLibraryException error = assertThrows(CameraLibraryException.class,
                () -> CameraLibraryLoader.resolve("build/libkeel-camera.so.1"));
        assertTrue(error.getMessage().startsWith("KEEL_CAMERA_LIBRARY must be an absolute path"), error.getMessage());
        assertEquals("libkeel-camera.so.1", CameraLibraryLoader.SONAME);
    }

    @Test
    void onlyLinuxIsAccepted() {
        CameraLibraryLoader.checkPlatform("Linux");
        CameraLibraryException error = assertThrows(CameraLibraryException.class,
                () -> CameraLibraryLoader.checkPlatform("Windows 11"));
        assertEquals("libkeel-camera requires Linux; found Windows 11", error.getMessage());
    }

    @Test
    void theAbiCheckNeedsTheMajorAndAtLeastTheMinor() {
        CameraLibraryLoader.checkAbi(0x0001_0000, 1, 0, "lib");
        CameraLibraryLoader.checkAbi(0x0001_0003, 1, 2, "lib");
        CameraLibraryException major = assertThrows(CameraLibraryException.class,
                () -> CameraLibraryLoader.checkAbi(0x0002_0000, 1, 0, "lib"));
        assertTrue(major.getMessage().startsWith("lib has camera ABI 2.0; this binding needs 1.0"), major.getMessage());
        assertThrows(CameraLibraryException.class, () -> CameraLibraryLoader.checkAbi(0x0001_0001, 1, 2, "lib"));
    }

    // The test suite runs with --enable-native-access=ALL-UNNAMED.
    @SuppressWarnings("restricted")
    @Test
    void everySymbolMustResolve() {
        SymbolLookup library = SymbolLookup.libraryLookup(Path.of(LIBRARY), Arena.global());
        new CameraNativeLibrary(library, LIBRARY);
        SymbolLookup partial = name -> name.equals("keel_camera_start") ? Optional.empty() : library.find(name);
        CameraLibraryException error = assertThrows(CameraLibraryException.class,
                () -> new CameraNativeLibrary(partial, "partial"));
        assertTrue(error.getMessage().contains("does not export keel_camera_start"), error.getMessage());
        assertEquals(25, CameraNativeLibrary.functionNames().size());
    }

    @Test
    void downcallFailuresAreRethrownUnchecked() {
        IllegalStateException runtime = new IllegalStateException("runtime");
        assertSame(runtime, CameraNativeLibrary.rethrow(runtime));
        assertThrows(AssertionError.class, () -> CameraNativeLibrary.rethrow(new AssertionError("error")));
        Exception checked = new Exception("checked");
        assertSame(checked, CameraNativeLibrary.rethrow(checked).getCause());
    }
}
