// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import static org.junit.jupiter.api.Assertions.*;

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.State;
import io.github.bisocmorg.keel.support.CallRecorder;
import io.github.bisocmorg.keel.support.StatusOverride;
import io.github.bisocmorg.keel.errors.Status;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import org.junit.jupiter.api.Test;

/** Managed sessions through the guarded native real-time simulator, never physical hardware. */
class RobotSessionTest {
    private static RobotSession open() { return RobotSession.open(OpenOptions.builder().backend(Backend.SIMULATOR).build()); }
    private static void await(BooleanSupplier ready) throws InterruptedException {
        long end = System.nanoTime() + 3_000_000_000L;
        while (!ready.getAsBoolean() && System.nanoTime() < end) Thread.sleep(5);
        assertTrue(ready.getAsBoolean(), "condition did not become true");
    }

    @Test void peripheralsAndStatusNeedNoApplicationController() throws Exception {
        try (RobotSession robot = open()) {
            assertTrue(robot.capabilities().contains(Capability.SIMULATED));
            assertNotNull(robot.report());
            await(robot::running);
            robot.setRgb(0, 0x120000);
            assertEquals(0x120000, robot.rgb()[0]);
            robot.setRgbAll(0x001200);
            assertEquals(0x001200, robot.rgb()[5]);
            robot.setRgbFrame(1, 2, 3, 4, 5, 6);
            assertArrayEquals(new int[]{1, 2, 3, 4, 5, 6}, robot.rgb());
            robot.setButtonLeds(15, 5);
            await(() -> robot.snapshot().buttonLedsValue() == 5);
            robot.simSet(SimItem.RANGE_MM, 500);
            assertEquals(500, robot.measureDistance().distanceMm());
            assertEquals(500, robot.measureDistance().distanceMm());
            assertTrue(robot.stats().cycles() > 0);
            assertEquals(State.DISARMED, robot.snapshot().state());
        }
    }

    @Test void driveLeaseExpiresAndRequiresFreshExplicitArming() throws Exception {
        try (RobotSession robot = open()) {
            robot.arm();
            robot.refreshDrive(20, -10, 100);
            await(() -> robot.snapshot().outputLeft() == 200);
            await(() -> robot.snapshot().state() == State.DISARMED);
            assertEquals(0, robot.snapshot().outputLeft());
            assertThrows(TaskException.class, () -> robot.refreshDrive(20, 20, 100));
        }
    }

    @Test void explicitStopRejectsRefreshEvenWhenCachedSnapshotIsStillArmed() throws Exception {
        try (RobotSession robot = open()) {
            robot.arm();
            robot.refreshDrive(20, 20, 1000);
            robot.disarm();
            assertThrows(TaskException.class, () -> robot.refreshDrive(20, 20, 100));
            robot.arm();
            Thread.sleep(30);
            assertEquals(0, robot.snapshot().outputLeft());
            robot.coast();
        }
    }

    @Test void emergencyStopAndRecoveryAreExplicit() throws Exception {
        try (RobotSession robot = open()) {
            robot.arm();
            robot.emergencyStop();
            await(() -> robot.snapshot().state() == State.FAULTED);
            assertEquals(FaultCause.USER_STOP, robot.fault().cause());
            assertThrows(TaskException.class, () -> robot.refreshDrive(10, 10, 100));
            robot.clearFault();
            await(() -> robot.snapshot().state() == State.DISARMED);
            assertEquals(0, robot.snapshot().outputLeft());
        }
    }

    @Test void queuedArmingCannotUndoConcurrentStop() throws Exception {
        try (RobotSession robot = open()) {
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            var request = robot.requestArm(() -> {
                entered.countDown();
                try { assertTrue(release.await(2, TimeUnit.SECONDS)); }
                catch (InterruptedException interrupted) { throw new RuntimeException(interrupted); }
            });
            assertTrue(entered.await(2, TimeUnit.SECONDS));
            robot.disarm();
            release.countDown();
            assertThrows(Exception.class, () -> request.get(2, TimeUnit.SECONDS));
            Thread.sleep(30);
            assertEquals(State.DISARMED, robot.snapshot().state());
        }
    }

    @Test void expiredLeaseCannotBeRefreshedAheadOfDelayedControlCallback() throws Exception {
        try (RobotSession robot = open()) {
            robot.arm();
            robot.refreshDrive(10, 10, 20);
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            var blocked = robot.setRgbAsync(0, 1, () -> {
                entered.countDown();
                try { assertTrue(release.await(2, TimeUnit.SECONDS)); }
                catch (InterruptedException interrupted) { throw new RuntimeException(interrupted); }
            });
            assertTrue(entered.await(2, TimeUnit.SECONDS));
            Thread.sleep(25);
            try {
                assertThrows(TaskException.class, () -> robot.refreshDrive(10, 10, 100));
            } finally { release.countDown(); }
            assertThrows(Exception.class, () -> blocked.get(2, TimeUnit.SECONDS));
        }
    }

    @Test void invalidDriveValuesAreRefusedAndClosedConnectionsStayClosed() throws Exception {
        RobotSession robot = open();
        assertThrows(IllegalArgumentException.class, () -> robot.refreshDrive(0, 0, 0));
        assertThrows(IllegalArgumentException.class, () -> robot.refreshDrive(Double.NaN, 0, 100));
        assertThrows(IllegalArgumentException.class, () -> robot.refreshDrive(101, 0, 100));
        assertThrows(TaskException.class, () -> robot.refreshDriveUntil(1, 1, System.nanoTime() - 1));
        assertThrows(IllegalArgumentException.class, () -> robot.refreshDriveUntil(1, 1, System.nanoTime() + 40_000_000_000L));
        var rejected = robot.requestArm(() -> { throw new IllegalStateException("permission revoked"); });
        assertThrows(Exception.class, () -> rejected.get(2, TimeUnit.SECONDS));
        robot.close();
        robot.close();
        assertFalse(robot.running());
        assertThrows(TaskException.class, robot::snapshot);
    }

    @Test void queuedRequestsCannotAcquireFreshLifetimeAfterTheyExpire() throws Exception {
        TaskOptions defaults = TaskOptions.defaults().withOpenOptions(OpenOptions.builder().backend(Backend.SIMULATOR).build());
        TaskOptions shortWait = new TaskOptions(defaults.openOptions(), defaults.cameraOptions(), 30_000, 20, 300_000, 1000);
        try (RobotSession robot = RobotSession.open(shortWait)) {
            var request = robot.requestArm(() -> {
                try { Thread.sleep(30); }
                catch (InterruptedException interrupted) { throw new RuntimeException(interrupted); }
            });
            assertThrows(Exception.class, () -> request.get(2, TimeUnit.SECONDS));
            assertEquals(State.DISARMED, robot.snapshot().state());
        }
    }

    @Test void buttonEdgesSurviveApplicationPollingDelay() throws Exception {
        try (RobotSession robot = open()) {
            robot.simSet(SimItem.BUTTONS, 1);
            Thread.sleep(70);
            robot.simSet(SimItem.BUTTONS, 0);
            Thread.sleep(70);
            assertEquals(1, robot.buttonsPressed());
            assertEquals(1, robot.buttonsReleased());
            assertEquals(0, robot.buttonsPressed());
            assertEquals(0, robot.buttonsReleased());
        }
    }

    @Test void nativeStopFailureDoesNotSkipOtherClosers() {
        RobotSession robot = open();
        try (CallRecorder calls = new CallRecorder(); StatusOverride error = new StatusOverride("keel_disarm", Status.E_IO)) {
            error.next();
            assertThrows(RuntimeException.class, robot::close);
            assertTrue(calls.calls.contains("keel_close"));
            assertFalse(robot.running());
        }
    }
}
