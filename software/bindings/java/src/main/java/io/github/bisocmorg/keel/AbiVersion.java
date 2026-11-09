// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

/**
 * The ABI version of the loaded library, unpacked from {@code keel_abi_version()}.
 *
 * <p>Compare it with {@link Abi#MAJOR} and {@link Abi#MINOR}, the ABI of the header this binding was generated from.
 * The binding loads only a library with an equal major and a minor at least {@link Abi#MINOR}.
 *
 * @param major incompatible changes; equals the shared library's SOVERSION
 * @param minor additive changes: new functions, appended fields and values
 */
public record AbiVersion(int major, int minor) {
    /**
     * Returns the version as {@code "major.minor"}.
     *
     * @return for example {@code "1.0"}
     */
    @Override
    public String toString() {
        return major + "." + minor;
    }
}
