// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.errors.AgainException;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.OpenReport;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.Cycle;
import io.github.bisocmorg.keel.loop.LoopStats;
import io.github.bisocmorg.keel.peripherals.RangeMeasurement;
import io.github.bisocmorg.keel.peripherals.SensorPower;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.State;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Supplier;

/**
 * A long-lived interactive robot connection with explicit arming and finite, replaceable motor leases.
 *
 * <p>The session owns the hardware control loop. Applications may call its methods from their request or
 * application threads; they never tick the board. A drive refresh must arrive before its finite deadline.
 * Expiry disarms instead of keeping a previous speed alive. Unlike a sequential {@link KeelTask}, this
 * interactive session allows explicit disarming and fault recovery without replacing the connection.
 * It never clears a fault or re-arms automatically. Close the session when the server or example exits.
 */
public final class RobotSession implements AutoCloseable {
    private final Object gate = new Object();
    private final Device device;
    private final ControlLoop loop;
    private final TaskOptions options;
    private long generation;
    private long driveDeadline;
    private int left;
    private int right;
    private boolean closed;
    private boolean authorized;
    private long lastFaultCount;
    private int pressedButtons;
    private int releasedButtons;

    /** Opens a session using the default real-board configuration.
     * @return an open, initially disarmed session
     */
    public static RobotSession open() { return open(TaskOptions.defaults()); }

    /** Opens a session with explicit board settings, such as the real-time simulator.
     * @param options board configuration; virtual time is rejected
     * @return an open, initially disarmed session
     */
    public static RobotSession open(OpenOptions options) {
        return open(TaskOptions.defaults().withOpenOptions(options));
    }

    /** Opens an interactive session with configured maximum drive and arming durations.
     * Camera and whole-task limits are not used: this connection owns no camera and has an explicit lifetime.
     * @param options validated resource settings and finite action limits
     * @return an open, initially disarmed session
     */
    public static RobotSession open(TaskOptions options) { return new RobotSession(options); }

    /** Opens the device and starts its private controller without arming. */
    private RobotSession(TaskOptions options) {
        this.options = Objects.requireNonNull(options, "options");
        device = Keel.open(options.openOptions());
        try {
            loop = ControlLoop.builder(device, this::step).build();
            loop.start();
        } catch (RuntimeException | Error error) {
            try { device.close(); } catch (RuntimeException | Error cleanup) { error.addSuppressed(cleanup); }
            throw error;
        }
    }

    /** Applies only a currently valid finite drive lease, independently of application work. */
    private void step(Cycle cycle) {
        synchronized (gate) {
            if (closed) { cycle.coast(); return; }
            pressedButtons |= cycle.snapshot().buttonsPressedValue();
            releasedButtons |= cycle.snapshot().buttonsReleasedValue();
            if (cycle.snapshot().state() == State.FAULTED) {
                if (cycle.snapshot().faultCount() > lastFaultCount) {
                    generation++;
                    lastFaultCount = cycle.snapshot().faultCount();
                }
                authorized = false;
                driveDeadline = 0;
                left = right = 0;
            }
            if (driveDeadline != 0 && System.nanoTime() - driveDeadline >= 0) {
                haltLocked(false);
            }
            if (driveDeadline != 0 && cycle.snapshot().state() == State.ARMED) {
                cycle.drive(left, right);
            } else {
                cycle.coast();
            }
        }
    }

    /** Queues an explicit arming request. Completion means request acceptance, not completed arming.
     * @return a future for request acceptance; disarming cancels stale queued requests
     */
    public CompletableFuture<Void> requestArm() { return requestArm(() -> { }); }

    /** Queues arming with a short application permission check at native execution time.
     * The check receives no hardware handle. It runs outside the session lock; generation checks prevent a
     * concurrent stop from being undone. The check must not block or perform image/network work.
     * @param permissionCheck throws when ownership, request expiry or application permissions are invalid
     * @return a future for native request acceptance, not completion of the arming handshake
     */
    public CompletableFuture<Void> requestArm(Runnable permissionCheck) {
        return submit(permissionCheck, () -> { device.requestArm(); authorized = true; return null; });
    }

    /** Explicitly arms and waits for the actual armed state within the configured arming timeout.
     * @throws InterruptedException if the caller is interrupted; the session is disarmed
     * @throws TaskException if cancelled, timed out, closed or faulted
     */
    public void arm() throws InterruptedException {
        long ticket;
        synchronized (gate) { checkLocked(); ticket = generation; }
        long deadline = System.nanoTime() + options.armTimeoutMs() * 1_000_000L;
        try {
            waitFor(requestArm(), options.armTimeoutMs());
            while (true) {
                synchronized (gate) {
                    checkLocked();
                    if (ticket != generation) throw new TaskException(TaskException.Reason.CANCELLED, "Arming was cancelled");
                    State state = loop.snapshot().state();
                    if (state == State.ARMED) return;
                    if (state == State.FAULTED) throw new TaskException(TaskException.Reason.FAULT, "The board is faulted");
                }
                if (System.nanoTime() - deadline >= 0) {
                    disarm();
                    throw new TaskException(TaskException.Reason.ARM_TIMEOUT, "Arming did not complete in time");
                }
                Thread.sleep(5);
            }
        } catch (InterruptedException interrupted) {
            disarm(); Thread.currentThread().interrupt(); throw interrupted;
        } catch (RuntimeException failure) {
            disarm(); throw failure;
        }
    }

    /** Replaces motor intent with a finite lease. This method never arms the board.
     * @param leftPercent signed left motor duty in -100..100 percent
     * @param rightPercent signed right motor duty in -100..100 percent
     * @param leaseMs remaining validity, 1..configured maximum motion duration, in milliseconds
     * @throws IllegalArgumentException for invalid values
     * @throws TaskException if the connection is not armed or is closed
     */
    public void refreshDrive(double leftPercent, double rightPercent, long leaseMs) {
        if (leaseMs < 1 || leaseMs > options.maxMotionMs()) throw new IllegalArgumentException("leaseMs exceeds finite motion limit");
        refreshDriveUntil(leftPercent, rightPercent, System.nanoTime() + leaseMs * 1_000_000L);
    }

    /** Replaces motor intent without extending an application's original absolute expiry.
     * @param leftPercent signed left duty in percent
     * @param rightPercent signed right duty in percent
     * @param expiresAtNanos expiry in the System.nanoTime clock, no more than maxMotionMs ahead
     * @throws IllegalArgumentException if duties or expiry are outside their bounds
     * @throws TaskException if expired, closed, or not armed
     */
    public void refreshDriveUntil(double leftPercent, double rightPercent, long expiresAtNanos) {
        int l = duty(leftPercent), r = duty(rightPercent);
        synchronized (gate) {
            checkLocked();
            long now = System.nanoTime();
            if (driveDeadline != 0 && now - driveDeadline >= 0) {
                haltLocked(false);
                throw new TaskException(TaskException.Reason.CANCELLED, "Previous drive lease expired; explicitly arm again");
            }
            long remaining = expiresAtNanos - now;
            if (remaining <= 0) throw new TaskException(TaskException.Reason.CANCELLED, "Drive request expired");
            if (remaining > options.maxMotionMs() * 1_000_000L) throw new IllegalArgumentException("Drive expiry exceeds motion limit");
            if (!authorized || loop.snapshot().state() != State.ARMED) throw new TaskException(TaskException.Reason.FAULT, "Explicitly arm before driving");
            left = l; right = r;
            driveDeadline = (l == 0 && r == 0) ? 0 : expiresAtNanos;
            if (driveDeadline == 0) device.coast();
        }
    }

    /** Disarms immediately and invalidates every older queued operation; the session remains usable. */
    public void disarm() { synchronized (gate) { if (!closed) haltLocked(false); } }

    /** Removes motor drive while retaining existing arming permission; it does not grant permission.
     * Subsequent driving still requires an explicit current-generation arm.
     */
    public void coast() {
        synchronized (gate) {
            checkLocked();
            if (driveDeadline != 0 && System.nanoTime() - driveDeadline >= 0) { haltLocked(false); return; }
            driveDeadline = 0; left = right = 0; device.coast();
        }
    }

    /** Latches an emergency stop and invalidates older operations; explicit fault recovery is required. */
    public void emergencyStop() { synchronized (gate) { if (!closed) haltLocked(true); } }

    /** Revokes a finite lease before issuing a native stop. */
    private void haltLocked(boolean emergency) {
        generation++; authorized = false; driveDeadline = 0; left = right = 0;
        if (emergency) device.emergencyStop(); else device.disarm();
        // This stop already invalidated the generation; do not invalidate fresh recovery/LED work again.
        lastFaultCount = Math.max(lastFaultCount, device.snapshot().faultCount());
    }

    /** Explicitly clears a recoverable native fault, without arming or restoring motor intent.
     * @throws InterruptedException if interrupted while waiting
     */
    public void clearFault() throws InterruptedException { waitFor(clearFaultAsync(() -> { }), options.armTimeoutMs()); }

    /** Queues explicit fault clearing after a short application permission check.
     * @param permissionCheck application ownership/expiry validation
     * @return future completing when the native operation finishes
     */
    public CompletableFuture<Void> clearFaultAsync(Runnable permissionCheck) {
        return submit(permissionCheck, () -> { device.clearFault(); return null; });
    }

    /** Reads the latest complete device observation from any thread.
     * @return the immutable latest snapshot
     */
    public Snapshot snapshot() { synchronized (gate) { checkLocked(); return loop.snapshot(); } }

    /** Consumes button press edges accumulated since the previous call, even across slow application work.
     * @return a bit mask of buttons pressed at least once
     */
    public int buttonsPressed() { synchronized (gate) { checkLocked(); int value = pressedButtons; pressedButtons = 0; return value; } }

    /** Consumes button release edges accumulated since the previous call.
     * @return a bit mask of buttons released at least once
     */
    public int buttonsReleased() { synchronized (gate) { checkLocked(); int value = releasedButtons; releasedButtons = 0; return value; } }

    /** Reads the control-loop timing counters without exposing the controller.
     * @return immutable timing counters
     */
    public LoopStats stats() { synchronized (gate) { checkLocked(); return loop.stats(); } }

    /** Reports whether the private control loop is running.
     * @return true while the session can service requests
     */
    public boolean running() { synchronized (gate) { return !closed && loop.isRunning(); } }

    /** Returns the board's supported operations.
     * @return immutable capability set
     */
    public Set<Capability> capabilities() { synchronized (gate) { checkLocked(); return device.capabilities(); } }

    /** Returns the original opening report for diagnostics.
     * @return immutable report
     */
    public OpenReport report() { return device.report(); }

    /** Returns the current native fault details.
     * @return the native fault record
     */
    public Fault fault() { synchronized (gate) { checkLocked(); return device.fault(); } }

    /** Sets one RGB LED.
     * @param index LED index, 0..5
     * @param color packed 0xRRGGBB value
     * @throws InterruptedException if interrupted while waiting
     */
    public void setRgb(int index, int color) throws InterruptedException { waitFor(setRgbAsync(index, color, () -> { }), options.armTimeoutMs()); }

    /** Queues an RGB update with an application ownership check.
     * @param index LED index, or -1 to update all six
     * @param color packed 0xRRGGBB value
     * @param permissionCheck short permission validation at execution
     * @return completion future
     */
    public CompletableFuture<Void> setRgbAsync(int index, int color, Runnable permissionCheck) {
        return submit(permissionCheck, () -> { if (index < 0) device.setRgbAll(color); else device.setRgb(index, color); return null; });
    }

    /** Sets all RGB LEDs to one colour.
     * @param color packed 0xRRGGBB value
     * @throws InterruptedException if interrupted while waiting
     */
    public void setRgbAll(int color) throws InterruptedException { setRgb(-1, color); }

    /** Replaces all six RGB LED colours.
     * @param colors exactly six packed 0xRRGGBB values
     * @throws InterruptedException if interrupted while waiting
     */
    public void setRgbFrame(int... colors) throws InterruptedException {
        int[] copy = colors.clone(); waitFor(submit(() -> { }, () -> { device.setRgbFrame(copy); return null; }), options.armTimeoutMs());
    }

    /** Reads the cached RGB frame.
     * @return six packed RGB values
     */
    public int[] rgb() { synchronized (gate) { checkLocked(); int[] colors = new int[6]; for (int i = 0; i < colors.length; i++) colors[i] = device.rgb(i); return colors; } }

    /** Sets selected button indicator LEDs.
     * @param mask bit mask selecting indicators
     * @param values bits to turn on within mask
     * @throws InterruptedException if interrupted while waiting
     */
    public void setButtonLeds(int mask, int values) throws InterruptedException {
        waitFor(setButtonLedsAsync(mask, values, () -> { }), options.armTimeoutMs());
    }

    /** Queues indicator changes with a short execution-time permission check.
     * @param mask indicators to change
     * @param values desired on bits
     * @param permissionCheck application ownership/expiry validation
     * @return completion future
     */
    public CompletableFuture<Void> setButtonLedsAsync(int mask, int values, Runnable permissionCheck) {
        return submit(permissionCheck, () -> { device.setButtonLeds(mask, values); return null; });
    }

    /** Measures distance on the caller's thread, without blocking the hardware controller.
     * @return a measurement whose valid flag must be checked before using millimetres
     */
    public RangeMeasurement measureDistance() {
        synchronized (gate) { checkLocked(); }
        long deadline = System.nanoTime() + 1_000_000_000L;
        while (snapshot().sensorPower() != SensorPower.PERMITTED && snapshot().sensorPower() != SensorPower.CONFIRMED
                && snapshot().state() != State.FAULTED && System.nanoTime() - deadline < 0) {
            try { Thread.sleep(5); }
            catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new TaskException(TaskException.Reason.CANCELLED, "Range wait interrupted", interrupted); }
        }
        while (true) {
            try { return device.rangeMeasure(); }
            catch (AgainException pending) {
                if (System.nanoTime() - deadline >= 0) throw pending;
                try { Thread.sleep(5); }
                catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new TaskException(TaskException.Reason.CANCELLED, "Range wait interrupted", interrupted); }
            }
        }
    }

    /** Supplies a simulator stimulus; a physical board refuses this diagnostic operation.
     * @param item simulator input to change
     * @param value native value for that input
     */
    public void simSet(SimItem item, int value) { synchronized (gate) { checkLocked(); device.simSet(item, value); } }

    /** Validates and queues a short native operation without publishing the raw device. */
    private <T> CompletableFuture<T> submit(Runnable permissionCheck, Supplier<T> operation) {
        Objects.requireNonNull(permissionCheck, "permissionCheck");
        long ticket;
        synchronized (gate) { checkLocked(); ticket = generation; }
        long expires = System.nanoTime() + options.armTimeoutMs() * 1_000_000L;
        CompletableFuture<T> result = new CompletableFuture<>();
        loop.submit(ignored -> {
            permissionCheck.run();
            synchronized (gate) {
                checkLocked();
                if (result.isCancelled() || ticket != generation || System.nanoTime() - expires >= 0)
                    throw new TaskException(TaskException.Reason.CANCELLED, "Operation cancelled or expired before execution");
                return operation.get();
            }
        }).whenComplete((value, error) -> { if (error != null) result.completeExceptionally(error); else result.complete(value); });
        return result;
    }

    /** Waits for one finite native command without swallowing its original exception. */
    private static <T> T waitFor(CompletableFuture<T> future, long timeoutMs) throws InterruptedException {
        try { return future.get(timeoutMs, TimeUnit.MILLISECONDS); }
        catch (TimeoutException error) { future.cancel(false); throw new TaskException(TaskException.Reason.LOOP_FAILED, "Native command timed out", error); }
        catch (ExecutionException error) {
            if (error.getCause() instanceof RuntimeException runtime) throw runtime;
            if (error.getCause() instanceof Error fatal) throw fatal;
            throw new TaskException(TaskException.Reason.LOOP_FAILED, "Native command failed", error.getCause());
        }
    }

    /** Rejects calls after closure or a failed controller. */
    private void checkLocked() {
        if (closed) throw new TaskException(TaskException.Reason.CLOSED, "Robot session is closed");
        if (loop != null && (loop.isEnded() || loop.error().isPresent())) throw new TaskException(TaskException.Reason.LOOP_FAILED, "Robot controller failed");
    }

    /** Converts an explicitly bounded duty percentage to native permille. */
    private static int duty(double value) {
        if (!Double.isFinite(value) || value < -100 || value > 100) throw new IllegalArgumentException("Duty must be finite -100..100 percent");
        return (int) Math.round(value * 10);
    }

    /** Disarms and closes the session from any thread; repeated calls have no effect. */
    @Override public void close() {
        Throwable failure = null;
        synchronized (gate) {
            if (closed) return;
            closed = true;
            generation++; authorized = false; driveDeadline = 0; left = right = 0;
            try { device.disarm(); } catch (RuntimeException | Error error) { failure = error; }
        }
        try { loop.close(); } catch (RuntimeException | Error error) { if (failure == null) failure = error; else failure.addSuppressed(error); }
        try { device.close(); } catch (RuntimeException | Error error) { if (failure == null) failure = error; else failure.addSuppressed(error); }
        if (failure instanceof RuntimeException runtime) throw runtime;
        if (failure instanceof Error error) throw error;
    }
}
