// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

import io.github.bisocmorg.keel.internal.AccessRegistry;
import io.github.bisocmorg.keel.state.Snapshot;
import java.util.Optional;

/**
 * A non-OK status from libkeel, or a refusal the binding made with the status libkeel would have returned.
 *
 * <p>Each status class maps to one type, and the type tells the caller what state the device is in after the call
 * (keel.h, "Status classes"):
 *
 * <table class="striped">
 * <caption>Exception types by status class</caption>
 * <thead><tr><th scope="col">Type</th><th scope="col">Class</th><th scope="col">Device after the call</th></tr>
 * </thead>
 * <tbody>
 * <tr><th scope="row">{@link RejectedException}</th><td>REJECTED</td><td>unchanged, except that a refused motion
 * request while ARMED coasts both motors</td></tr>
 * <tr><th scope="row">{@link SafetyFaultException}</th><td>FAULT, and any class this binding does not know</td>
 * <td>latched FAULTED: Category-0 stop done, heartbeat stopped</td></tr>
 * <tr><th scope="row">{@link PeripheralException}</th><td>PERIPHERAL</td><td>safety state unchanged</td></tr>
 * <tr><th scope="row">{@link SetupException}</th><td>every failed {@code keel_open}</td><td>no device</td></tr>
 * <tr><th scope="row">{@link HandleException}</th><td>HANDLE</td><td>the handle is unusable by this caller</td></tr>
 * </tbody>
 * </table>
 *
 * <p>The type is sealed and abstract, so a pattern {@code switch} over the five types is exhaustive without a
 * {@code default}. The exceptions are unchecked: every device call can fault, so a checked exception would sit on
 * every signature.
 */
public abstract sealed class KeelException extends RuntimeException
        permits RejectedException, SafetyFaultException, PeripheralException, SetupException, HandleException {
    private static final long serialVersionUID = 1L;

    static {
        AccessRegistry.registerErrors(new ErrorFactory());
    }

    /** The raw status code. */
    private final int code;
    /** The raw {@code keel_status_class} value of the status. */
    private final int classValue;
    /** The status name, {@code keel_status_name(code)}. */
    private final String name;
    /** The remedy sentence, {@code keel_status_string(code)}. */
    private final String description;
    /** The C function that returned the status, or that a binding refusal stood in for. */
    private final String function;
    /** The snapshot {@code keel_cycle} wrote, or {@code null}. */
    private final Snapshot snapshot;

    KeelException(StatusDetails details, String message) {
        super(message);
        this.code = details.code();
        this.classValue = details.classValue();
        this.name = details.name();
        this.description = details.description();
        this.function = details.function();
        this.snapshot = details.snapshot();
    }

    /**
     * Returns the status.
     *
     * @return the constant, or {@link Status#UNRECOGNIZED} for a code this binding does not know ({@link #code()}
     *     keeps it)
     */
    public Status status() {
        return Status.of(code);
    }

    /**
     * Returns the raw status code.
     *
     * @return the {@code int32_t} libkeel returned
     */
    public int code() {
        return code;
    }

    /**
     * Returns the status name, as {@code keel_status_name} gives it.
     *
     * @return for example {@code "KEEL_E_CONFLICT"}
     */
    public String name() {
        return name;
    }

    /**
     * Returns the one-sentence description of the status, as {@code keel_status_string} gives it.
     *
     * @return the description, which names the remedy where there is one
     */
    public String description() {
        return description;
    }

    /**
     * Returns the class libkeel gives the status.
     *
     * @return the class, or {@link StatusClass#UNRECOGNIZED} for a class this binding does not know
     */
    public StatusClass statusClass() {
        return StatusClass.of(classValue);
    }

    /**
     * Returns the C function that returned the status, or that a binding refusal stood in for.
     *
     * @return for example {@code "keel_set_motors"}
     */
    public String function() {
        return function;
    }

    /**
     * Returns the snapshot {@code keel_cycle} wrote along with the status.
     *
     * @return the snapshot; empty for every other function and when the call left its output untouched
     */
    public Optional<Snapshot> snapshot() {
        return Optional.ofNullable(snapshot);
    }
}
