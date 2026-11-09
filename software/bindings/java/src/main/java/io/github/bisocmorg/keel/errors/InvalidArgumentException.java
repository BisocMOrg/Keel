// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * {@link Status#E_INVALID_ARGUMENT}: libkeel refused an argument, or the binding refused a duty handed to the
 * control loop before any call was made.
 *
 * <p>A refinement of {@link RejectedException}: catching that type also catches this one.
 */
public final class InvalidArgumentException extends RejectedException {
    private static final long serialVersionUID = 1L;

    InvalidArgumentException(StatusDetails details, String message) {
        super(details, message);
    }
}
