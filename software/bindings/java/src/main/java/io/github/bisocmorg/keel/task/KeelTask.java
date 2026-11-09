// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * A single-use sequential student program. Subclasses implement ordinary Java methods and use {@link #keel}
 * for bounded hardware operations. Box finding, navigation and other application algorithms belong in those
 * student methods. The runtime starts and closes the controller; students need no ARMED-state checks.
 */
public abstract class KeelTask {
    /** The scoped robot during run(); null before execution and after cleanup. Never retain it for another task. */
    protected TaskRobot keel;
    private final AtomicBoolean used = new AtomicBoolean();

    /** Creates an unused task; no native library is loaded and no hardware is opened. */
    protected KeelTask() { }

    /**
     * Runs the student's sequence on its calling platform thread. Movement methods finish with zero drive;
     * an ordinary Thread.sleep between actions therefore leaves the motors without drive. Mechanical
     * coast-down is not measured by this API.
     *
     * @throws Exception for any failure in student code or a bounded operation; the runner closes hardware
     */
    protected abstract void run() throws Exception;

    /**
     * Executes this task once with the supplied options. Normally called through
     * {@link io.github.bisocmorg.keel.Keel#run(KeelTask, TaskOptions)}. The caller must use a platform thread
     * because native camera ownership follows that thread. Timeout removes hardware authority, but cannot
     * forcibly terminate arbitrary Java code that never returns.
     *
     * @param options resource configuration and millisecond limits
     * @throws NullPointerException if options is null
     * @throws IllegalStateException if already used or called from a virtual thread
     * @throws Exception the original student failure, or a task/native failure
     */
    public final void execute(TaskOptions options) throws Exception {
        Objects.requireNonNull(options, "options");
        if (Thread.currentThread().isVirtual()) throw new IllegalStateException("KeelTask requires a platform thread");
        if (!used.compareAndSet(false, true)) throw new IllegalStateException("A KeelTask can execute only once");
        try (TaskRobot robot = TaskRobot.open(options)) {
            keel = robot;
            try {
                run();
                robot.verifyCompletion();
            } catch (Exception | Error error) {
                if (error instanceof InterruptedException) Thread.currentThread().interrupt();
                robot.abort(error);
                throw error;
            } finally {
                keel = null;
            }
        }
    }
}
