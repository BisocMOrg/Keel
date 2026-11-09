// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * {@link Status#E_UNSUPPORTED}: the device lacks the capability, or a simulator call reached hardware.
 *
 * <p>A refinement of {@link RejectedException}: catching that type also catches this one.
 */
public final class UnsupportedException extends RejectedException {
    private static final long serialVersionUID = 1L;

    UnsupportedException(StatusDetails details, String message) {
        super(details, message);
    }
}
