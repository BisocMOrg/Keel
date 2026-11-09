// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * {@link Status#E_CONFLICT}: a go call from a thread other than the control owner, or a conflict with an active
 * instruction such as a servo burst.
 *
 * <p>A refinement of {@link RejectedException}: catching that type also catches this one.
 */
public final class ConflictException extends RejectedException {
    private static final long serialVersionUID = 1L;

    ConflictException(StatusDetails details, String message) {
        super(details, message);
    }
}
