// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.errors.KeelException;
import java.util.Objects;

/**
 * Hands the binding's packages the members of another package that are not public API.
 *
 * <p>Each implementation is a package-private class that a static initialiser of its public owner registers:
 * {@link KeelException} registers the {@link ErrorAccess}, {@link Device} the {@link DeviceAccess}. A getter called
 * before that initialises the owner, so it never returns {@code null}. The module does not export this package, so
 * applications on the module path cannot reach the registry.
 */
public final class AccessRegistry {
    private static volatile ErrorAccess errors;
    private static volatile DeviceAccess devices;

    private AccessRegistry() {
    }

    /**
     * Registers the exception factory; {@link KeelException} calls it once, from its static initialiser.
     *
     * @param access the factory
     * @throws IllegalStateException if a factory is already registered
     */
    public static synchronized void registerErrors(ErrorAccess access) {
        if (errors != null) {
            throw new IllegalStateException("the exception factory is already registered");
        }
        errors = Objects.requireNonNull(access, "access");
    }

    /**
     * Returns the exception factory, initialising {@link KeelException} first if it has not run yet.
     *
     * @return the factory
     */
    public static ErrorAccess errors() {
        ErrorAccess access = errors;
        if (access == null) {
            initialize(KeelException.class);
            access = errors;
        }
        return access;
    }

    /**
     * Registers the device access; {@link Device} calls it once, from its static initialiser.
     *
     * @param access the device access
     * @throws IllegalStateException if a device access is already registered
     */
    public static synchronized void registerDevices(DeviceAccess access) {
        if (devices != null) {
            throw new IllegalStateException("the device access is already registered");
        }
        devices = Objects.requireNonNull(access, "access");
    }

    /**
     * Returns the device access, initialising {@link Device} first if it has not run yet.
     *
     * @return the device access
     */
    public static DeviceAccess devices() {
        DeviceAccess access = devices;
        if (access == null) {
            initialize(Device.class);
            access = devices;
        }
        return access;
    }

    /** Runs the static initialiser of a class, which registers its implementation. */
    private static void initialize(Class<?> owner) {
        try {
            Class.forName(owner.getName(), true, owner.getClassLoader());
        } catch (ClassNotFoundException impossible) {
            throw new IllegalStateException(owner.getName() + " cannot be initialised", impossible);
        }
    }
}
