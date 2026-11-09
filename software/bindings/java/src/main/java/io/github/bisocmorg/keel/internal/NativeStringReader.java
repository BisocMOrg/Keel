// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.errors.LibraryException;
import java.lang.foreign.MemorySegment;

/**
 * Reads the static strings libkeel returns.
 *
 * <p>keel.h: every returned string is "static, NUL-terminated UTF-8, never NULL, must not be freed and is at most
 * {@code KEEL_STRING_MAX} bytes long including the terminating NUL". The reader never reads past that bound, and a
 * string that breaks the contract is a {@link LibraryException}, never a read beyond it.
 */
public final class NativeStringReader {
    private NativeStringReader() {
    }

    /**
     * Reads one returned string.
     *
     * @param address the pointer the function returned
     * @param function the C function that returned it, for the error message
     * @return the decoded string
     * @throws LibraryException if the pointer is NULL or no NUL ends the string within {@link Limits#STRING_MAX}
     *     bytes
     */
    // MemorySegment.reinterpret is a restricted method; the application grants native access to this module or
    // to the class path (README, "Native access").
    @SuppressWarnings("restricted")
    public static String read(MemorySegment address, String function) {
        if (address.address() == 0) {
            throw new LibraryException(function + " returned NULL, which keel.h rules out");
        }
        MemorySegment bounded = address.reinterpret(Limits.STRING_MAX);
        for (long index = 0; index < Limits.STRING_MAX; index++) {
            if (bounded.get(JAVA_BYTE, index) == 0) {
                return bounded.getString(0);
            }
        }
        throw new LibraryException(function + " returned a string with no NUL within KEEL_STRING_MAX ("
                + Limits.STRING_MAX + ") bytes, which keel.h rules out");
    }
}
