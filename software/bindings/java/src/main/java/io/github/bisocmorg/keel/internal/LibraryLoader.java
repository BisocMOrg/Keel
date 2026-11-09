// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import io.github.bisocmorg.keel.errors.LibraryException;
import java.lang.foreign.Arena;
import java.lang.foreign.SymbolLookup;
import java.nio.file.Path;
import java.util.Locale;

/**
 * The rules for finding and accepting libkeel.
 *
 * <p>Resolution: {@code KEEL_LIBRARY} when it is set and not empty, as an absolute path; otherwise the SONAME
 * {@code libkeel.so.1} through the dynamic loader. libkeel is Linux-only, so any other platform is refused before
 * anything is opened. After loading, the library's ABI must have the binding's major and at least its minor.
 */
public final class LibraryLoader {
    /** The SONAME of libkeel 1.x. */
    public static final String SONAME = "libkeel.so.1";

    /** The environment variable that names a library by absolute path. */
    public static final String ENVIRONMENT = "KEEL_LIBRARY";

    private static final String PINNING = "Install the libkeel1 package whose X.Y matches this binding "
            + "(dpkg-query -W libkeel1 shows the installed version), or use the release of the binding that matches "
            + "the installed library.";

    private LibraryLoader() {
    }

    /**
     * Refuses every platform but Linux.
     *
     * @param osName the value of the {@code os.name} system property
     * @throws LibraryException on any platform but Linux
     */
    public static void checkPlatform(String osName) {
        if (!osName.toLowerCase(Locale.ROOT).startsWith("linux")) {
            throw new LibraryException("libkeel requires Linux; found " + osName);
        }
    }

    /**
     * Resolves the library to load from the value of {@code KEEL_LIBRARY}.
     *
     * @param environment the variable's value, or {@code null} when it is not set
     * @return the absolute path to load, or {@code null} for the SONAME
     * @throws LibraryException if the value is a relative path
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
     * @throws LibraryException if the path is relative
     */
    public static Path requireAbsolute(Path path, String origin) {
        if (!path.isAbsolute()) {
            throw new LibraryException(origin + " must be an absolute path to libkeel.so.1, not " + path);
        }
        return path;
    }

    /**
     * Opens the library for the life of the process.
     *
     * @param path an absolute path, or {@code null} for the SONAME
     * @return the library's symbols
     * @throws LibraryException if the library cannot be opened or native access is denied
     */
    // SymbolLookup.libraryLookup is a restricted method; the application grants native access to this module or to
    // the class path (README, "Native access").
    @SuppressWarnings("restricted")
    public static SymbolLookup open(Path path) {
        String name = describe(path);
        try {
            // The global arena keeps the library loaded for good: unloading it during a call is undefined behaviour.
            return path == null ? SymbolLookup.libraryLookup(SONAME, Arena.global())
                    : SymbolLookup.libraryLookup(path, Arena.global());
        } catch (IllegalArgumentException error) {
            throw new LibraryException("cannot open " + name + ": " + error.getMessage() + ". Install the libkeel1 "
                    + "package, or set KEEL_LIBRARY to the absolute path of a libkeel build", error);
        } catch (IllegalCallerException error) {
            throw nativeAccessDenied(error);
        }
    }

    /**
     * The library failure for a JVM that denies this binding native access.
     *
     * @param error the JVM's refusal
     * @return the exception to throw
     */
    public static LibraryException nativeAccessDenied(IllegalCallerException error) {
        return new LibraryException("native access is denied to the Keel binding: start the JVM with "
                + "--enable-native-access=ALL-UNNAMED (class path) or --enable-native-access=io.github.bisocmorg.keel "
                + "(module path)", error);
    }

    /**
     * Checks the ABI of the loaded library against the one the binding requires.
     *
     * <p>keel.h: "A binding requires an equal major and a minor at least as high as the newest function it calls."
     * The binding was generated from one header minor and may use all of it, so it requires that minor as a whole.
     *
     * @param version the packed value {@code keel_abi_version()} returned, {@code (major << 16) | minor}
     * @param major the major the binding requires
     * @param minor the smallest minor the binding accepts
     * @param library the library that was loaded, for the error message
     * @throws LibraryException if the major differs or the minor is lower
     */
    public static void checkAbi(int version, int major, int minor, String library) {
        int foundMajor = version >>> 16;
        int foundMinor = version & 0xFFFF;
        if (foundMajor != major || foundMinor < minor) {
            throw new LibraryException(library + " has ABI " + foundMajor + "." + foundMinor + "; this binding needs "
                    + major + "." + minor + " or a newer " + major + ".x. " + PINNING);
        }
    }

    /**
     * Names a library for messages.
     *
     * @param path an absolute path, or {@code null} for the SONAME
     * @return a description such as {@code "libkeel.so.1 (dynamic loader)"}
     */
    public static String describe(Path path) {
        return path == null ? SONAME + " (dynamic loader)" : path.toString();
    }
}
