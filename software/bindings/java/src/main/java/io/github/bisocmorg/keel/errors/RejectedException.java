// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * Status class REJECTED: refused with no effect on the device.
 *
 * <p>The one documented exception (keel.h): "a motion request refused for its arguments or capability while ARMED
 * coasts both motors and drops the stored command, so a refused motion command can never leave the wheels driving".
 * No fault is latched and the heartbeat continues.
 *
 * <p>Five subclasses refine the common codes; a REJECTED code without one, including a code this binding does not
 * know, is this type itself. Catch this type rather than branching on codes.
 */
public sealed class RejectedException extends KeelException
        permits InvalidArgumentException, StateException, AgainException, UnsupportedException, ConflictException {
    private static final long serialVersionUID = 1L;

    RejectedException(StatusDetails details, String message) {
        super(details, message);
    }
}
