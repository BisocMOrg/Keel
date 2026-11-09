// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.loop;

import static java.lang.foreign.ValueLayout.JAVA_LONG;

import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.errors.InvalidArgumentException;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.SafetyFaultException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.errors.StatusClass;
import io.github.bisocmorg.keel.internal.AccessRegistry;
import io.github.bisocmorg.keel.internal.DeviceAccess;
import io.github.bisocmorg.keel.internal.ErrorAccess;
import io.github.bisocmorg.keel.internal.Layouts;
import io.github.bisocmorg.keel.internal.StructCodec;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.State;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.ref.Reference;
import java.time.Duration;
import java.util.EnumMap;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Predicate;

/**
 * The control loop of libkeel's binding rules: one {@code keel_cycle} per period on a dedicated platform thread, on
 * the device clock, with honest health, every outcome dispatched by class, and no keepalive.
 *
 * <p>Each cycle runs the requests queued from other threads ({@link #requestArm()}, {@link #clearFault()},
 * {@link #submit(Function)}), then the step, then one {@code keel_cycle} carrying the health and the motor command,
 * then sleeps until the next period on the device clock. Health is {@link Health#ALL} only if that work completed
 * without an exception, within the step budget and with the application's I/O healthy; otherwise the tick
 * fail-stops on purpose, and the fault record's {@code detail} names the missing bits.
 *
 * <p>The loop never arms, never clears a fault and never re-sends a command on the application's behalf. In
 * FAULTED it keeps running without motion, so the step can show an error colour and read buttons, and so
 * {@link #clearFault()} can run on the owner thread. One loop serves a device for the device's whole life: a dead
 * loop thread means close and reopen.
 *
 * <pre>{@code
 * try (Device device = Keel.open(OpenOptions.builder().backend(Backend.SIMULATOR).build());
 *         ControlLoop loop = ControlLoop.builder(device, cycle -> {
 *             if (cycle.snapshot().state() == State.ARMED) {
 *                 cycle.drive(200, 200);
 *             }
 *         }).build()) {
 *     loop.start();
 *     loop.requestArm().join();
 *     Thread.sleep(3000);
 * }
 * }</pre>
 *
 * <p><b>Threads.</b> {@link #start()} runs the loop on a new non-daemon platform thread named {@code keel-loop};
 * {@link #run(long)} and {@link #runUntil(Predicate, long)} run it synchronously on the calling platform thread, which
 * is how virtual-clock tests drive a device step by step. The stop methods and the posting methods are safe from any
 * thread, and {@link #join()} and {@link #close()} wait for the loop however it runs. Interrupting the thread that
 * runs the loop is a stop request: the loop disarms and ends, and the thread's interrupt status stays set.
 *
 * <p><b>Errors.</b> The loop's fatal error is a {@link KeelException}, an {@link Error} that escaped the step, or a
 * {@link RuntimeException} thrown by the {@code runUntil} condition. {@code run}, {@code runUntil} and {@code join}
 * rethrow it as it is; {@code close()} rethrows it only if none of them has, so try-with-resources reports it once.
 */
public final class ControlLoop implements AutoCloseable {
    private static final System.Logger LOG = System.getLogger("io.github.bisocmorg.keel");
    private static final ErrorAccess ERRORS = AccessRegistry.errors();
    private static final DeviceAccess DEVICES = AccessRegistry.devices();
    private static final Intent COAST = new Intent(CycleCommand.DRIVE.code(), 0, 0);
    private static final Intent BRAKE = new Intent(CycleCommand.BRAKE.code(), 0, 0);

    /**
     * The application's work for one cycle.
     *
     * <p>It runs on the loop's thread, the control owner, so calls on {@link Cycle#device()} from here are go calls
     * from the owner. An exception it throws makes this cycle unhealthy (the tick fail-stops) and is kept in
     * {@link ControlLoop#error()}; an {@link Error} also makes the loop stop the device with an emergency stop and
     * end.
     */
    @FunctionalInterface
    public interface Step {
        /**
         * Runs one cycle's work.
         *
         * @param cycle the cycle: the latest snapshot and the command for this cycle
         * @throws Exception when this cycle's work failed
         */
        void step(Cycle cycle) throws Exception;
    }

    private final Device device;
    private final Step step;
    private final long periodUs;
    private final long budgetUs;
    private final boolean takeButtons;
    private final Consumer<Fault> onFault;
    private final String threadName;
    private final Cycle cycle;
    private final ConcurrentLinkedQueue<Request<?>> requests = new ConcurrentLinkedQueue<>();
    private final AtomicReference<Intent> intent = new AtomicReference<>();
    private final AtomicBoolean running = new AtomicBoolean();
    private final ReentrantLock idleLock = new ReentrantLock();
    private final Condition idle = idleLock.newCondition();
    private volatile boolean stopRequested;
    private volatile boolean ended;
    private volatile Thread cycling;
    private volatile Snapshot snapshot;
    private volatile Fault fault;
    private volatile Throwable error;
    private volatile Throwable fatal;
    private volatile boolean fatalDelivered;
    // Fault latches already reported (keel_snapshot.fault_count); used by the thread running the loop.
    private long reportedFaults;

    private final AtomicLong cycles = new AtomicLong();
    private final AtomicLong overruns = new AtomicLong();
    private final AtomicLong maxGapUs = new AtomicLong();
    private final AtomicLong maxStepUs = new AtomicLong();
    private final AtomicLong stepErrors = new AtomicLong();
    private final AtomicLong faults = new AtomicLong();
    private final Map<Integer, AtomicLong> rejected = new ConcurrentHashMap<>();
    private final RateLimiter stepErrorLog = new RateLimiter();
    private final RateLimiter rejectedLog = new RateLimiter();
    private final RateLimiter overrunLog = new RateLimiter();
    private final RateLimiter peripheralLog = new RateLimiter();

    private ControlLoop(Builder builder) {
        device = builder.device;
        step = builder.step;
        periodUs = builder.periodMs * 1000L;
        budgetUs = builder.stepBudgetMs * 1000L;
        takeButtons = builder.takeButtons;
        onFault = builder.onFault;
        threadName = builder.threadName;
        cycle = new Cycle(device);
    }

    /**
     * Starts a builder for a loop on a device.
     *
     * @param device the device; one loop serves it for its whole life
     * @param step the application's work per cycle, or {@code null} for a loop that only ticks
     * @return a new builder
     */
    public static Builder builder(Device device, Step step) {
        return new Builder(device, step);
    }

    /** Builds a {@link ControlLoop}; every setter returns the builder. */
    public static final class Builder {
        private final Device device;
        private final Step step;
        private int periodMs = Limits.TICK_PERIOD_MS;
        private int stepBudgetMs = Limits.STALL_BUDGET_MS;
        private boolean takeButtons = true;
        private Consumer<Fault> onFault;
        private String threadName = "keel-loop";

        private Builder(Device device, Step step) {
            this.device = Objects.requireNonNull(device, "device");
            this.step = step;
        }

        /**
         * Sets the period: 1 to {@link Limits#TICK_PERIOD_MS} (the default, 10 ms).
         *
         * @param value the period in milliseconds
         * @return this builder
         * @throws IllegalArgumentException if the value is out of range
         */
        public Builder periodMs(int value) {
            if (value < 1 || value > Limits.TICK_PERIOD_MS) {
                throw new IllegalArgumentException("periodMs must be 1.." + Limits.TICK_PERIOD_MS + ", not " + value);
            }
            periodMs = value;
            return this;
        }

        /**
         * Sets the step budget: 1 to {@link Limits#STALL_BUDGET_MS} (the default, 40 ms). It covers all application
         * work of one cycle, the queued requests and the step; a cycle over budget reports health without
         * {@link Health#CONTROL_OK}.
         *
         * @param value the budget in milliseconds
         * @return this builder
         * @throws IllegalArgumentException if the value is out of range
         */
        public Builder stepBudgetMs(int value) {
            if (value < 1 || value > Limits.STALL_BUDGET_MS) {
                throw new IllegalArgumentException("stepBudgetMs must be 1.." + Limits.STALL_BUDGET_MS + ", not "
                        + value);
            }
            stepBudgetMs = value;
            return this;
        }

        /**
         * Sets whether each cycle takes the button edges ({@link CycleFlag#TAKE_BUTTONS}); the default is
         * {@code true}, so the snapshot's pressed and released masks are this cycle's edges.
         *
         * @param value whether to take the edges
         * @return this builder
         */
        public Builder takeButtons(boolean value) {
            takeButtons = value;
            return this;
        }

        /**
         * Sets a callback for fault latches. It runs once per latch, on the loop's thread, with the fault record,
         * also for a fault that latches again in the cycle that cleared the previous one.
         *
         * @param value the callback, or {@code null} for none
         * @return this builder
         */
        public Builder onFault(Consumer<Fault> value) {
            onFault = value;
            return this;
        }

        /**
         * Sets the name of the thread {@link ControlLoop#start()} creates; the default is {@code keel-loop}.
         *
         * @param value the thread name
         * @return this builder
         */
        public Builder threadName(String value) {
            threadName = Objects.requireNonNull(value, "threadName");
            return this;
        }

        /**
         * Builds the loop and attaches it to the device.
         *
         * @return the loop
         * @throws IllegalStateException if a loop already serves the device
         */
        public ControlLoop build() {
            if (!DEVICES.attachLoop(device)) {
                throw new IllegalStateException("a control loop already serves this device; one loop serves a "
                        + "device for its whole life");
            }
            ControlLoop loop = new ControlLoop(this);
            Snapshot current = device.snapshot();
            loop.snapshot = current;
            // A fault that is active when the loop is built is still reported once.
            loop.reportedFaults = Math.max(0, current.faultCount()
                    - (current.stateValue() == State.FAULTED.code() ? 1 : 0));
            return loop;
        }
    }

    // ---------------------------------------------------------------- running ----

    /**
     * Runs the loop on a new non-daemon platform thread until it is stopped or ends. Allowed from a virtual thread,
     * since it only creates the loop's own platform thread.
     *
     * @throws IllegalStateException if the loop is running or has ended, or if libkeel has recorded a control owner:
     *     the new thread would not be that owner, and its first tick would fail-stop
     */
    public void start() {
        if (ended) {
            throw endedError();
        }
        if (!running.compareAndSet(false, true)) {
            throw new IllegalStateException("the control loop is already running");
        }
        try {
            long owner = device.snapshot().ownerThreadId();
            if (owner != 0) {
                throw new IllegalStateException("libkeel has recorded control owner thread " + owner + ", so the "
                        + "loop's new thread would fail-stop with TICK_THREAD_CHANGED; close and reopen the device, "
                        + "or call run() on the owner thread");
            }
            Thread.ofPlatform().name(threadName).daemon(false).start(this::threadMain);
        } catch (RuntimeException | Error failure) {
            markIdle();
            throw failure;
        }
    }

    private void threadMain() {
        cycling = Thread.currentThread();
        try {
            execute(Long.MAX_VALUE, null);
            if (!ended) {
                end();
            }
        } finally {
            cycling = null;
            markIdle();
        }
    }

    /** Marks the loop as not running and wakes the threads waiting in {@link #join()} or {@link #close()}. */
    private void markIdle() {
        idleLock.lock();
        try {
            running.set(false);
            idle.signalAll();
        } finally {
            idleLock.unlock();
        }
    }

    /**
     * Runs a number of cycles synchronously on the calling thread, then returns. Repeated calls on one thread
     * continue the same loop. On the virtual clock this drives the device deterministically; on the real-time clock
     * a gap between calls longer than the 70 ms tick window while armed is {@link FaultCause#TICK_LATE}.
     *
     * @param cycleCount the number of cycles, at least 0
     * @return the latest snapshot
     * @throws WrongThreadException on a virtual thread
     * @throws IllegalStateException if the loop is running elsewhere or has ended, or another thread is the control
     *     owner
     * @throws KeelException the loop's fatal error, if it ended with one; an {@link Error} that escaped the step is
     *     rethrown as it is
     */
    public Snapshot run(long cycleCount) {
        return runHere(cycleCount, null);
    }

    /**
     * Runs cycles synchronously on the calling thread until a snapshot satisfies a condition or a number of cycles
     * has run.
     *
     * @param done tested with the snapshot after each cycle; an exception it throws ends the loop (disarming) and
     *     is rethrown
     * @param maxCycles the most cycles to run, at least 0
     * @return the latest snapshot
     * @throws WrongThreadException on a virtual thread
     * @throws IllegalStateException as {@link #run(long)}
     * @throws KeelException the loop's fatal error, if it ended with one; an {@link Error} that escaped the step or
     *     a {@link RuntimeException} thrown by {@code done} is rethrown as it is
     */
    public Snapshot runUntil(Predicate<Snapshot> done, long maxCycles) {
        return runHere(maxCycles, Objects.requireNonNull(done, "done"));
    }

    private Snapshot runHere(long maxCycles, Predicate<Snapshot> done) {
        if (Thread.currentThread().isVirtual()) {
            throw new WrongThreadException("ControlLoop.run must run on a platform thread: libkeel's control owner "
                    + "is an OS thread, and a virtual thread's carrier can change between cycles");
        }
        if (maxCycles < 0) {
            throw new IllegalArgumentException("the cycle count must not be negative, not " + maxCycles);
        }
        if (ended) {
            throw endedError();
        }
        if (!running.compareAndSet(false, true)) {
            throw new IllegalStateException("the control loop is already running on another thread");
        }
        try {
            long owner = device.snapshot().ownerThreadId();
            if (owner != 0 && owner != DEVICES.library(device).gettid()) {
                throw new IllegalStateException("libkeel has recorded control owner thread " + owner + ", not this "
                        + "thread, so a tick from here would fail-stop with TICK_THREAD_CHANGED; run the loop on the "
                        + "owner thread, or close and reopen the device");
            }
            cycling = Thread.currentThread();
            execute(maxCycles, done);
        } finally {
            cycling = null;
            markIdle();
        }
        rethrowFatal();
        return snapshot;
    }

    private static IllegalStateException endedError() {
        return new IllegalStateException("the control loop has ended; one loop serves a device for its whole life, "
                + "so close the device and open a new one");
    }

    /**
     * Runs cycles on the current thread; returns when the count is reached, the condition holds or the loop ends. A
     * cycle makes four foreign calls: two clock reads around the application's work, {@code keel_cycle} and
     * {@code keel_sleep_until}.
     */
    private void execute(long maxCycles, Predicate<Snapshot> done) {
        boolean interrupted = false;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment in = StructCodec.allocate(arena, Layouts.CYCLE_INPUT);
            MemorySegment out = StructCodec.allocate(arena, Layouts.SNAPSHOT);
            MemorySegment clock = arena.allocate(JAVA_LONG);
            Snapshot latest = device.snapshot();
            publish(latest);
            long id = latest.lastCycleId();
            long next = DEVICES.nowUsRaw(device, clock);
            long lastCall = -1;
            for (long count = 0; count < maxCycles; count++) {
                if (stopRequested || next < 0) {
                    break;
                }
                if (Thread.interrupted()) {
                    interrupted = true;
                    LOG.log(System.Logger.Level.INFO, "the loop thread was interrupted: disarming and ending");
                    stopRequested = true;
                    stopQuietly(false);
                    break;
                }
                next += periodUs;
                long workStart = DEVICES.nowUsRaw(device, clock);
                long stepStart = runRequests() ? DEVICES.nowUsRaw(device, clock) : workStart;
                if (workStart < 0 || stepStart < 0) {
                    endOnHandle(Status.E_INVALID_HANDLE.code(), "keel_now_us");
                    return;
                }
                cycle.reset(snapshot, ++id, stepStart);
                int health = Health.ALL.mask();
                if (step != null) {
                    try {
                        step.step(cycle);
                    } catch (Exception failure) {
                        health &= ~Health.NO_EXCEPTION.mask();
                        error = failure;
                        stepErrors.incrementAndGet();
                        if (stepErrorLog.allow()) {
                            LOG.log(System.Logger.Level.ERROR, "the control step threw; this cycle reports health "
                                    + "without NO_EXCEPTION and the tick fail-stops", failure);
                        }
                    }
                }
                if (cycle.dutyRefused()) {
                    health &= ~Health.NO_EXCEPTION.mask();
                }
                long workEnd = DEVICES.nowUsRaw(device, clock);
                if (workEnd < 0) {
                    endOnHandle(Status.E_INVALID_HANDLE.code(), "keel_now_us");
                    return;
                }
                maxStepUs.accumulateAndGet(workEnd - workStart, Math::max);
                if (workEnd - workStart > budgetUs) {
                    health &= ~Health.CONTROL_OK.mask();
                }
                if (!cycle.ioOk()) {
                    health &= ~Health.IO_OK.mask();
                }
                StructCodec.writeCycleInput(in, input(health, id));
                if (lastCall >= 0) {
                    maxGapUs.accumulateAndGet(workEnd - lastCall, Math::max);
                }
                lastCall = workEnd;
                int status = DEVICES.cycleRaw(device, in, out);
                cycles.incrementAndGet();
                if (!dispatch(status, out)) {
                    return;
                }
                // dispatch continues only when keel_cycle wrote the snapshot, which carries the time it was taken.
                long now = snapshot.timeUs();
                if (now > next + periodUs) {
                    next = now;
                    overruns.incrementAndGet();
                    if (overrunLog.allow()) {
                        LOG.log(System.Logger.Level.WARNING, "the control loop overran its period ({0} overruns so "
                                + "far); it resynchronises instead of catching up", overruns.get());
                    }
                }
                int slept = DEVICES.sleepUntilRaw(device, next);
                if (slept != 0 && !dispatchSleep(slept)) {
                    return;
                }
                if (done != null && done.test(snapshot)) {
                    break;
                }
            }
            if (stopRequested) {
                end();
            } else if (next < 0) {
                endOnHandle(Status.E_INVALID_HANDLE.code(), "keel_now_us");
            }
        } catch (HandleException closed) {
            endOnHandle(closed.code(), closed.function());
        } catch (Error failure) {
            LOG.log(System.Logger.Level.ERROR, "an Error escaped the control loop: emergency stop", failure);
            stopQuietly(true);
            fail(failure);
        } catch (RuntimeException failure) {
            LOG.log(System.Logger.Level.ERROR, "the control loop failed: disarming", failure);
            stopQuietly(false);
            fail(failure);
        } finally {
            Reference.reachabilityFence(device);
            if (interrupted) {
                Thread.currentThread().interrupt(); // the stop request is honoured; the caller still sees it
            }
        }
    }

    /** The cycle input: the step's command wins over a posted intent; no motion is sent while FAULTED. */
    private CycleInput input(int health, long id) {
        Intent posted = intent.getAndSet(null);
        int command = cycle.command();
        int left = cycle.left();
        int right = cycle.right();
        if (command == CycleCommand.NONE.code() && posted != null) {
            command = posted.command();
            left = posted.left();
            right = posted.right();
        }
        if (snapshot.stateValue() == State.FAULTED.code()) {
            command = CycleCommand.NONE.code();
            left = 0;
            right = 0;
        }
        return new CycleInput(health, id, command, left, right, takeButtons ? CycleFlag.TAKE_BUTTONS.mask() : 0);
    }

    /**
     * Acts on a {@code keel_cycle} result by class.
     *
     * @return whether the loop continues
     */
    private boolean dispatch(int status, MemorySegment out) {
        if (status == 0) {
            publish(StructCodec.snapshot(out));
            return true;
        }
        int classValue = DEVICES.library(device).statusClass(status);
        switch (StatusClass.of(classValue)) {
            case REJECTED -> {
                if (status == Status.E_INVALID_ARGUMENT.code() || status == Status.E_UNSUPPORTED.code()) {
                    Snapshot written = status == Status.E_UNSUPPORTED.code() ? StructCodec.snapshot(out) : null;
                    KeelException failure = ERRORS.of(status, "keel_cycle", () -> DEVICES.faultOrNull(device), written);
                    LOG.log(System.Logger.Level.ERROR, "keel_cycle refused the loop's own input, a defect in the "
                            + "binding: disarming and ending the loop", failure);
                    stopQuietly(false);
                    fail(failure);
                    return false;
                }
                rejected.computeIfAbsent(status, code -> new AtomicLong()).incrementAndGet();
                if (rejectedLog.allow()) {
                    LOG.log(System.Logger.Level.DEBUG, "keel_cycle: {0}: {1}", Keel.statusName(status),
                            Keel.statusString(status));
                }
                publish(StructCodec.snapshot(out));
                return true;
            }
            case PERIPHERAL -> {
                if (peripheralLog.allow()) {
                    LOG.log(System.Logger.Level.WARNING, "keel_cycle: {0}", Keel.statusString(status));
                }
                publish(StructCodec.snapshot(out));
                return true;
            }
            case HANDLE -> {
                endOnHandle(status, "keel_cycle");
                return false;
            }
            default -> {
                // FAULT, and any class this binding does not know: the output is written and shows the latch.
                publish(StructCodec.snapshot(out));
                return true;
            }
        }
    }

    /**
     * Acts on a failed {@code keel_sleep_until}.
     *
     * @return whether the loop continues
     */
    private boolean dispatchSleep(int status) {
        StatusClass statusClass = StatusClass.of(DEVICES.library(device).statusClass(status));
        if (statusClass == StatusClass.HANDLE) {
            endOnHandle(status, "keel_sleep_until");
            return false;
        }
        // The device clock failed (a sticky FAULT) or refused the deadline: the loop cannot pace itself any more.
        if (statusClass == StatusClass.REJECTED) {
            stopQuietly(false);
        } else {
            try {
                publish(device.snapshot()); // reports the clock fault's latch
            } catch (KeelException unreadable) {
                LOG.log(System.Logger.Level.DEBUG, "the snapshot after the clock fault could not be read", unreadable);
            }
        }
        fail(ERRORS.of(status, "keel_sleep_until", () -> DEVICES.faultOrNull(device), null));
        return false;
    }

    /**
     * Publishes a snapshot, and reports a fault latch that it shows for the first time. A latch is recognised by the
     * rise of {@code fault_count}, not by the state, so a fault that latches again in the cycle whose
     * {@link #clearFault()} request ended the previous one is reported as well.
     */
    private void publish(Snapshot latest) {
        snapshot = latest;
        if (latest.faultCount() > reportedFaults) {
            enterFaulted(latest.faultCount());
        }
    }

    private void enterFaulted(long faultCount) {
        reportedFaults = faultCount;
        faults.incrementAndGet();
        Fault record = DEVICES.faultOrNull(device);
        fault = record;
        if (record == null) {
            LOG.log(System.Logger.Level.ERROR, "the device is FAULTED; its fault record could not be read");
            return;
        }
        LOG.log(System.Logger.Level.ERROR, "the device is FAULTED: {0}, cause {1}, detail {2}; the loop keeps "
                + "running without motion until ControlLoop.clearFault()", record.status(), record.cause(),
                Long.toUnsignedString(record.detail()));
        if (onFault != null) {
            try {
                onFault.accept(record);
            } catch (RuntimeException failure) {
                error = failure;
                LOG.log(System.Logger.Level.ERROR, "the onFault callback threw", failure);
            }
        }
    }

    /**
     * Runs the queued requests on this thread.
     *
     * @return whether any ran
     */
    private boolean runRequests() {
        boolean ran = false;
        for (Request<?> request = requests.poll(); request != null; request = requests.poll()) {
            request.run(device);
            ran = true;
        }
        return ran;
    }

    /** A HANDLE result ends the loop: normally when the device is closing, otherwise with the error kept. */
    private void endOnHandle(int status, String function) {
        if (!DEVICES.closing(device)) {
            fail(ERRORS.of(status, function, () -> DEVICES.faultOrNull(device), null));
            return;
        }
        LOG.log(System.Logger.Level.DEBUG, "the device was closed; the control loop ends");
        end();
    }

    private void fail(Throwable failure) {
        error = failure;
        fatal = failure;
        end();
    }

    private void end() {
        ended = true;
        for (Request<?> request = requests.poll(); request != null; request = requests.poll()) {
            request.fail();
        }
    }

    /** A stop the binding itself issues on an exit path: logged, never thrown. */
    private void stopQuietly(boolean emergency) {
        try {
            if (emergency) {
                device.emergencyStop();
            } else {
                device.disarm();
            }
        } catch (HandleException closed) {
            LOG.log(System.Logger.Level.DEBUG, "already stopped by close");
        } catch (KeelException failure) {
            LOG.log(System.Logger.Level.WARNING, "the stop could not be confirmed", failure);
        }
    }

    // --------------------------------------------------------------- stopping ----

    /**
     * Disarms the device now, on the calling thread ("stop wins"), and ends the loop after the current cycle. Never
     * waits. Safe from any thread, including the step.
     *
     * @throws SafetyFaultException if the stop could not be confirmed
     */
    public void stop() {
        requestStop(false);
    }

    /**
     * Stops the device with {@code keel_emergency_stop} now, on the calling thread, and ends the loop after the
     * current cycle. The device latches FAULTED with {@link FaultCause#USER_STOP}. Never waits.
     *
     * @throws SafetyFaultException if a stop action failed
     */
    public void emergencyStop() {
        requestStop(true);
    }

    private void requestStop(boolean emergency) {
        stopRequested = true;
        try {
            if (emergency) {
                device.emergencyStop();
            } else {
                device.disarm();
            }
        } catch (HandleException closed) {
            LOG.log(System.Logger.Level.DEBUG, "already stopped by close");
        } finally {
            if (!running.get()) {
                end();
            }
        }
    }

    /**
     * Waits for the loop to end, however it runs (its own thread after {@link #start()}, or another thread's
     * {@link #run(long)} or {@link #runUntil(Predicate, long)}), then rethrows the loop's fatal error, if any. Returns
     * at once if the loop is not running.
     *
     * @throws InterruptedException if the waiting thread is interrupted
     * @throws IllegalStateException if called from the loop's own thread, which cannot wait for itself
     * @throws KeelException the loop's fatal error; an {@link Error} that escaped the step or a
     *     {@link RuntimeException} thrown by the {@code runUntil} condition is rethrown as it is
     */
    public void join() throws InterruptedException {
        requireOtherThread();
        idleLock.lockInterruptibly();
        try {
            while (running.get()) {
                idle.await();
            }
        } finally {
            idleLock.unlock();
        }
        rethrowFatal();
    }

    /**
     * Waits a bounded time for the loop to end, however it runs, then rethrows the loop's fatal error, if any.
     *
     * @param timeout the longest wait
     * @return {@code true} if the loop is not running when this returns
     * @throws InterruptedException if the waiting thread is interrupted
     * @throws IllegalStateException if called from the loop's own thread
     * @throws KeelException the loop's fatal error; an {@link Error} that escaped the step or a
     *     {@link RuntimeException} thrown by the {@code runUntil} condition is rethrown as it is
     */
    public boolean join(Duration timeout) throws InterruptedException {
        requireOtherThread();
        long remaining = timeout.toNanos();
        idleLock.lockInterruptibly();
        try {
            while (running.get()) {
                if (remaining <= 0) {
                    return false;
                }
                remaining = idle.awaitNanos(remaining);
            }
        } finally {
            idleLock.unlock();
        }
        rethrowFatal();
        return true;
    }

    /**
     * Stops the loop (disarming the device) and waits for it to end, however it runs. From the loop's own thread it
     * only requests the end and returns. The loop's fatal error is rethrown only if {@code run}, {@code runUntil} or
     * {@code join} has not already thrown it, so a failing loop in try-with-resources reports its error once and as
     * it is.
     *
     * @throws SafetyFaultException if the stop could not be confirmed
     * @throws KeelException the loop's fatal error, if no other method has thrown it; an {@link Error} that escaped
     *     the step or a {@link RuntimeException} thrown by the {@code runUntil} condition is rethrown as it is
     */
    @Override
    public void close() {
        RuntimeException stopFailure = null;
        try {
            stop();
        } catch (RuntimeException failure) {
            stopFailure = failure;
        }
        if (Thread.currentThread() != cycling) {
            idleLock.lock();
            try {
                while (running.get()) {
                    idle.awaitUninterruptibly(); // keeps the caller's interrupt status
                }
            } finally {
                idleLock.unlock();
            }
            Throwable failure = fatal;
            if (failure != null && !fatalDelivered) {
                if (stopFailure != null) {
                    failure.addSuppressed(stopFailure);
                }
                rethrowFatal();
            }
        }
        if (stopFailure != null) {
            throw stopFailure;
        }
    }

    private void requireOtherThread() {
        if (Thread.currentThread() == cycling) {
            throw new IllegalStateException("join() was called from the loop's own thread, which cannot wait for "
                    + "itself; stop() ends the loop after the current cycle");
        }
    }

    /** Rethrows the loop's fatal error as it is, if there is one, and records that a caller has received it. */
    private void rethrowFatal() {
        Throwable failure = fatal;
        if (failure == null) {
            return;
        }
        fatalDelivered = true;
        if (failure instanceof RuntimeException runtime) {
            throw runtime;
        }
        if (failure instanceof Error severe) {
            throw severe;
        }
        throw new IllegalStateException("the control loop ended with a failure", failure);
    }

    // ---------------------------------------------------------------- posting ----

    /**
     * Arms the device from the loop's thread, the control owner; equivalent to {@code submit(Device::requestArm)}.
     *
     * @return completes after {@code keel_request_arm} has returned on the loop's thread, or with its exception
     */
    public CompletableFuture<Void> requestArm() {
        return submit(target -> {
            target.requestArm();
            return null;
        });
    }

    /**
     * Clears a fault from the loop's thread, the control owner. An explicit decision, never a retry loop.
     *
     * @return completes after {@code keel_clear_fault} has returned on the loop's thread, or with its exception
     */
    public CompletableFuture<Void> clearFault() {
        return submit(target -> {
            target.clearFault();
            return null;
        });
    }

    /**
     * Runs a function on the loop's thread at the start of the next cycle, inside the step budget. Its exceptions go
     * to the future, never into the loop. Pending functions fail with {@link HandleException} when the loop ends.
     *
     * @param <T> the result type
     * @param function the function; it receives the device
     * @return the function's result
     */
    public <T> CompletableFuture<T> submit(Function<Device, T> function) {
        Request<T> request = new Request<>(Objects.requireNonNull(function, "function"), new CompletableFuture<>());
        requests.add(request);
        if (ended) {
            end();
        }
        return request.future();
    }

    /**
     * Posts a one-shot drive command for the next cycle. It applies to one cycle and is never re-sent: post again
     * within the command timeout to keep the robot moving, so the deadman still certifies the posting thread.
     *
     * @param left the left duty, -{@link Limits#DUTY_MAX} to {@link Limits#DUTY_MAX}
     * @param right the right duty
     * @throws InvalidArgumentException if a duty is out of range; a coast is posted instead, and the loop stays
     *     healthy because the error belongs to the posting thread
     */
    public void setCommand(int left, int right) {
        if (Math.abs((long) left) > Limits.DUTY_MAX || Math.abs((long) right) > Limits.DUTY_MAX) {
            intent.set(COAST);
            throw ERRORS.dutyRefused("ControlLoop.setCommand", left, right);
        }
        intent.set(new Intent(CycleCommand.DRIVE.code(), left, right));
    }

    /** Posts a one-shot brake for the next cycle. */
    public void brake() {
        intent.set(BRAKE);
    }

    /** Posts a one-shot coast for the next cycle. */
    public void coast() {
        intent.set(COAST);
    }

    // ---------------------------------------------------------------- reading ----

    /**
     * Returns the latest snapshot; readable from any thread.
     *
     * @return the snapshot of the latest cycle, or of the device when the loop was built
     */
    public Snapshot snapshot() {
        return snapshot;
    }

    /**
     * Returns the fault record the loop read on its latest entry into FAULTED.
     *
     * @return the record, or empty if the loop has not seen a fault
     */
    public Optional<Fault> fault() {
        return Optional.ofNullable(fault);
    }

    /**
     * Returns the latest exception of a step or of the loop itself.
     *
     * @return the exception, or empty if none occurred
     */
    public Optional<Throwable> error() {
        return Optional.ofNullable(error);
    }

    /**
     * Returns whether a thread is running the loop now.
     *
     * @return {@code true} while cycles run
     */
    public boolean isRunning() {
        return running.get();
    }

    /**
     * Returns whether the loop has ended for good.
     *
     * @return {@code true} after a stop, a fatal error or the device's close
     */
    public boolean isEnded() {
        return ended;
    }

    /**
     * Returns the loop's counters.
     *
     * @return a snapshot of the counters
     */
    public LoopStats stats() {
        Map<Status, Long> counts = new EnumMap<>(Status.class);
        rejected.forEach((code, count) -> counts.merge(Status.of(code), count.get(), Long::sum));
        return new LoopStats(cycles.get(), overruns.get(), maxGapUs.get(), maxStepUs.get(), counts, stepErrors.get(),
                faults.get());
    }

    // ----------------------------------------------------------------- helpers ----

    private record Intent(int command, int left, int right) {
    }

    private record Request<T>(Function<Device, T> function, CompletableFuture<T> future) {
        void run(Device target) {
            try {
                future.complete(function.apply(target));
            } catch (Throwable failure) {
                future.completeExceptionally(failure);
            }
        }

        void fail() {
            future.completeExceptionally(ERRORS.loopEnded("keel_cycle"));
        }
    }

    /** Lets one message through per interval; used only on the loop's thread. */
    private static final class RateLimiter {
        private static final long INTERVAL_NANOS = TimeUnit.SECONDS.toNanos(5);
        private long next;
        private boolean used;

        boolean allow() {
            long now = System.nanoTime();
            if (used && now - next < 0) {
                return false;
            }
            used = true;
            next = now + INTERVAL_NANOS;
            return true;
        }
    }
}
