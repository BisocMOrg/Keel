// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * {@link Status#E_AGAIN}: temporarily refused, retry later (servo cooldown, ranging spacing). Never a safety
 * refusal.
 *
 * <p>A refinement of {@link RejectedException}: catching that type also catches this one.
 */
public final class AgainException extends RejectedException {
    private static final long serialVersionUID = 1L;

    AgainException(StatusDetails details, String message) {
        super(details, message);
    }
}
