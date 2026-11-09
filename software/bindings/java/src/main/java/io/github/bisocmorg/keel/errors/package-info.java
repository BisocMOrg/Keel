// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The binding's exceptions and the status enums. Every non-OK status of libkeel becomes the
 * {@link io.github.bisocmorg.keel.errors.KeelException} of its class: REJECTED (with one subclass per common code),
 * FAULT, PERIPHERAL, SETUP or HANDLE; the hierarchy is sealed, so a pattern {@code switch} over the classes is
 * exhaustive. {@link io.github.bisocmorg.keel.errors.LibraryException} reports a library that cannot be loaded or
 * used, and carries no status.
 */
package io.github.bisocmorg.keel.errors;
