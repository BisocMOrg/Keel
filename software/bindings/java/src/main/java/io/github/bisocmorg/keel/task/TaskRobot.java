// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.OpenReport;
import io.github.bisocmorg.keel.loop.LoopStats;
import io.github.bisocmorg.keel.peripherals.RangeMeasurement;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.State;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.LongSupplier;

/**
 * Sequential, bounded robot operations available during a {@link KeelTask}.
 *
 * <p>The task's calling platform thread owns every operation except {@link #stop()}.
 * Movement calls automatically perform the first arming handshake and block until their finite duration ends
 * and a later native snapshot confirms zero commands and outputs. No nonzero drive command persists after a
 * successful movement call. Mechanical coast-down is not measured. Percent duties are converted to libkeel's
 * signed permille; this is not a measured speed API.
 *
 * <p>The existing control loop enforces motion and total-task deadlines independently of student code.
 * A fault, cancellation or expired task is terminal: it cannot re-arm. A task timeout removes hardware
 * authority; it cannot forcibly terminate arbitrary Java computation or Thread.sleep in student code.
 */
public final class TaskRobot implements AutoCloseable {
    private static final long NANOS_PER_MS = 1_000_000L;
    private final Object gate = new Object();
    private final TaskOptions options;
    private final TaskHardware hardware;
    private final LongSupplier clock;
    private final Thread owner = Thread.currentThread();
    private final long taskDeadline;
    private Motion motion;
    private TaskException failure;
    private boolean armedOnce;
    private boolean closed;

    /** Creates the native-backed task resource on the student's platform thread. */
    static TaskRobot open(TaskOptions options) {
        return new TaskRobot(options, new NativeTaskHardware(options), System::nanoTime);
    }

    /** Attaches the coordinator to its hardware boundary; the injected clock is monotonic nanoseconds. */
    TaskRobot(TaskOptions options, TaskHardware hardware, LongSupplier clock) {
        this.options = Objects.requireNonNull(options, "options");
        this.hardware = Objects.requireNonNull(hardware, "hardware");
        this.clock = Objects.requireNonNull(clock, "clock");
        taskDeadline = clock.getAsLong() + options.taskTimeoutMs() * NANOS_PER_MS;
        try {
            hardware.start(this::step);
        } catch (RuntimeException | Error error) {
            try { hardware.close(); } catch (RuntimeException | Error closeError) { error.addSuppressed(closeError); }
            throw error;
        }
    }

    /**
     * Drives for a bounded duration, then waits until zero motor drive is confirmed by a native snapshot.
     * The duration starts once arming has completed. Zero duration is a no-op; two zero duties hold coast for
     * the duration without arming. Deadlines are checked at control-cycle resolution, normally 10 ms. This call
     * never clears a fault or re-arms after an unexpected disarm.
     *
     * @param leftPercent left motor duty, finite -100..100 percent; positive is forward
     * @param rightPercent right motor duty, finite -100..100 percent; positive is forward
     * @param durationMs duration in milliseconds, 0..{@link TaskOptions#maxMotionMs()}
     * @throws IllegalArgumentException for invalid duties or duration
     * @throws IllegalStateException when called outside the owning task thread
     * @throws InterruptedException if the task thread is interrupted; motor authority is revoked
     * @throws TaskException if the task is cancelled, expired, closed or the hardware fails
     */
    public void driveFor(double leftPercent, double rightPercent, long durationMs) throws InterruptedException {
        int left = duty(leftPercent, "leftPercent");
        int right = duty(rightPercent, "rightPercent");
        if (durationMs < 0 || durationMs > options.maxMotionMs()) {
            throw new IllegalArgumentException("durationMs must be 0.." + options.maxMotionMs() + " milliseconds");
        }
        Motion requested;
        synchronized (gate) {
            requireOwner();
            checkLocked();
            if (durationMs == 0) return;
            requested = new Motion(left, right, durationMs * NANOS_PER_MS,
                    clock.getAsLong() + options.armTimeoutMs() * NANOS_PER_MS);
            motion = requested;
        }
        await(requested.done);
    }

    /**
     * Turns left using -20% left and +20% right duty, then confirms zero drive before returning.
     * The duration is time-based and does not imply a measured turn angle.
     *
     * @param durationMs duration in milliseconds, 0..the configured maximum motion duration
     * @throws IllegalArgumentException if durationMs is invalid
     * @throws IllegalStateException when called from another thread
     * @throws InterruptedException if interrupted; motor authority is revoked
     * @throws TaskException if the task or hardware cannot continue
     */
    public void turnLeftFor(long durationMs) throws InterruptedException {
        driveFor(-20, 20, durationMs);
    }

    /**
     * Turns right using +20% left and -20% right duty, then confirms zero drive before returning.
     * The duration is time-based and does not imply a measured turn angle.
     *
     * @param durationMs duration in milliseconds, 0..the configured maximum motion duration
     * @throws IllegalArgumentException if durationMs is invalid
     * @throws IllegalStateException when called from another thread
     * @throws InterruptedException if interrupted; motor authority is revoked
     * @throws TaskException if the task or hardware cannot continue
     */
    public void turnRightFor(long durationMs) throws InterruptedException {
        driveFor(20, -20, durationMs);
    }

    /**
     * Returns one frame from a lazily opened camera while motor drive is zero. Camera opening and reads stay
     * on the task's platform thread. The pixel order is BGR888. The returned frame owns its buffer and remains
     * valid after later captures and task completion.
     *
     * @return a camera frame, suitable for student-supplied image analysis
     * @throws IllegalStateException when called from another thread
     * @throws TaskException if capture fails, the thread is interrupted, or the task is no longer active
     */
    public Frame takeImage() {
        long timeout;
        synchronized (gate) {
            requireOwner();
            checkLocked();
            timeout = Math.min(options.cameraTimeoutMs(),
                    Math.max(1, (taskDeadline - clock.getAsLong()) / NANOS_PER_MS));
        }
        Frame image;
        try {
            image = hardware.image(timeout);
        } catch (RuntimeException | Error error) {
            synchronized (gate) {
                failLocked(new TaskException(TaskException.Reason.CAMERA_FAILED, "Camera capture failed", error));
                throw failure;
            }
        }
        synchronized (gate) {
            checkLocked(); // A late camera result cannot revive an expired or cancelled task.
        }
        return image;
    }

    /** Reads the latest immutable observation without managing a control loop.
     * @return latest state, button and motor observation
     * @throws TaskException if the task is no longer active
     */
    public Snapshot snapshot() { checkOwner(); return hardware.snapshot(); }

    /** Reads supported board features.
     * @return immutable capability set
     */
    public Set<Capability> capabilities() { checkOwner(); return hardware.capabilities(); }

    /** Reads the board's opening report.
     * @return immutable report including degraded features
     */
    public OpenReport report() { checkOwner(); return hardware.report(); }

    /** Reads timing counters without exposing the controller.
     * @return immutable counters
     */
    public LoopStats stats() { checkOwner(); return hardware.stats(); }

    /** Sets one RGB light, without changing motor permission.
     * @param index light index 0..5
     * @param color packed 0xRRGGBB value
     */
    public void setRgb(int index, int color) { checkOwner(); hardware.setRgb(index, color); checkOwner(); }

    /** Sets all RGB lights to the same colour.
     * @param color packed 0xRRGGBB value
     */
    public void setRgbAll(int color) { checkOwner(); hardware.setRgbAll(color); checkOwner(); }

    /** Replaces the complete frame of RGB lights.
     * @param colors exactly six packed 0xRRGGBB values
     */
    public void setRgbFrame(int... colors) { checkOwner(); hardware.setRgbFrame(colors.clone()); checkOwner(); }

    /** Sets selected button indicator lights.
     * @param mask bit mask of indicators to change
     * @param values selected indicators to turn on
     */
    public void setButtonLeds(int mask, int values) { checkOwner(); hardware.setButtonLeds(mask, values); checkOwner(); }

    /** Takes an ultrasonic measurement without blocking the hardware controller.
     * @return result; use its valid flag before reading millimetres
     */
    public RangeMeasurement measureDistance() { checkOwner(); var result = hardware.measureDistance(); checkOwner(); return result; }

    /** Sets a diagnostic simulator input; the native library refuses this on a physical board.
     * @param item simulator input identifier
     * @param value the input's native integer value
     */
    public void simSet(SimItem item, int value) { checkOwner(); hardware.simSet(item, value); checkOwner(); }

    /** Validates task scope and thread affinity before peripheral access. */
    private void checkOwner() { synchronized (gate) { requireOwner(); checkLocked(); } }

    /**
     * Cancels this task permanently and disarms immediately. Safe from any thread. An active operation fails
     * with CANCELLED; later operations are refused, even if student code catches that exception.
     *
     * @throws TaskException if the native stop could not be confirmed; the task remains permanently cancelled
     */
    public void stop() {
        synchronized (gate) {
            if (!closed) failLocked(new TaskException(TaskException.Reason.CANCELLED, "Task was stopped"));
            if (failure != null && failure.getSuppressed().length != 0) throw failure;
        }
    }

    /** Verifies a normal student return before the runner closes hardware. */
    void verifyCompletion() {
        synchronized (gate) { checkLocked(); }
    }

    /** Removes hardware authority after an exception from student code, preserving that original exception. */
    void abort(Throwable cause) {
        synchronized (gate) {
            failLocked(new TaskException(TaskException.Reason.CANCELLED, "Task code failed", cause));
            if (failure != null && failure != cause) {
                for (Throwable stopError : failure.getSuppressed()) cause.addSuppressed(stopError);
            }
        }
    }

    /**
     * Permanently closes the scoped robot, disarming before joining its controller and closing its camera.
     * The task runner invokes this automatically on success and failure. Repeated calls have no effect.
     *
     * @throws IllegalStateException when called outside the owning task thread
     * @throws RuntimeException if underlying resource cleanup fails
     */
    @Override
    public void close() {
        synchronized (gate) {
            requireOwner();
            if (closed) return;
            closed = true;
            if (motion != null) {
                motion.done.completeExceptionally(new TaskException(TaskException.Reason.CLOSED, "Task has closed"));
                motion = null;
            }
        }
        // Joining a controller under gate would deadlock its last step.
        hardware.close();
    }

    /** Advances only bounded intents; arbitrary student code never runs on this control thread. */
    private void step(TaskHardware.Tick tick) {
        synchronized (gate) {
            if (closed || failure != null) { tick.coast(); return; }
            try {
                checkLocked();
                if (tick.state() == State.FAULTED) {
                    failLocked(new TaskException(TaskException.Reason.FAULT, "The board reported a fault"));
                    tick.coast();
                    return;
                }
                if (armedOnce && tick.state() != State.ARMED) {
                    failLocked(new TaskException(TaskException.Reason.FAULT, "The board unexpectedly disarmed"));
                    tick.coast();
                    return;
                }
                Motion current = motion;
                if (current == null) { tick.coast(); return; }
                long now = clock.getAsLong();
                if (!current.started) {
                    if (current.left != 0 || current.right != 0) {
                        if (tick.state() != State.ARMED) {
                            if (now - current.armDeadline >= 0) {
                                failLocked(new TaskException(TaskException.Reason.ARM_TIMEOUT, "Arming did not complete in time"));
                            } else if (!current.armRequested) {
                                current.armRequested = true;
                                tick.arm();
                            }
                            tick.coast();
                            return;
                        }
                        armedOnce = true;
                    }
                    current.started = true;
                    current.deadline = now + current.duration;
                }
                if (current.stopping) {
                    tick.coast();
                    if (tick.observedCycleId() >= current.stopCycle && tick.stationary()) {
                        motion = null;
                        current.done.complete(null);
                    }
                } else if (now - current.deadline >= 0) {
                    current.stopping = true;
                    current.stopCycle = tick.cycleId();
                    tick.coast();
                } else if (current.left == 0 && current.right == 0) {
                    tick.coast();
                } else {
                    tick.drive(current.left, current.right);
                }
            } catch (RuntimeException | Error error) {
                failLocked(error instanceof TaskException taskError ? taskError
                        : new TaskException(TaskException.Reason.LOOP_FAILED, "The task controller failed", error));
                tick.coast();
            }
        }
    }

    /** Waits in small interruptible intervals so a stopped native controller cannot strand an operation. */
    private void await(CompletableFuture<Void> completion) throws InterruptedException {
        try {
            while (true) {
                try {
                    completion.get(10, TimeUnit.MILLISECONDS);
                    synchronized (gate) { checkLocked(); }
                    return;
                } catch (TimeoutException waitAgain) {
                    synchronized (gate) { checkLocked(); }
                } catch (ExecutionException error) {
                    Throwable cause = error.getCause();
                    if (cause instanceof RuntimeException runtime) throw runtime;
                    if (cause instanceof Error fatal) throw fatal;
                    throw new TaskException(TaskException.Reason.LOOP_FAILED, "The task controller failed", cause);
                }
            }
        } catch (InterruptedException interrupted) {
            try { stop(); } catch (RuntimeException stopError) { interrupted.addSuppressed(stopError); }
            Thread.currentThread().interrupt();
            throw interrupted;
        }
    }

    /** Checks task lifetime and native controller state while gate is held. */
    private void checkLocked() {
        if (failure != null) throw failure;
        if (closed) throw new TaskException(TaskException.Reason.CLOSED, "Task has closed");
        if (owner.isInterrupted()) {
            failLocked(new TaskException(TaskException.Reason.CANCELLED, "Task thread was interrupted"));
        } else if (clock.getAsLong() - taskDeadline >= 0) {
            failLocked(new TaskException(TaskException.Reason.TASK_TIMEOUT, "Task exceeded its time limit"));
        } else if (hardware.failure() != null || hardware.ended()) {
            failLocked(new TaskException(TaskException.Reason.LOOP_FAILED, "The control loop failed or ended",
                    hardware.failure()));
        }
        if (failure != null) throw failure;
    }

    /** Latches the first terminal error and disarms; later errors cannot restore the active motion. */
    private void failLocked(TaskException error) {
        if (failure != null || closed) return;
        failure = error;
        Motion current = motion;
        motion = null;
        try { hardware.stop(); } catch (RuntimeException | Error stopError) { error.addSuppressed(stopError); }
        if (current != null) current.done.completeExceptionally(error);
    }

    /** Enforces task and camera affinity without changing the existing native ownership rules. */
    private void requireOwner() {
        if (Thread.currentThread() != owner) {
            throw new IllegalStateException("Task robot operations must run on the task thread; stop() works anywhere");
        }
    }

    /** Converts a validated percentage to the native signed-permille duty. */
    private static int duty(double value, String name) {
        if (!Double.isFinite(value) || value < -100 || value > 100) {
            throw new IllegalArgumentException(name + " must be finite -100..100 percent");
        }
        return (int) Math.round(value * 10);
    }

    /** One finite action, never shared with student code. */
    private static final class Motion {
        private final int left;
        private final int right;
        private final long duration;
        private final long armDeadline;
        private final CompletableFuture<Void> done = new CompletableFuture<>();
        private boolean armRequested;
        private boolean started;
        private boolean stopping;
        private long deadline;
        private long stopCycle;

        /** Captures a finite command and a separate bound for its first arming handshake. */
        private Motion(int left, int right, long duration, long armDeadline) {
            this.left = left;
            this.right = right;
            this.duration = duration;
            this.armDeadline = armDeadline;
        }
    }
}
