// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.loop;

import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.errors.InvalidArgumentException;
import io.github.bisocmorg.keel.internal.AccessRegistry;
import io.github.bisocmorg.keel.internal.ErrorAccess;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.Snapshot;

/**
 * One cycle of a {@link ControlLoop}, handed to its step. The loop reuses one object, which is valid only inside the
 * step that receives it.
 *
 * <p>The step decides this cycle's motor command with {@link #drive(int, int)}, {@link #brake()} or {@link #coast()};
 * without one, the stored command keeps ageing toward its timeout and the wheels coast when it expires. Send the
 * command again every cycle while it should stay in force. The step's command wins over a command posted with
 * {@link ControlLoop#setCommand(int, int)} for the same cycle.
 */
public final class Cycle {
    private static final ErrorAccess ERRORS = AccessRegistry.errors();

    private final Device device;
    private Snapshot snapshot;
    private long cycleId;
    private long nowUs;
    private int command;
    private int left;
    private int right;
    private boolean ioOk;
    private boolean dutyRefused;

    Cycle(Device device) {
        this.device = device;
    }

    void reset(Snapshot latest, long id, long now) {
        snapshot = latest;
        cycleId = id;
        nowUs = now;
        command = CycleCommand.NONE.code();
        left = 0;
        right = 0;
        ioOk = true;
        dutyRefused = false;
    }

    /**
     * Returns the snapshot of the previous {@code keel_cycle} (before the first cycle: {@link Device#snapshot()}).
     *
     * @return the snapshot
     */
    public Snapshot snapshot() {
        return snapshot;
    }

    /**
     * Returns the id this cycle's {@code keel_cycle} will carry.
     *
     * @return the cycle id; it strictly increases over the device's lifetime
     */
    public long cycleId() {
        return cycleId;
    }

    /**
     * Returns the device clock at the start of the step.
     *
     * @return the device time in microseconds
     */
    public long nowUs() {
        return nowUs;
    }

    /**
     * Returns the device. Calls from the step run on the loop's thread, the control owner.
     *
     * @return the device
     */
    public Device device() {
        return device;
    }

    /**
     * Drives both motors this cycle, in signed permille.
     *
     * <p>A duty outside -{@link Limits#DUTY_MAX}..{@link Limits#DUTY_MAX} fails this cycle: the loop sends a coast,
     * reports the step as unhealthy (without {@link Health#NO_EXCEPTION}, even if the step catches this exception),
     * and the tick fail-stops with {@link FaultCause#HEALTH_FLAGS}, a latched Category-0 stop that
     * {@link ControlLoop#clearFault()} recovers. libkeel would otherwise refuse the duty in a way that ends the loop
     * for the device's life.
     *
     * @param leftDuty the left duty
     * @param rightDuty the right duty
     * @throws InvalidArgumentException if a duty is out of range
     */
    public void drive(int leftDuty, int rightDuty) {
        if (Math.abs((long) leftDuty) > Limits.DUTY_MAX || Math.abs((long) rightDuty) > Limits.DUTY_MAX) {
            dutyRefused = true;
            set(CycleCommand.DRIVE, 0, 0);
            throw ERRORS.dutyRefused("Cycle.drive", leftDuty, rightDuty);
        }
        set(CycleCommand.DRIVE, leftDuty, rightDuty);
    }

    /** Brakes both motors this cycle (needs {@link Capability#BRAKE}). */
    public void brake() {
        set(CycleCommand.BRAKE, 0, 0);
    }

    /** Coasts both motors now: an explicit {@code DRIVE (0, 0)} this cycle. */
    public void coast() {
        set(CycleCommand.DRIVE, 0, 0);
    }

    /** Reports that the application's own I/O failed this cycle: the tick then fail-stops without IO_OK. */
    public void reportIoFailure() {
        ioOk = false;
    }

    private void set(CycleCommand value, int leftDuty, int rightDuty) {
        command = value.code();
        left = leftDuty;
        right = rightDuty;
    }

    int command() {
        return command;
    }

    int left() {
        return left;
    }

    int right() {
        return right;
    }

    boolean ioOk() {
        return ioOk;
    }

    boolean dutyRefused() {
        return dutyRefused;
    }
}
