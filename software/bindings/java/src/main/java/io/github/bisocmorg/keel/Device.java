// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import io.github.bisocmorg.keel.errors.ConflictException;
import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.errors.InvalidArgumentException;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.SafetyFaultException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.errors.UnsupportedException;
import io.github.bisocmorg.keel.internal.AccessRegistry;
import io.github.bisocmorg.keel.internal.DeviceAccess;
import io.github.bisocmorg.keel.internal.ErrorAccess;
import io.github.bisocmorg.keel.internal.Layouts;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import io.github.bisocmorg.keel.internal.StructCodec;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.CycleInput;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.peripherals.ButtonState;
import io.github.bisocmorg.keel.peripherals.Led;
import io.github.bisocmorg.keel.peripherals.RangeMeasurement;
import io.github.bisocmorg.keel.peripherals.RangeStatus;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.sim.SimState;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.SafetyInput;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.State;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.ref.Reference;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * An open Keel device: the robot's board, or the simulator.
 *
 * <p>Every method is one libkeel call and maps a non-OK status to a {@link KeelException} by class. Arguments reach
 * libkeel unchanged: libkeel is the only safety logic, so every refusal, coast and fault is its documented behaviour.
 *
 * <p><b>Threads.</b> Every method is safe from any thread, subject to libkeel's rule "stop from anywhere, go only
 * from the loop". The control owner is the OS thread of the first successful {@link #tick(long, int)} or
 * {@link #cycle(CycleInput)} after open or {@link #clearFault()}; until then every thread is the owner. Go calls
 * ({@link #requestArm()}, {@link #clearFault()}, non-zero {@link #setMotors(int, int)}, {@link #brake()},
 * {@link #servoStart(int, int)}, {@link #servoSetPulse(int)}) from another thread throw {@link ConflictException}.
 * Stop calls ({@link #disarm()}, {@link #emergencyStop()}, {@link #coast()}, {@link #servoEnd()}, {@link #close()})
 * are accepted from every thread in every state and take no binding lock. Run the loop with {@link ControlLoop}.
 *
 * <p><b>Virtual threads.</b> {@link #tick(long, int)}, {@link #cycle(CycleInput)} and {@link #sleepUntil(long)}
 * throw {@link WrongThreadException} on a virtual thread, before any call: its carrier OS thread can change between
 * calls, which libkeel would see as a tick from a foreign thread. Every other call is allowed and pins its carrier
 * only for its documented time: open and close about 100 ms, {@link #rangeMeasure()} at most 35 ms, an LED call at
 * most 20 ms per I2C transaction, everything else under 2 ms.
 *
 * <p><b>Lifetime.</b> Close with try-with-resources. {@link #close()} never throws and returns only after
 * {@code keel_close} has returned. A device that is dropped unclosed is closed by a cleaner, with a warning, and the
 * binding's shutdown hook stops and closes every open device when the JVM exits. A call on a closed or closing device
 * throws {@link HandleException} without calling libkeel.
 */
public final class Device implements AutoCloseable {
    private static final System.Logger LOG = System.getLogger("io.github.bisocmorg.keel");
    private static final ErrorAccess ERRORS = AccessRegistry.errors();

    static {
        AccessRegistry.registerDevices(new Access());
    }

    private final NativeLibrary library;
    private final CloseState state;
    private final OpenReport report;
    private final AtomicBoolean loopAttached = new AtomicBoolean();

    private Device(NativeLibrary library, MemorySegment token, OpenReport report) {
        this.library = library;
        this.report = report;
        this.state = new CloseState(token, library);
        state.register(this);
    }

    /**
     * {@code keel_open} with raw option values; {@link Keel#open(OpenOptions)} is the public form.
     *
     * @param library the loaded library
     * @param backend {@code keel_open_options.backend}
     * @param flags {@code keel_open_options.flags}
     * @param commandTimeoutMs {@code keel_open_options.command_timeout_ms}
     * @param gpioChipPath the encoded GPIO chip path
     * @param i2cPath the encoded I2C adapter path
     * @return the device
     */
    static Device open(NativeLibrary library, int backend, int flags, int commandTimeoutMs, byte[] gpioChipPath,
            byte[] i2cPath) {
        OpenReport report;
        MemorySegment token;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment options = StructCodec.allocate(arena, Layouts.OPEN_OPTIONS);
            StructCodec.writeOpenOptions(options, backend, flags, commandTimeoutMs, gpioChipPath, i2cPath);
            MemorySegment out = arena.allocate(ADDRESS);
            MemorySegment written = StructCodec.allocate(arena, Layouts.OPEN_REPORT);
            int status = library.open(options, out, written);
            report = StructCodec.openReport(written);
            if (status != 0) {
                throw ERRORS.setup(status, report);
            }
            token = out.get(ADDRESS, 0);
        }
        Device device = new Device(library, token, report);
        if (report.warningsValue() != 0) {
            LOG.log(System.Logger.Level.WARNING, "keel_open: the device opened degraded ({0}): {1}",
                    report.warnings(), report.hint());
        }
        return device;
    }

    // --------------------------------------------------------------- plumbing ----

    /** The handle for a call, refused once the device is closing. */
    private MemorySegment token(String function) {
        if (state.closing.get()) {
            throw ERRORS.closed(function);
        }
        return state.token;
    }

    private void check(int status, String function) {
        if (status != 0) {
            throw ERRORS.of(status, function, this::faultOrNull, null);
        }
    }

    private static void requirePlatformThread(String method) {
        if (Thread.currentThread().isVirtual()) {
            throw new WrongThreadException(method + " must run on a platform thread: a virtual thread's carrier OS "
                    + "thread can change between calls, which libkeel treats as a tick from another thread");
        }
    }

    private static boolean written(int status) {
        return status != Status.E_INVALID_ARGUMENT.code() && status != Status.E_INVALID_HANDLE.code()
                && status != Status.E_NOT_OWNER.code();
    }

    /**
     * The fault record for a FAULT exception.
     *
     * @return the record, or {@code null} when the device is closing or the read fails
     */
    Fault faultOrNull() {
        if (state.closing.get()) {
            return null;
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = StructCodec.allocate(arena, Layouts.FAULT);
            return library.getFault(state.token, out) == 0 ? StructCodec.fault(out) : null;
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * Claims the device for a control loop: one loop serves a device for the device's whole life.
     *
     * @return {@code false} if a loop already serves it
     */
    boolean attachLoop() {
        return loopAttached.compareAndSet(false, true);
    }

    /**
     * {@code keel_cycle} for the loop: the raw status, and no call once the device is closing.
     *
     * @param in the loop's {@code keel_cycle_input}
     * @param out the loop's {@code keel_snapshot}
     * @return the status; {@link Status#E_INVALID_HANDLE} when the device is closing
     */
    int cycleRaw(MemorySegment in, MemorySegment out) {
        try {
            return state.closing.get() ? Status.E_INVALID_HANDLE.code() : library.cycle(state.token, in, out);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_sleep_until} for the loop.
     *
     * @param deadlineUs the absolute device time
     * @return the status; {@link Status#E_INVALID_HANDLE} when the device is closing
     */
    int sleepUntilRaw(long deadlineUs) {
        try {
            return state.closing.get() ? Status.E_INVALID_HANDLE.code() : library.sleepUntil(state.token, deadlineUs);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_now_us} for the loop.
     *
     * @param out a {@code uint64_t} the loop owns
     * @return the device time, or -1 when the device is closing or closed
     */
    long nowUsRaw(MemorySegment out) {
        try {
            if (state.closing.get() || library.nowUs(state.token, out) != 0) {
                return -1;
            }
            return out.get(JAVA_LONG, 0);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * Returns whether the device is closing or closed.
     *
     * @return {@code true} once {@link #close()} has started
     */
    boolean closing() {
        return state.closing.get();
    }

    /**
     * Returns the library this device belongs to.
     *
     * @return the library
     */
    NativeLibrary library() {
        return library;
    }

    /**
     * Returns the close state, which outlives the device.
     *
     * @return the state
     */
    CloseState closeState() {
        return state;
    }

    // --------------------------------------------------------------- lifetime ----

    /**
     * Returns the report {@code keel_open} wrote for this device.
     *
     * @return the report: backend, capabilities, warnings and their hint
     */
    public OpenReport report() {
        return report;
    }

    /**
     * Returns whether {@link #close()} has started.
     *
     * @return {@code true} once the device is closing or closed
     */
    public boolean isClosed() {
        return state.closing.get();
    }

    /**
     * Returns the status {@code keel_close} returned. It is diagnostic only: a non-OK status means the final stop
     * could not be confirmed, and the safety MCU's heartbeat timeout still stops the motors.
     *
     * @return the status, or empty before the close has finished
     */
    public Optional<Status> closeStatus() {
        return state.completed() ? Optional.of(Status.of(state.closeStatus())) : Optional.empty();
    }

    /**
     * Returns the final fault record {@code keel_close} wrote, including the {@code cleanup_status} of its stop.
     *
     * @return the record, or empty before the close has finished
     */
    public Optional<Fault> finalFault() {
        return Optional.ofNullable(state.finalFault());
    }

    /**
     * {@code keel_close}: a Category-0 stop, LEDs off, every resource released.
     *
     * <p>Idempotent and safe from any thread, concurrently with other calls ("stop wins"): callers blocked in
     * {@link #sleepUntil(long)} or {@link #rangeMeasure()} return with {@link HandleException}, and calls in flight
     * finish first. Returns only after {@code keel_close} has returned, so the lines are free for an immediate reopen.
     * Never throws: a non-OK status is logged at WARNING and kept in {@link #closeStatus()}.
     */
    @Override
    public void close() {
        state.close();
    }

    /**
     * {@code keel_get_capabilities}: the features of this device.
     *
     * @return the capabilities this binding knows
     */
    public Set<Capability> capabilities() {
        return Capability.setOf(capabilitiesMask());
    }

    /**
     * {@code keel_get_capabilities} as the raw mask, which keeps bits this binding does not know.
     *
     * @return the {@code keel_capability} mask
     */
    public int capabilitiesMask() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(JAVA_INT);
            check(library.getCapabilities(token("keel_get_capabilities"), out), "keel_get_capabilities");
            return out.get(JAVA_INT, 0);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    // ------------------------------------------------------------------ clock ----

    /**
     * {@code keel_now_us}: the device clock, monotonic microseconds; the virtual clock of a virtual-clock simulator.
     *
     * @return the device time
     */
    public long nowUs() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(JAVA_LONG);
            check(library.nowUs(token("keel_now_us"), out), "keel_now_us");
            return out.get(JAVA_LONG, 0);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_sleep_until}: sleeps until an absolute device time, at most {@link Limits#SLEEP_AHEAD_MAX_MS}
     * ahead. On a virtual-clock simulator it advances the clock instead. Wakes early with {@link HandleException} when
     * the device is closed.
     *
     * @param deadlineUs the absolute device time
     * @throws WrongThreadException on a virtual thread, whose carrier it would pin for up to 10 s
     */
    public void sleepUntil(long deadlineUs) {
        requirePlatformThread("Device.sleepUntil");
        try {
            check(library.sleepUntil(token("keel_sleep_until"), deadlineUs), "keel_sleep_until");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    // ----------------------------------------------------------------- safety ----

    /**
     * {@code keel_tick}: one health-loop step. Call it every 10 ms from the loop that really performs the
     * application's control work; {@link ControlLoop} does that. {@code health} has no default: asserting it is the
     * caller's decision, and anything but {@link Health#ALL} fail-stops on purpose.
     *
     * @param cycleId a strictly increasing loop counter
     * @param health the health mask; {@code Health.ALL.mask()} when this cycle's work succeeded
     * @throws WrongThreadException on a virtual thread
     */
    public void tick(long cycleId, int health) {
        requirePlatformThread("Device.tick");
        try {
            check(library.tick(token("keel_tick"), cycleId, health), "keel_tick");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_cycle}: a tick, a motor command and the snapshot in one call. A failure carries the snapshot the
     * call wrote ({@link KeelException#snapshot()}).
     *
     * @param input the cycle input
     * @return the snapshot after the cycle
     * @throws WrongThreadException on a virtual thread
     */
    public Snapshot cycle(CycleInput input) {
        Objects.requireNonNull(input, "input");
        requirePlatformThread("Device.cycle");
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment in = StructCodec.allocate(arena, Layouts.CYCLE_INPUT);
            StructCodec.writeCycleInput(in, input);
            MemorySegment out = StructCodec.allocate(arena, Layouts.SNAPSHOT);
            int status = library.cycle(token("keel_cycle"), in, out);
            if (status != 0) {
                throw ERRORS.of(status, "keel_cycle", this::faultOrNull,
                        written(status) ? StructCodec.snapshot(out) : null);
            }
            return StructCodec.snapshot(out);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_request_arm}: requests motor permission, an explicit decision of the operator or application.
     * Returns at once; arming completes over about 0.6 to 0.85 s of continuing ticks. Never call it automatically
     * after a fault.
     */
    public void requestArm() {
        try {
            check(library.requestArm(token("keel_request_arm")), "keel_request_arm");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_disarm}: a Category-0 stop that does not latch. Motor enable falls before any lock is taken; the
     * state becomes DISARMED (FAULTED stays FAULTED) and the heartbeat continues. Accepted from every thread.
     *
     * @throws SafetyFaultException if the stop could not be confirmed ({@link Status#E_IO})
     */
    public void disarm() {
        try {
            check(library.disarm(token("keel_disarm")), "keel_disarm");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_emergency_stop}: the latching Category-0 stop for stop buttons and Ctrl-C. Stops the heartbeat and
     * latches FAULTED with {@link FaultCause#USER_STOP}; returns normally when that succeeded. Accepted from every
     * thread in every state.
     *
     * @throws SafetyFaultException if a stop action failed
     */
    public void emergencyStop() {
        try {
            check(library.emergencyStop(token("keel_emergency_stop")), "keel_emergency_stop");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_clear_fault}: leaves FAULTED without arming; an explicit decision, never a retry loop. Must come
     * from the control owner; with a loop running, use {@link ControlLoop#clearFault()}.
     */
    public void clearFault() {
        try {
            check(library.clearFault(token("keel_clear_fault")), "keel_clear_fault");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_get_state}: the coarse state (cached; no I/O).
     *
     * @return the state
     */
    public State state() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(JAVA_INT);
            check(library.getState(token("keel_get_state"), out), "keel_get_state");
            return State.of(out.get(JAVA_INT, 0));
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_get_snapshot}: the cached state and diagnostics. Peeks; consumes nothing.
     *
     * @return the snapshot
     */
    public Snapshot snapshot() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = StructCodec.allocate(arena, Layouts.SNAPSHOT);
            check(library.getSnapshot(token("keel_get_snapshot"), out), "keel_get_snapshot");
            return StructCodec.snapshot(out);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_get_fault}: the first fault since the last clear (cached; no I/O).
     *
     * @return the record; status {@link Status#OK} and cause {@link FaultCause#NONE} when none was ever recorded
     */
    public Fault fault() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = StructCodec.allocate(arena, Layouts.FAULT);
            check(library.getFault(token("keel_get_fault"), out), "keel_get_fault");
            return StructCodec.fault(out);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_read_safety_inputs}: a fresh read of READY and ARMED (diagnostic). Each call is one read.
     *
     * @return the inputs this binding knows
     */
    public Set<SafetyInput> readSafetyInputs() {
        return SafetyInput.setOf(readSafetyInputsMask());
    }

    /**
     * {@code keel_read_safety_inputs} as the raw mask, which keeps bits this binding does not know. Each call is one
     * read.
     *
     * @return the {@code keel_input} mask
     */
    public int readSafetyInputsMask() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(JAVA_INT);
            check(library.readSafetyInputs(token("keel_read_safety_inputs"), out), "keel_read_safety_inputs");
            return out.get(JAVA_INT, 0);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    // ----------------------------------------------------------------- motion ----

    /**
     * {@code keel_set_motors}: both motor duties in signed permille, passed to libkeel unchanged. A duty outside
     * -1000..1000 is refused ({@link InvalidArgumentException}) and, while ARMED, coasts both motors. (0, 0) is a stop
     * accepted from any thread; a non-zero command needs ARMED and the control owner.
     *
     * @param left the left duty
     * @param right the right duty
     */
    public void setMotors(int left, int right) {
        try {
            check(library.setMotors(token("keel_set_motors"), left, right), "keel_set_motors");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /** {@code keel_set_motors(0, 0)}: coasts both motors; a stop, accepted from any thread in every state. */
    public void coast() {
        setMotors(0, 0);
    }

    /** {@code keel_brake}: brakes both motors while staying armed, for the command timeout. */
    public void brake() {
        try {
            check(library.brake(token("keel_brake")), "keel_brake");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    // ------------------------------------------------------------------ servo ----

    /**
     * {@code keel_servo_start}: starts a servo burst. Needs {@link Capability#SERVO}; the board throws
     * {@link UnsupportedException} (J5 is not fitted on R3), the simulator implements the full contract.
     *
     * @param pulseUs the pulse width, {@link Limits#SERVO_PULSE_MIN_US} to {@link Limits#SERVO_PULSE_MAX_US}
     * @param leaseMs the burst length, {@link Limits#SERVO_LEASE_MIN_MS} to {@link Limits#SERVO_LEASE_MAX_MS}
     */
    public void servoStart(int pulseUs, int leaseMs) {
        try {
            check(library.servoStart(token("keel_servo_start"), pulseUs, leaseMs), "keel_servo_start");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_servo_set_pulse}: changes the pulse width of the active burst; never extends it.
     *
     * @param pulseUs the pulse width
     */
    public void servoSetPulse(int pulseUs) {
        try {
            check(library.servoSetPulse(token("keel_servo_set_pulse"), pulseUs), "keel_servo_set_pulse");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /** {@code keel_servo_end}: ends the active burst early; a no-op without one. Accepted from any thread. */
    public void servoEnd() {
        try {
            check(library.servoEnd(token("keel_servo_end")), "keel_servo_end");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    // ---------------------------------------------------------------- ranging ----

    /**
     * {@code keel_range_start}: triggers a measurement without blocking (about 15 us). The form for loops; collect
     * the result with {@link #rangePoll()}.
     */
    public void rangeStart() {
        try {
            check(library.rangeStart(token("keel_range_start")), "keel_range_start");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_range_poll}: the latest measurement, without blocking.
     *
     * @return the result; {@link RangeStatus#PENDING} until it completes
     */
    public RangeMeasurement rangePoll() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = StructCodec.allocate(arena, Layouts.RANGE);
            check(library.rangePoll(token("keel_range_poll"), out), "keel_range_poll");
            return StructCodec.range(out);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_range_measure}: triggers and waits for one measurement, at most
     * {@link Limits#RANGE_MEASURE_MAX_MS}. Refused ({@link ConflictException}) from the control owner while ARMING,
     * ARMED or SERVO. Allowed on a virtual thread, whose carrier it pins for at most 35 ms.
     *
     * @return the completed result; only {@link RangeStatus#VALID} carries a distance
     */
    public RangeMeasurement rangeMeasure() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = StructCodec.allocate(arena, Layouts.RANGE);
            check(library.rangeMeasure(token("keel_range_measure"), out), "keel_range_measure");
            return StructCodec.range(out);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    // ------------------------------------------------------------------- LEDs ----

    /**
     * {@code keel_set_rgb}: sets one RGB LED. Works in every state, including FAULTED.
     *
     * @param index the LED, 0 to 5 ({@link Led})
     * @param rgb the colour 0x00RRGGBB; bits 24 to 31 are ignored, so {@code java.awt.Color.getRGB()} passes directly
     */
    public void setRgb(int index, int rgb) {
        try {
            check(library.setRgb(token("keel_set_rgb"), index, rgb), "keel_set_rgb");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_set_rgb_all}: sets all six RGB LEDs to one colour in one I2C frame.
     *
     * @param rgb the colour 0x00RRGGBB
     */
    public void setRgbAll(int rgb) {
        try {
            check(library.setRgbAll(token("keel_set_rgb_all"), rgb), "keel_set_rgb_all");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_set_rgb_frame}: sets the first {@code colors.length} LEDs in one atomic I2C frame, RGB1 first.
     * The count is passed through; libkeel accepts 1 to 6.
     *
     * @param colors the colours 0x00RRGGBB
     */
    public void setRgbFrame(int... colors) {
        Objects.requireNonNull(colors, "colors");
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment frame = arena.allocate(JAVA_INT, Math.max(1, colors.length));
            MemorySegment.copy(colors, 0, frame, JAVA_INT, 0, colors.length);
            check(library.setRgbFrame(token("keel_set_rgb_frame"), frame, colors.length), "keel_set_rgb_frame");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_get_rgb}: the colour last written successfully to one LED (cached; no I/O).
     *
     * @param index the LED, 0 to 5
     * @return the colour 0x00RRGGBB
     */
    public int rgb(int index) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(JAVA_INT);
            check(library.getRgb(token("keel_get_rgb"), index, out), "keel_get_rgb");
            return out.get(JAVA_INT, 0);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_set_button_leds}: switches button indicator LEDs. A failure is FAULT class: these lines share the
     * control request with motor enable and the heartbeat.
     *
     * @param mask the LEDs to change, a {@link Button} mask such as {@code Button.maskOf(Button.A)}
     * @param values the new levels of the LEDs in {@code mask} (1 = lit)
     */
    public void setButtonLeds(int mask, int values) {
        try {
            check(library.setButtonLeds(token("keel_set_button_leds"), mask, values), "keel_set_button_leds");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    // ---------------------------------------------------------------- buttons ----

    /**
     * {@code keel_take_buttons}: reads the buttons and takes the accumulated edges. Works in every state.
     *
     * @return the held buttons and the edges since the previous take
     */
    public ButtonState takeButtons() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = StructCodec.allocate(arena, Layouts.BUTTONS);
            check(library.takeButtons(token("keel_take_buttons"), out), "keel_take_buttons");
            return StructCodec.buttons(out);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    // -------------------------------------------------------------- simulator ----

    /**
     * {@code keel_sim_set}: applies a simulator stimulus; {@link UnsupportedException} on hardware.
     *
     * @param item the stimulus
     * @param value its value (see {@link SimItem})
     * @throws IllegalArgumentException for {@link SimItem#UNRECOGNIZED}, which has no C value
     */
    public void simSet(SimItem item, int value) {
        Objects.requireNonNull(item, "item");
        if (item == SimItem.UNRECOGNIZED) {
            throw new IllegalArgumentException("SimItem.UNRECOGNIZED is not a simulator item");
        }
        try {
            check(library.simSet(token("keel_sim_set"), item.code(), value), "keel_sim_set");
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * {@code keel_sim_get_state}: the simulator's ground truth; {@link UnsupportedException} on hardware.
     *
     * @return the state
     */
    public SimState simState() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = StructCodec.allocate(arena, Layouts.SIM_STATE);
            check(library.simGetState(token("keel_sim_get_state"), out), "keel_sim_get_state");
            return StructCodec.simState(out);
        } finally {
            Reference.reachabilityFence(this);
        }
    }

    /**
     * Describes the device for logs.
     *
     * @return for example {@code "Device[SIMULATOR, open]"}
     */
    @Override
    public String toString() {
        return "Device[" + report.backend() + ", " + (isClosed() ? "closed" : "open") + "]";
    }

    /** The control loop's access to the package-private members above, through {@link AccessRegistry#devices()}. */
    private static final class Access implements DeviceAccess {
        @Override
        public boolean attachLoop(Device device) {
            return device.attachLoop();
        }

        @Override
        public int cycleRaw(Device device, MemorySegment in, MemorySegment out) {
            return device.cycleRaw(in, out);
        }

        @Override
        public int sleepUntilRaw(Device device, long deadlineUs) {
            return device.sleepUntilRaw(deadlineUs);
        }

        @Override
        public long nowUsRaw(Device device, MemorySegment out) {
            return device.nowUsRaw(out);
        }

        @Override
        public boolean closing(Device device) {
            return device.closing();
        }

        @Override
        public NativeLibrary library(Device device) {
            return device.library();
        }

        @Override
        public Fault faultOrNull(Device device) {
            return device.faultOrNull();
        }
    }
}
