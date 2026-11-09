// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import static org.junit.jupiter.api.Assertions.fail;

import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.loop.CycleCommand;
import io.github.bisocmorg.keel.loop.CycleInput;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.State;
import java.util.function.Predicate;

/**
 * Drives a simulator device by hand on the test thread, which becomes the control owner: one {@code keel_cycle} and
 * one period of device time per call.
 */
public final class CycleDriver {
    private final Device device;
    private long next;

    /** Starts driving a device from its current time. */
    public CycleDriver(Device device) {
        this.device = device;
        this.next = device.nowUs();
    }

    /** One cycle with a command, then a sleep to the next period. */
    public Snapshot cycle(CycleCommand command, int left, int right) {
        long id = device.snapshot().lastCycleId() + 1;
        next += SimulatorDevices.PERIOD_US;
        try {
            return device.cycle(new CycleInput(Health.ALL.mask(), id, command.code(), left, right, 0));
        } finally {
            device.sleepUntil(Math.max(next, device.nowUs()));
        }
    }

    /** Cycles without a command. */
    public Snapshot ticks(int count) {
        Snapshot last = null;
        for (int i = 0; i < count; i++) {
            last = cycle(CycleCommand.NONE, 0, 0);
        }
        return last;
    }

    /** Cycles with a drive command. */
    public Snapshot drive(int count, int left, int right) {
        Snapshot last = null;
        for (int i = 0; i < count; i++) {
            last = cycle(CycleCommand.DRIVE, left, right);
        }
        return last;
    }

    /** Cycles without a command until the condition holds; fails the test after {@code maxCycles}. */
    public Snapshot until(Predicate<Snapshot> done, int maxCycles) {
        for (int i = 0; i < maxCycles; i++) {
            Snapshot snapshot = cycle(CycleCommand.NONE, 0, 0);
            if (done.test(snapshot)) {
                return snapshot;
            }
        }
        return fail("the condition did not hold within " + maxCycles + " cycles");
    }

    /** Arms on the test thread (the owner) and cycles until ARMED. */
    public Snapshot arm() {
        device.requestArm();
        return until(snapshot -> snapshot.state() == State.ARMED, 200);
    }
}
