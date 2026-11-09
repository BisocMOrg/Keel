// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * {@link Status#E_STATE}: the call is not allowed in the current state, for example motion while DISARMED or a
 * request to arm during the re-arm hold-off.
 *
 * <p>A refinement of {@link RejectedException}: catching that type also catches this one.
 */
public final class StateException extends RejectedException {
    private static final long serialVersionUID = 1L;

    StateException(StatusDetails details, String message) {
        super(details, message);
    }
}
