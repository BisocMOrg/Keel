// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static org.junit.jupiter.api.Assertions.fail;

import io.github.bisocmorg.keel.camera.Camera;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.invoke.MethodHandle;
import org.junit.jupiter.api.extension.AfterEachCallback;
import org.junit.jupiter.api.extension.BeforeEachCallback;
import org.junit.jupiter.api.extension.ExtensionContext;

/**
 * Selects the synthetic camera of a libkeel-camera test build for every test of a class, with no fault.
 *
 * <p>A build with {@code KEEL_CAMERA_TEST_HOOKS=ON} (the CMake presets) replaces libcamera and the hardware encoders
 * when {@code KEEL_CAMERA_TEST_SOURCE=1} is set (libkeel SPEC section 26.9): camera 0 is a Camera Module 2 (IMX219,
 * fixed focus), camera 1 a Camera Module 3 (IMX708, autofocus). {@link #fault(String)} injects one
 * {@code KEEL_CAMERA_TEST_FAULT}. The library reads both variables at each call, so they are set through the C
 * library's {@code setenv}, between camera lifetimes, never while a camera captures. Camera tests never load
 * libkeel.so.
 */
public final class CameraSourceExtension implements BeforeEachCallback, AfterEachCallback {
    private static final MethodHandle SETENV;
    private static final MethodHandle UNSETENV;

    static {
        Linker linker = Linker.nativeLinker();
        SETENV = downcall(linker, "setenv", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT));
        UNSETENV = downcall(linker, "unsetenv", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    }

    /** Creates the extension; JUnit instantiates it. */
    public CameraSourceExtension() {
    }

    // The test suite runs with --enable-native-access=ALL-UNNAMED.
    @SuppressWarnings("restricted")
    private static MethodHandle downcall(Linker linker, String name, FunctionDescriptor descriptor) {
        return linker.downcallHandle(linker.defaultLookup().find(name).orElseThrow(), descriptor);
    }

    @Override
    public void beforeEach(ExtensionContext context) {
        if (System.getenv("KEEL_CAMERA_LIBRARY") == null || System.getenv("KEEL_CAMERA_LIBRARY").isEmpty()) {
            fail("set KEEL_CAMERA_LIBRARY to the absolute path of a libkeel-camera test build "
                    + "(cmake --preset sim-only in software/libkeel, with libcamera-dev installed)");
        }
        if (!Camera.versionString().contains("test-hooks")) {
            fail("KEEL_CAMERA_LIBRARY is not a test build: " + Camera.versionString());
        }
        set("KEEL_CAMERA_TEST_SOURCE", "1");
        unset("KEEL_CAMERA_TEST_FAULT");
        unset("KEEL_CAMERA_TEST_ENCODER");
    }

    @Override
    public void afterEach(ExtensionContext context) {
        unset("KEEL_CAMERA_TEST_FAULT");
    }

    /**
     * Injects one fault for the cameras the test opens next.
     *
     * @param name a {@code KEEL_CAMERA_TEST_FAULT} value, such as {@code "stall"}
     */
    public static void fault(String name) {
        set("KEEL_CAMERA_TEST_FAULT", name);
    }

    /**
     * Sets a variable in the process environment, where the C library reads it.
     *
     * @param name the variable
     * @param value its value
     */
    public static void set(String name, String value) {
        try (Arena arena = Arena.ofConfined()) {
            int status = (int) SETENV.invokeExact(arena.allocateFrom(name), arena.allocateFrom(value), 1);
            if (status != 0) {
                fail("setenv " + name + " failed");
            }
        } catch (Throwable thrown) {
            throw new AssertionError(thrown);
        }
    }

    /**
     * Removes a variable from the process environment.
     *
     * @param name the variable
     */
    public static void unset(String name) {
        try (Arena arena = Arena.ofConfined()) {
            int status = (int) UNSETENV.invokeExact(arena.allocateFrom(name));
            if (status != 0) {
                fail("unsetenv " + name + " failed");
            }
        } catch (Throwable thrown) {
            throw new AssertionError(thrown);
        }
    }
}
