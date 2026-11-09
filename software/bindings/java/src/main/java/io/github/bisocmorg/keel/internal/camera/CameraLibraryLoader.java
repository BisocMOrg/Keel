// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal.camera;

import io.github.bisocmorg.keel.camera.errors.CameraLibraryException;
import java.lang.foreign.Arena;
import java.lang.foreign.SymbolLookup;
import java.nio.file.Path;
import java.util.Locale;

/**
 * The rules for finding and accepting libkeel-camera.
 *
 * <p>Resolution: {@code KEEL_CAMERA_LIBRARY} when it is set and not empty, as an absolute path; otherwise the SONAME
 * {@code libkeel-camera.so.1} through the dynamic loader. libkeel-camera is Linux-only, so any other platform is
 * refused before anything is opened. After loading, the library's ABI must have the binding's major and at least its
 * minor.
 */
public final class CameraLibraryLoader {
    /** The SONAME of libkeel-camera 1.x. */
    public static final String SONAME = "libkeel-camera.so.1";

    /** The environment variable that names the camera library by absolute path. */
    public static final String ENVIRONMENT = "KEEL_CAMERA_LIBRARY";

    private static final String REMEDY = "On the robot, install the libkeel-camera1 package (sudo apt install "
            + "libkeel-camera1); elsewhere, build libkeel-camera from software/libkeel (it needs libcamera-dev) and "
            + "set KEEL_CAMERA_LIBRARY to the absolute path of libkeel-camera.so.1";

    private CameraLibraryLoader() {
    }

    /**
     * Refuses every platform but Linux.
     *
     * @param osName the value of the {@code os.name} system property
     * @throws CameraLibraryException on any platform but Linux
     */
    public static void checkPlatform(String osName) {
        if (!osName.toLowerCase(Locale.ROOT).startsWith("linux")) {
            throw new CameraLibraryException("libkeel-camera requires Linux; found " + osName);
        }
    }

    /**
     * Resolves the library to load from the value of {@code KEEL_CAMERA_LIBRARY}.
     *
     * @param environment the variable's value, or {@code null} when it is not set
     * @return the absolute path to load, or {@code null} for the SONAME
     * @throws CameraLibraryException if the value is a relative path
     */
    public static Path resolve(String environment) {
        if (environment == null || environment.isEmpty()) {
            return null;
        }
        return requireAbsolute(Path.of(environment), ENVIRONMENT);
    }

    /**
     * Requires an absolute library path.
     *
     * @param path the path
     * @param origin where the path came from, for the error message
     * @return {@code path}
     * @throws CameraLibraryException if the path is relative
     */
    public static Path requireAbsolute(Path path, String origin) {
        if (!path.isAbsolute()) {
            throw new CameraLibraryException(origin + " must be an absolute path to libkeel-camera.so.1, not " + path);
        }
        return path;
    }

    /**
     * Opens the library for the life of the process.
     *
     * @param path an absolute path, or {@code null} for the SONAME
     * @return the library's symbols
     * @throws CameraLibraryException if the library cannot be opened or native access is denied
     */
    // SymbolLookup.libraryLookup is a restricted method; the application grants native access to this module or to
    // the class path (README, "Native access").
    @SuppressWarnings("restricted")
    public static SymbolLookup open(Path path) {
        try {
            // The global arena keeps the library loaded for good: unloading it during a call is undefined behaviour.
            return path == null ? SymbolLookup.libraryLookup(SONAME, Arena.global())
                    : SymbolLookup.libraryLookup(path, Arena.global());
        } catch (IllegalArgumentException error) {
            throw new CameraLibraryException("cannot load " + describe(path) + ": " + error.getMessage() + ". "
                    + REMEDY, error);
        } catch (IllegalCallerException error) {
            throw nativeAccessDenied(error);
        }
    }

    /**
     * The camera library failure for a JVM that denies this binding native access.
     *
     * @param error the JVM's refusal
     * @return the exception to throw
     */
    public static CameraLibraryException nativeAccessDenied(IllegalCallerException error) {
        return new CameraLibraryException("native access is denied to the Keel binding: start the JVM with "
                + "--enable-native-access=ALL-UNNAMED (class path) or --enable-native-access=io.github.bisocmorg.keel "
                + "(module path)", error);
    }

    /**
     * Checks the ABI of the loaded library against the one the binding requires.
     *
     * @param version the packed value {@code keel_camera_abi_version()} returned, {@code (major << 16) | minor}
     * @param major the major the binding requires
     * @param minor the smallest minor the binding accepts
     * @param library the library that was loaded, for the error message
     * @throws CameraLibraryException if the major differs or the minor is lower
     */
    public static void checkAbi(int version, int major, int minor, String library) {
        int foundMajor = version >>> 16;
        int foundMinor = version & 0xFFFF;
        if (foundMajor != major || foundMinor < minor) {
            throw new CameraLibraryException(library + " has camera ABI " + foundMajor + "." + foundMinor
                    + "; this binding needs " + major + "." + minor + " or a newer " + major + ".x. Install the "
                    + "libkeel-camera1 package that matches the binding, or the binding that matches the library.");
        }
    }

    /**
     * Names a library for messages.
     *
     * @param path an absolute path, or {@code null} for the SONAME
     * @return the path, or {@code libkeel-camera.so.1} for the dynamic loader
     */
    public static String describe(Path path) {
        return path == null ? SONAME : path.toString();
    }
}
