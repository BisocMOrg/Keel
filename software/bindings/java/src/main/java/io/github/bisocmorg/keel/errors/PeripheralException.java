// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * Status class PERIPHERAL: a non-safety peripheral (the RGB LED driver or the ultrasonic sensor) failed. The safety
 * state is unchanged.
 */
public final class PeripheralException extends KeelException {
    private static final long serialVersionUID = 1L;

    PeripheralException(StatusDetails details) {
        super(details, details.message());
    }
}
