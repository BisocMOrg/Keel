// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera.errors;

import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.internal.camera.CameraErrorRegistry;
import java.util.EnumMap;
import java.util.Map;
import java.util.Optional;

/**
 * A camera call returned a status other than OK; the base of every camera status exception.
 *
 * <p>It is thrown itself for the statuses without a subclass: {@code KEEL_E_SETUP_IO} (the camera or the encoder
 * failed to start), {@code KEEL_E_PERIPHERAL} (it failed while running; stop and start again),
 * {@code KEEL_E_NO_MEMORY} and any code this binding does not know. The message is
 * {@code "<function>: <status name>: <description>"}, where the description is the error record's message, which
 * names the remedy, or a short description of the status. The status names come from {@link Status}, generated from
 * keel.h: the camera needs no libkeel.so.
 */
public sealed class CameraException extends RuntimeException permits CameraArgumentException,
        CameraUnsupportedException, CameraStateException, CameraTimeoutException, CameraThreadException,
        CameraNotFoundException, CameraBusyException, CameraPermissionException, CameraClosedException {
    private static final long serialVersionUID = 1L;
    private static final Map<Status, String> DESCRIPTIONS = new EnumMap<>(Status.class);

    static {
        DESCRIPTIONS.put(Status.E_INVALID_ARGUMENT, "a bad option or argument");
        DESCRIPTIONS.put(Status.E_UNSUPPORTED, "a feature this camera or build lacks");
        DESCRIPTIONS.put(Status.E_STATE, "not allowed in the camera's current state");
        DESCRIPTIONS.put(Status.E_AGAIN, "nothing arrived within the timeout");
        DESCRIPTIONS.put(Status.E_CONFLICT,
                "only the thread that opened the camera may call this; close() works from any thread");
        DESCRIPTIONS.put(Status.E_NOT_FOUND, "no such camera, encoder or destination");
        DESCRIPTIONS.put(Status.E_IN_USE, "the camera or the port is in use");
        DESCRIPTIONS.put(Status.E_PERMISSION, "a device node or file is not accessible");
        DESCRIPTIONS.put(Status.E_INVALID_HANDLE, "the camera is closed");
        DESCRIPTIONS.put(Status.E_NOT_OWNER, "the camera belongs to the process that opened it");
        DESCRIPTIONS.put(Status.E_SETUP_IO, "the camera, the encoder or an output failed");
        DESCRIPTIONS.put(Status.E_PERIPHERAL, "the camera, the encoder or an output failed while running");
        DESCRIPTIONS.put(Status.E_NO_MEMORY, "an allocation failed");
        CameraErrorRegistry.register(new CameraErrorFactory());
    }

    /** The status code. */
    private final int code;
    /** The C function that returned it. */
    private final String function;
    /** The error record, or {@code null}. */
    private final ErrorReport report;
    /** The record's message or the status description. */
    private final String description;

    CameraException(int code, String function, ErrorReport report) {
        this(code, function, report, describe(code, report));
    }

    private CameraException(int code, String function, ErrorReport report, String description) {
        super(function + ": " + statusName(code) + ": " + description);
        this.code = code;
        this.function = function;
        this.report = report;
        this.description = description;
    }

    private static String describe(int code, ErrorReport report) {
        if (report != null && !report.message().isEmpty()) {
            return report.message();
        }
        return DESCRIPTIONS.getOrDefault(Status.of(code), "no further details");
    }

    /**
     * The C name of a status code, from the {@link Status} enum generated from keel.h.
     *
     * @param code any status code
     * @return for example {@code "KEEL_E_IN_USE"}, or {@code "status -99"} for a code this binding does not know
     */
    public static String statusName(int code) {
        Status status = Status.of(code);
        return status == Status.UNRECOGNIZED ? "status " + code : "KEEL_" + status.name();
    }

    /**
     * Returns the status.
     *
     * @return the constant, or {@link Status#UNRECOGNIZED} for a code this binding does not know
     */
    public Status status() {
        return Status.of(code);
    }

    /**
     * Returns the raw status code.
     *
     * @return the code the C function returned
     */
    public int code() {
        return code;
    }

    /**
     * Returns the C function that returned the status.
     *
     * @return for example {@code "keel_camera_open"}
     */
    public String function() {
        return function;
    }

    /**
     * Returns the error record libkeel-camera wrote for the failure: the resource, the OS error, the path and the
     * message.
     *
     * @return the record, or empty when there is none (a closed camera, or a call from another thread)
     */
    public Optional<ErrorReport> report() {
        return Optional.ofNullable(report);
    }

    /**
     * Returns the description in the message.
     *
     * @return the record's message, which names the remedy, or a short description of the status
     */
    public String description() {
        return description;
    }
}
