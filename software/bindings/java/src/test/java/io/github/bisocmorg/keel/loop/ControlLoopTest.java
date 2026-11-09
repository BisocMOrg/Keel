// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.loop;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.errors.InvalidArgumentException;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.SafetyFaultException;
import io.github.bisocmorg.keel.errors.StateException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.SnapshotFlag;
import io.github.bisocmorg.keel.state.State;
import io.github.bisocmorg.keel.support.CycleDriver;
import io.github.bisocmorg.keel.support.SimulatorDevices;
import io.github.bisocmorg.keel.support.StatusOverride;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

/** The control loop on the virtual clock: health, duties, faulted mode, mailbox, endings. */
class ControlLoopTest {
    private static final int NO_EXCEPTION = Health.NO_EXCEPTION.mask();

    /** Arms through the loop: the request runs on the loop's thread, which becomes the owner. */
    private static void arm(ControlLoop loop) {
        CompletableFuture<Void> armed = loop.requestArm();
        Snapshot snapshot = loop.runUntil(latest -> latest.state() == State.ARMED, 200);
        assertEquals(State.ARMED, snapshot.state());
        armed.join();
    }

    @Test
    void armThroughTheFutureAndDrive() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.snapshot().state() == State.ARMED) {
                    cycle.drive(200, 200);
                }
            }).build();
            CompletableFuture<Void> armed = loop.requestArm();
            Snapshot driving = loop.runUntil(snapshot -> snapshot.outputLeft() == 200, 200);
            assertTrue(armed.isDone() && !armed.isCompletedExceptionally());
            assertEquals(State.ARMED, driving.state());
            assertEquals(200, device.simState().bridgeLeft());
            assertSame(driving, loop.snapshot());
            assertTrue(loop.fault().isEmpty());
            assertTrue(loop.error().isEmpty());
            LoopStats stats = loop.stats();
            assertTrue(stats.cycles() > 50, stats.toString());
            assertEquals(0, stats.faults());
            assertEquals(SimulatorDevices.PERIOD_US, stats.maxGapUs());
            loop.stop();
            assertTrue(loop.isEnded());
            assertFalse(loop.isRunning());
            assertEquals(State.DISARMED, device.state());
        }
    }

    @Test
    void aThrowingStepFailsItsTickAndTheLoopKeepsRunning() {
        try (Device device = SimulatorDevices.open()) {
            AtomicInteger faults = new AtomicInteger();
            IllegalStateException boom = new IllegalStateException("step failed");
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 3) {
                    throw boom;
                }
            }).onFault(fault -> faults.incrementAndGet()).build();
            Snapshot snapshot = loop.run(10);
            assertEquals(State.FAULTED, snapshot.state());
            Fault fault = loop.fault().orElseThrow();
            assertEquals(FaultCause.HEALTH_FLAGS, fault.cause());
            assertEquals(NO_EXCEPTION, fault.detail());
            assertSame(boom, loop.error().orElseThrow());
            assertEquals(1, faults.get(), "onFault runs once per transition");
            LoopStats stats = loop.stats();
            assertEquals(10, stats.cycles());
            assertEquals(1, stats.stepErrors());
            assertEquals(1, stats.faults());
            assertFalse(loop.isEnded());
        }
    }

    @Test
    void aRefusedDutyFailsTheCycleEvenWhenTheStepCatchesIt() {
        try (Device device = SimulatorDevices.open()) {
            AtomicReference<InvalidArgumentException> caught = new AtomicReference<>();
            AtomicInteger refusals = new AtomicInteger(1);
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.snapshot().state() == State.ARMED && refusals.getAndDecrement() > 0) {
                    try {
                        cycle.drive(1001, 0);
                    } catch (InvalidArgumentException error) {
                        caught.set(error);
                    }
                }
            }).build();
            arm(loop);
            Snapshot faulted = loop.runUntil(snapshot -> snapshot.state() == State.FAULTED, 5);
            assertEquals(State.FAULTED, faulted.state());
            assertNotNull(caught.get());
            assertEquals(Status.E_INVALID_ARGUMENT, caught.get().status());
            Fault fault = loop.fault().orElseThrow();
            assertEquals(FaultCause.HEALTH_FLAGS, fault.cause());
            assertEquals(NO_EXCEPTION, fault.detail());
            assertTrue(loop.error().isEmpty(), "the step caught the refusal");
            CompletableFuture<Void> cleared = loop.clearFault();
            assertEquals(State.DISARMED, loop.run(1).state());
            cleared.join();
            assertFalse(loop.isEnded());
        }
    }

    @Test
    void anUncaughtRefusedDutyIsAStepError() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.snapshot().state() == State.ARMED) {
                    cycle.drive(0, -1001);
                }
            }).build();
            arm(loop);
            loop.runUntil(snapshot -> snapshot.state() == State.FAULTED, 5);
            assertInstanceOf(InvalidArgumentException.class, loop.error().orElseThrow());
            assertEquals(NO_EXCEPTION, loop.fault().orElseThrow().detail());
        }
    }

    @Test
    void setCommandRefusesAnOutOfRangeDutyOnThePostingThread() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            arm(loop);
            loop.setCommand(300, 300);
            assertEquals(300, loop.run(1).outputLeft());
            InvalidArgumentException error = assertThrows(InvalidArgumentException.class,
                    () -> loop.setCommand(1001, 0));
            assertTrue(error.getMessage().contains("ControlLoop.setCommand"), error.getMessage());
            assertThrows(InvalidArgumentException.class, () -> loop.setCommand(0, Integer.MIN_VALUE));
            Snapshot coasting = loop.run(1);
            assertEquals(0, coasting.outputLeft());
            assertEquals(State.ARMED, coasting.state(), "the loop stays healthy");
            assertTrue(loop.fault().isEmpty());
        }
    }

    @Test
    void aSlowStepOverrunsItsBudget() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 4) {
                    cycle.device().sleepUntil(cycle.nowUs() + 45_000);
                }
            }).build();
            loop.run(6);
            Fault fault = loop.fault().orElseThrow();
            assertEquals(FaultCause.HEALTH_FLAGS, fault.cause());
            assertEquals(Health.CONTROL_OK.mask(), fault.detail());
            assertTrue(loop.stats().maxStepUs() >= 45_000);
        }
    }

    @Test
    void aSlowSubmittedFunctionCountsAgainstTheBudget() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, null).stepBudgetMs(20).build();
            loop.run(3);
            CompletableFuture<Integer> result = loop.submit(target -> {
                target.sleepUntil(target.nowUs() + 25_000);
                return 7;
            });
            loop.run(2);
            assertEquals(7, result.join());
            assertEquals(Health.CONTROL_OK.mask(), loop.fault().orElseThrow().detail());
        }
    }

    @Test
    void anIoFailureFailsTheTick() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 2) {
                    cycle.reportIoFailure();
                }
            }).build();
            loop.run(4);
            assertEquals(Health.IO_OK.mask(), loop.fault().orElseThrow().detail());
        }
    }

    @Test
    void clearFaultThroughTheLoopAndTheRearmHoldoff() {
        try (Device device = SimulatorDevices.open()) {
            AtomicReference<Fault> seen = new AtomicReference<>();
            ControlLoop loop = ControlLoop.builder(device, null).onFault(seen::set).build();
            arm(loop);
            device.emergencyStop();
            assertEquals(State.FAULTED, loop.run(1).state());
            assertEquals(FaultCause.USER_STOP, seen.get().cause());
            CompletableFuture<Void> cleared = loop.clearFault();
            assertEquals(State.DISARMED, loop.run(1).state());
            cleared.join();
            CompletableFuture<Void> early = loop.requestArm();
            loop.run(1);
            CompletionException holdoff = assertThrows(CompletionException.class, early::join);
            assertInstanceOf(StateException.class, holdoff.getCause());
            loop.run(Limits.REARM_HOLDOFF_MS / Limits.TICK_PERIOD_MS);
            arm(loop);
            assertEquals(1, loop.stats().faults());
        }
    }

    @Test
    void aFaultThatLatchesAgainInTheClearingCycleIsReported() {
        try (Device device = SimulatorDevices.open()) {
            List<Fault> seen = new ArrayList<>();
            // The cause persists: every cycle reports failed I/O, so the cycle that clears the fault latches again.
            ControlLoop loop = ControlLoop.builder(device, Cycle::reportIoFailure).onFault(seen::add).build();
            assertEquals(State.FAULTED, loop.run(5).state());
            assertEquals(1, seen.size());
            CompletableFuture<Void> cleared = loop.clearFault();
            Snapshot snapshot = loop.run(5);
            cleared.join();
            assertEquals(State.FAULTED, snapshot.state());
            assertEquals(2, snapshot.faultCount());
            assertEquals(2, seen.size());
            assertEquals(2, seen.get(1).faultCount());
            assertEquals(2, loop.stats().faults());
            assertSame(seen.get(1), loop.fault().orElseThrow());
        }
    }

    @Test
    void theMailboxIsOneShotAndTheDeadmanExpires() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            arm(loop);
            loop.setCommand(300, -300);
            Snapshot driving = loop.run(1);
            assertEquals(300, driving.outputLeft());
            assertEquals(-300, driving.outputRight());
            Snapshot expired = loop.run(Limits.COMMAND_TIMEOUT_DEFAULT_MS / Limits.TICK_PERIOD_MS + 5);
            assertTrue(expired.flags().contains(SnapshotFlag.COMMAND_EXPIRED));
            assertEquals(0, expired.outputLeft());
            assertEquals(State.ARMED, expired.state());
            loop.brake();
            assertTrue(loop.run(1).flags().contains(SnapshotFlag.BRAKING));
            loop.coast();
            assertFalse(loop.run(1).flags().contains(SnapshotFlag.BRAKING));
        }
    }

    @Test
    void theStepsCommandWinsOverAPostedOne() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.snapshot().state() == State.ARMED) {
                    cycle.drive(100, 100);
                }
            }).build();
            arm(loop);
            loop.setCommand(500, 500);
            assertEquals(100, loop.run(2).outputLeft());
        }
    }

    @Test
    void aBrakingStepAndAnExplicitCoast() {
        try (Device device = SimulatorDevices.open()) {
            AtomicInteger mode = new AtomicInteger();
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                switch (mode.get()) {
                    case 1 -> cycle.brake();
                    case 2 -> cycle.coast();
                    default -> {
                    }
                }
            }).build();
            arm(loop);
            mode.set(1);
            assertTrue(loop.run(1).flags().contains(SnapshotFlag.BRAKING));
            mode.set(2);
            assertFalse(loop.run(1).flags().contains(SnapshotFlag.BRAKING));
        }
    }

    @Test
    void anInvalidArgumentOrUnsupportedCycleIsFatal() {
        for (Status forced : new Status[] {Status.E_UNSUPPORTED, Status.E_INVALID_ARGUMENT}) {
            try (Device device = SimulatorDevices.open();
                    StatusOverride override = new StatusOverride("keel_cycle", forced)) {
                ControlLoop loop = ControlLoop.builder(device, null).build();
                arm(loop);
                override.next();
                KeelException error = assertThrows(KeelException.class, () -> loop.run(5));
                assertEquals(forced, error.status());
                assertTrue(loop.isEnded());
                assertEquals(State.DISARMED, device.state(), "the loop disarms");
                assertSame(error, assertThrows(KeelException.class, loop::join));
                assertThrows(IllegalStateException.class, () -> loop.run(1));
                assertThrows(IllegalStateException.class, loop::start);
                assertEquals(forced == Status.E_UNSUPPORTED, error.snapshot().isPresent());
            }
        }
    }

    @Test
    void closingTheDeviceEndsTheLoopNormally() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 3) {
                    cycle.device().close();
                }
            }).build();
            loop.run(100);
            assertTrue(loop.isEnded());
            assertEquals(2, loop.stats().cycles(), "the cycle whose step closed the device makes no foreign call");
            assertTrue(loop.error().isEmpty());
            assertTrue(device.isClosed());
        }
    }

    @Test
    void anUnexpectedHandleResultIsStored() {
        try (Device device = SimulatorDevices.open();
                StatusOverride override = new StatusOverride("keel_cycle", Status.E_INVALID_HANDLE)) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            override.next();
            HandleException error = assertThrows(HandleException.class, () -> loop.run(5));
            assertEquals("keel_cycle", error.function());
            assertTrue(loop.isEnded());
        }
    }

    @Test
    void aFailingDeviceClockEndsTheLoop() {
        Object[][] cases = {{Status.E_IO, SafetyFaultException.class}, {Status.E_INVALID_ARGUMENT,
            InvalidArgumentException.class}, {Status.E_INVALID_HANDLE, HandleException.class}};
        for (Object[] item : cases) {
            Status forced = (Status) item[0];
            try (Device device = SimulatorDevices.open();
                    StatusOverride override = new StatusOverride("keel_sleep_until", forced)) {
                ControlLoop loop = ControlLoop.builder(device, null).build();
                loop.run(2);
                override.next();
                KeelException error = assertThrows(KeelException.class, () -> loop.run(5));
                assertInstanceOf((Class<?>) item[1], error);
                assertEquals("keel_sleep_until", error.function());
                assertTrue(loop.isEnded());
            }
        }
    }

    @Test
    void peripheralAndUnknownResultsLetTheLoopContinue() {
        try (Device device = SimulatorDevices.open();
                StatusOverride peripheral = new StatusOverride("keel_cycle", Status.E_PERIPHERAL);
                StatusOverride unknown = new StatusOverride("keel_cycle", -99)) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            peripheral.next();
            loop.run(2);
            unknown.next();
            loop.run(2);
            assertFalse(loop.isEnded());
            assertEquals(4, loop.stats().cycles());
            assertTrue(loop.fault().isEmpty());
        }
    }

    @Test
    void rejectedResultsAreCountedAndTheLoopContinues() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> cycle.drive(100, 100)).build();
            Snapshot snapshot = loop.run(5);
            assertEquals(State.DISARMED, snapshot.state());
            assertEquals(Status.E_STATE, snapshot.lastRejection());
            assertEquals(5L, loop.stats().rejected().get(Status.E_STATE));
            assertTrue(loop.fault().isEmpty());
        }
    }

    @Test
    void startIsRefusedOnceAnOwnerIsRecorded() {
        try (Device device = SimulatorDevices.open()) {
            new CycleDriver(device).ticks(1);
            ControlLoop loop = ControlLoop.builder(device, null).build();
            IllegalStateException error = assertThrows(IllegalStateException.class, loop::start);
            assertTrue(error.getMessage().contains("TICK_THREAD_CHANGED"), error.getMessage());
            assertFalse(loop.isRunning());
            Snapshot snapshot = loop.run(3);
            assertEquals(4, snapshot.lastCycleId(), "the calling thread is the owner, so run() continues");
        }
    }

    @Test
    void runIsRefusedForAForeignOwner() throws InterruptedException {
        try (Device device = SimulatorDevices.open()) {
            Thread other = Thread.ofPlatform().start(() -> device.tick(1, Health.ALL.mask()));
            other.join();
            ControlLoop loop = ControlLoop.builder(device, null).build();
            IllegalStateException error = assertThrows(IllegalStateException.class, () -> loop.run(1));
            assertTrue(error.getMessage().contains("not this thread"), error.getMessage());
            assertFalse(loop.isEnded());
        }
    }

    @Test
    void theCycleIdContinuesFromTheLastAcceptedTick() {
        try (Device device = SimulatorDevices.open()) {
            device.tick(1000, Health.ALL.mask());
            ControlLoop loop = ControlLoop.builder(device, null).build();
            assertEquals(1001, loop.run(1).lastCycleId());
            assertEquals(1003, loop.run(2).lastCycleId());
        }
    }

    @Test
    void stopAndCloseFromTheStepReturnWithoutJoining() {
        try (Device device = SimulatorDevices.open()) {
            AtomicReference<ControlLoop> self = new AtomicReference<>();
            AtomicReference<IllegalStateException> joinError = new AtomicReference<>();
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 3) {
                    try {
                        self.get().join();
                    } catch (IllegalStateException error) {
                        joinError.set(error);
                    }
                    self.get().stop();
                }
            }).build();
            self.set(loop);
            loop.run(100);
            assertEquals(3, loop.stats().cycles());
            assertTrue(loop.isEnded());
            assertNotNull(joinError.get());
            assertEquals(State.DISARMED, device.state());
        }
        try (Device device = SimulatorDevices.open()) {
            AtomicReference<ControlLoop> self = new AtomicReference<>();
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 2) {
                    self.get().close();
                }
            }).build();
            self.set(loop);
            loop.run(100);
            assertEquals(2, loop.stats().cycles());
            assertTrue(loop.isEnded());
        }
    }

    @Test
    void aSecondLoopIsRefused() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop.builder(device, null).build();
            assertThrows(IllegalStateException.class, () -> ControlLoop.builder(device, null).build());
        }
    }

    @Test
    void builderAndArgumentChecks() throws InterruptedException {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop.Builder builder = ControlLoop.builder(device, null);
            assertThrows(IllegalArgumentException.class, () -> builder.periodMs(0));
            assertThrows(IllegalArgumentException.class, () -> builder.periodMs(Limits.TICK_PERIOD_MS + 1));
            assertThrows(IllegalArgumentException.class, () -> builder.stepBudgetMs(0));
            assertThrows(IllegalArgumentException.class, () -> builder.stepBudgetMs(Limits.STALL_BUDGET_MS + 1));
            assertThrows(NullPointerException.class, () -> builder.threadName(null));
            ControlLoop loop = builder.periodMs(5).stepBudgetMs(30).takeButtons(false).threadName("robot").build();
            assertThrows(IllegalArgumentException.class, () -> loop.run(-1));
            assertThrows(NullPointerException.class, () -> loop.runUntil(null, 1));
            long start = device.nowUs();
            loop.run(10);
            assertEquals(start + 50_000, device.nowUs());
            loop.join();
            assertTrue(loop.join(Duration.ofMillis(10)));
            loop.stop();
            assertTrue(loop.isEnded());
            CompletableFuture<Void> late = loop.requestArm();
            CompletionException error = assertThrows(CompletionException.class, late::join);
            assertInstanceOf(HandleException.class, error.getCause());
            assertThrows(IllegalStateException.class, () -> loop.run(1));
        }
    }

    @Test
    void buttonEdgesAreTakenOnlyWhenAsked() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, null).takeButtons(false).build();
            device.simSet(SimItem.BUTTONS, Button.maskOf(Button.X));
            loop.run(4);
            assertTrue(device.takeButtons().pressed().contains(Button.X), "the edges were left for takeButtons");
        }
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            device.simSet(SimItem.BUTTONS, Button.maskOf(Button.X));
            loop.run(4);
            assertTrue(device.takeButtons().pressed().isEmpty(), "each cycle took its edges");
        }
    }

    @Test
    void anOverrunResynchronisesInsteadOfCatchingUp() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> cycle.device().sleepUntil(cycle.nowUs() + 25_000))
                    .build();
            loop.run(5);
            assertTrue(loop.stats().overruns() > 0, loop.stats().toString());
            assertTrue(loop.fault().isEmpty(), "25 ms is within the 40 ms budget");
        }
    }

    @Test
    void anInterruptIsAStopRequest() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            Thread.currentThread().interrupt();
            loop.run(10);
            assertTrue(Thread.interrupted(), "the loop leaves the caller's interrupt status set");
            assertTrue(loop.isEnded());
            assertEquals(0, loop.stats().cycles());
        }
    }

    @Test
    void anErrorInTheStepIsAnEmergencyStop() {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 2) {
                    throw new AssertionError("defect in the step");
                }
            }).build();
            AssertionError error = assertThrows(AssertionError.class, () -> loop.run(10));
            assertEquals("defect in the step", error.getMessage());
            assertTrue(loop.isEnded());
            assertEquals(FaultCause.USER_STOP, device.fault().cause());
            assertDoesNotThrow(loop::close, "run() already reported the error");
            assertSame(error, assertThrows(AssertionError.class, loop::join), "join() reports it again");
        }
    }

    @Test
    void aFailingLoopInTryWithResourcesReportsItsOwnError() {
        try (Device device = SimulatorDevices.open()) {
            AssertionError error = assertThrows(AssertionError.class, () -> {
                try (ControlLoop loop = ControlLoop.builder(device, cycle -> {
                    if (cycle.cycleId() == 2) {
                        throw new AssertionError("defect in the step");
                    }
                }).build()) {
                    loop.run(10);
                }
            });
            assertEquals("defect in the step", error.getMessage());
            assertEquals(0, error.getSuppressed().length);
        }
        try (Device device = SimulatorDevices.open()) {
            IllegalStateException error = assertThrows(IllegalStateException.class, () -> {
                try (ControlLoop loop = ControlLoop.builder(device, null).build()) {
                    loop.runUntil(snapshot -> {
                        throw new IllegalStateException("condition failed");
                    }, 10);
                }
            });
            assertEquals("condition failed", error.getMessage());
            assertEquals(0, error.getSuppressed().length);
        }
    }

    @Test
    void closeReportsAFatalErrorThatNoOneHasReceived() throws InterruptedException {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 2) {
                    throw new AssertionError("defect on the loop's thread");
                }
            }).build();
            loop.start();
            while (loop.isRunning()) {
                Thread.sleep(5);
            }
            AssertionError error = assertThrows(AssertionError.class, loop::close);
            assertEquals("defect on the loop's thread", error.getMessage());
            assertDoesNotThrow(loop::close, "reported once");
            assertSame(error, assertThrows(AssertionError.class, loop::join));
        }
    }

    @Test
    void joinAndCloseWaitForALoopRunningSynchronouslyOnAnotherThread() throws InterruptedException {
        try (Device device = SimulatorDevices.open()) {
            CountDownLatch inStep = new CountDownLatch(1);
            CountDownLatch release = new CountDownLatch(1);
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.cycleId() == 2) {
                    inStep.countDown();
                    release.await();
                }
            }).build();
            Thread runner = Thread.ofPlatform().start(() -> loop.run(5));
            inStep.await();
            assertFalse(loop.join(Duration.ofMillis(50)), "the loop still runs on the other thread");
            assertTrue(loop.isRunning());
            Thread releaser = Thread.ofPlatform().start(() -> {
                try {
                    Thread.sleep(100);
                } catch (InterruptedException interruption) {
                    Thread.currentThread().interrupt();
                }
                release.countDown();
            });
            loop.close();
            assertFalse(loop.isRunning(), "close() returned only after the loop ended");
            assertEquals(0, release.getCount());
            assertTrue(loop.join(Duration.ofSeconds(1)));
            runner.join();
            releaser.join();
            assertEquals(State.DISARMED, device.state());
        }
    }

    @Test
    void aThrowingOnFaultIsKeptAsTheError() {
        try (Device device = SimulatorDevices.open()) {
            IllegalStateException failure = new IllegalStateException("callback failed");
            ControlLoop loop = ControlLoop.builder(device, null).onFault(fault -> {
                throw failure;
            }).build();
            device.emergencyStop();
            loop.run(3);
            assertSame(failure, loop.error().orElseThrow());
            assertFalse(loop.isEnded());
        }
    }

    @Test
    void emergencyStopEndsTheLoopWithUserStop() {
        Device device = SimulatorDevices.open();
        ControlLoop loop = ControlLoop.builder(device, null).build();
        arm(loop);
        loop.emergencyStop();
        assertTrue(loop.isEnded());
        assertEquals(FaultCause.USER_STOP, device.fault().cause());
        device.close();
        loop.stop();
        loop.close();
        assertTrue(device.isClosed());
    }
}
