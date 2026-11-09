// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.State;
import java.util.Optional;

/**
 * Status class FAULT: the device is latched {@link State#FAULTED} after the call.
 *
 * <p>Either this call detected a safety-relevant failure and performed a Category-0 stop, or the device was already
 * latched. The heartbeat stops, so the safety MCU removes motor, servo and sensor power. Only
 * {@link Device#clearFault()}, from the control owner, leaves the state, and it never arms.
 *
 * <p>Codes libkeel does not define read as FAULT, and so does a status class this binding does not know.
 */
public final class SafetyFaultException extends KeelException {
    private static final long serialVersionUID = 1L;

    /** The fault record, or {@code null} when it could not be read. */
    private final Fault fault;

    SafetyFaultException(StatusDetails details, Fault fault) {
        super(details, details.message() + describe(fault));
        this.fault = fault;
    }

    private static String describe(Fault fault) {
        if (fault == null) {
            return "; the fault record could not be read";
        }
        return "; cause " + fault.cause().name() + ", detail " + Long.toUnsignedString(fault.detail());
    }

    /**
     * Returns the fault record, read with {@code keel_get_fault} before this exception was thrown.
     *
     * @return the record; empty when it could not be read, for example because the device was closed concurrently
     */
    public Optional<Fault> fault() {
        return Optional.ofNullable(fault);
    }
}
