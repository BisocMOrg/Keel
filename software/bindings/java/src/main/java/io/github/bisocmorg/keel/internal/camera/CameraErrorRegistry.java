// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal.camera;

import io.github.bisocmorg.keel.camera.errors.CameraException;

/**
 * The registry through which the camera packages reach the package-private exception factory of
 * {@code io.github.bisocmorg.keel.camera.errors}, without widening the API. The module does not export this package.
 */
public final class CameraErrorRegistry {
    private static volatile CameraErrorAccess errors;

    private CameraErrorRegistry() {
    }

    /**
     * Registers the factory; called once, from the static initialiser of {@link CameraException}.
     *
     * @param access the factory
     * @throws IllegalStateException if a factory is already registered
     */
    public static synchronized void register(CameraErrorAccess access) {
        if (errors != null) {
            throw new IllegalStateException("the camera error factory is already registered");
        }
        errors = access;
    }

    /**
     * Returns the factory, initialising {@link CameraException} first if necessary.
     *
     * @return the factory
     */
    public static CameraErrorAccess errors() {
        CameraErrorAccess access = errors;
        if (access == null) {
            try {
                Class.forName(CameraException.class.getName(), true, CameraException.class.getClassLoader());
            } catch (ClassNotFoundException impossible) {
                throw new IllegalStateException(impossible);
            }
            access = errors;
        }
        return access;
    }
}
