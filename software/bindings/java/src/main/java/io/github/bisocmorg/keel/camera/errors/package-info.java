// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The camera's exceptions and the error record they carry.
 *
 * <p>Every camera call returns a keel.h status, and every failure records an
 * {@link io.github.bisocmorg.keel.camera.errors.ErrorReport}: the resource involved, the OS error, the path and a
 * one-line message that names the remedy. Each status that needs its own handling maps to one subclass of the sealed
 * {@link io.github.bisocmorg.keel.camera.errors.CameraException}; the others throw the base class itself:
 *
 * <ul>
 * <li>{@code KEEL_E_INVALID_ARGUMENT}: {@link io.github.bisocmorg.keel.camera.errors.CameraArgumentException}</li>
 * <li>{@code KEEL_E_UNSUPPORTED}: {@link io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException}</li>
 * <li>{@code KEEL_E_STATE}: {@link io.github.bisocmorg.keel.camera.errors.CameraStateException}</li>
 * <li>{@code KEEL_E_AGAIN}: {@link io.github.bisocmorg.keel.camera.errors.CameraTimeoutException}</li>
 * <li>{@code KEEL_E_CONFLICT}: {@link io.github.bisocmorg.keel.camera.errors.CameraThreadException}</li>
 * <li>{@code KEEL_E_NOT_FOUND}: {@link io.github.bisocmorg.keel.camera.errors.CameraNotFoundException}</li>
 * <li>{@code KEEL_E_IN_USE}: {@link io.github.bisocmorg.keel.camera.errors.CameraBusyException}</li>
 * <li>{@code KEEL_E_PERMISSION}: {@link io.github.bisocmorg.keel.camera.errors.CameraPermissionException}</li>
 * <li>{@code KEEL_E_INVALID_HANDLE}, {@code KEEL_E_NOT_OWNER}:
 * {@link io.github.bisocmorg.keel.camera.errors.CameraClosedException}</li>
 * </ul>
 *
 * <p>{@link io.github.bisocmorg.keel.camera.errors.CameraLibraryException} (a
 * {@link io.github.bisocmorg.keel.errors.LibraryException}) means that libkeel-camera.so.1 could not be loaded. An
 * {@link java.lang.IllegalArgumentException} or {@link java.lang.NullPointerException} thrown before any call means
 * that the binding refused an argument and no native call was made.
 */
package io.github.bisocmorg.keel.camera.errors;
