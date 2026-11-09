// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import io.github.bisocmorg.keel.OpenReport;
import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.errors.InvalidArgumentException;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.SetupException;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.Snapshot;
import java.util.function.Supplier;

/**
 * Builds the status exceptions, whose constructors are package-private to {@code errors}: the factory the other
 * packages of the binding throw through. {@link AccessRegistry#errors()} returns the one implementation.
 */
public interface ErrorAccess {
    /**
     * The exception for a non-OK status of a device call, with the class libkeel gives the status.
     *
     * @param code the status
     * @param function the C function
     * @param fault reads the device's fault record for a FAULT exception; returns {@code null} when it cannot
     * @param snapshot the snapshot {@code keel_cycle} wrote, or {@code null}
     * @return the exception to throw
     */
    KeelException of(int code, String function, Supplier<Fault> fault, Snapshot snapshot);

    /**
     * The exception for a status of a given class.
     *
     * @param code the status
     * @param classValue its {@code keel_status_class} value
     * @param function the C function
     * @param fault reads the fault record for a FAULT exception; returns {@code null} when it cannot
     * @param report the open report for a SETUP exception, or {@code null}
     * @param snapshot the snapshot {@code keel_cycle} wrote, or {@code null}
     * @return the exception to throw
     */
    KeelException of(int code, int classValue, String function, Supplier<Fault> fault, OpenReport report,
            Snapshot snapshot);

    /**
     * The exception for a failed {@code keel_open}: always a {@link SetupException}, whatever the class.
     *
     * @param code the status
     * @param report the report libkeel wrote
     * @return the exception to throw
     */
    SetupException setup(int code, OpenReport report);

    /**
     * The binding's refusal of a call on a device it has closed or is closing.
     *
     * @param function the C function that was not called
     * @return the exception to throw
     */
    HandleException closed(String function);

    /**
     * The binding's refusal of a request to a control loop that has ended.
     *
     * @param function the C function the request would have reached
     * @return the exception to throw
     */
    HandleException loopEnded(String function);

    /**
     * The control loop's refusal of an out-of-range duty.
     *
     * @param caller the method that refused it
     * @param left the left duty
     * @param right the right duty
     * @return the exception to throw
     */
    InvalidArgumentException dutyRefused(String caller, int left, int right);
}
