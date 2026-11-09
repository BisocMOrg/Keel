// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

/** A terminal failure of a sequential task. Motor authority is revoked before it is reported. */
public final class TaskException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    /** The reason why a task can no longer issue hardware operations. */
    public enum Reason {
        /** A caller stopped the task, or its task thread was interrupted. */
        CANCELLED,
        /** The complete task exhausted its configured wall-clock hardware lifetime. */
        TASK_TIMEOUT,
        /** The initial arming handshake did not complete in time. */
        ARM_TIMEOUT,
        /** A board fault or unexpected loss of arming ended the task. */
        FAULT,
        /** The underlying control loop failed or ended unexpectedly. */
        LOOP_FAILED,
        /** Opening or reading the task camera failed. */
        CAMERA_FAILED,
        /** The task's scoped robot has already closed. */
        CLOSED
    }

    /** The stable terminal reason retained when this exception is serialized. */
    private final Reason reason;

    /**
     * Creates a task failure with its machine-readable reason.
     *
     * @param reason the terminal reason
     * @param message explanation for the task author
     */
    public TaskException(Reason reason, String message) {
        super(message);
        this.reason = java.util.Objects.requireNonNull(reason, "reason");
    }

    /**
     * Creates a task failure retaining the lower-level failure.
     *
     * @param reason the terminal reason
     * @param message explanation for the task author
     * @param cause the underlying failure
     */
    public TaskException(Reason reason, String message, Throwable cause) {
        super(message, cause);
        this.reason = java.util.Objects.requireNonNull(reason, "reason");
    }

    /**
     * Returns the terminal reason, without requiring message parsing.
     *
     * @return the terminal reason
     */
    public Reason reason() {
        return reason;
    }
}
