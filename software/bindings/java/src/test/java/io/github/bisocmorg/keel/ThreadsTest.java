// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import io.github.bisocmorg.keel.errors.ConflictException;
import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.errors.SafetyFaultException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.CycleInput;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.peripherals.RangeStatus;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.State;
import io.github.bisocmorg.keel.support.CycleDriver;
import io.github.bisocmorg.keel.support.SimulatorDevices;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import org.junit.jupiter.api.Test;

/** "Stop from anywhere, go only from the loop", virtual threads and close racing calls. */
class ThreadsTest {
    /** Runs an action on a new platform thread and returns what it threw, or null. */
    private static Throwable onOtherThread(Runnable action) throws InterruptedException {
        AtomicReference<Throwable> thrown = new AtomicReference<>();
        Thread thread = Thread.ofPlatform().start(() -> {
            try {
                action.run();
            } catch (RuntimeException | Error error) {
                thrown.set(error);
            }
        });
        thread.join();
        return thrown.get();
    }

    private static void await(BooleanSupplier condition, String what) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() > deadline) {
                fail("timed out waiting until " + what);
            }
            Thread.sleep(5);
        }
    }

    @Test
    void goCallsFromAnotherThreadConflictAndStopsAreAccepted() throws InterruptedException {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            driver.drive(3, 300, 300);
            assertInstanceOf(ConflictException.class, onOtherThread(() -> device.setMotors(100, 100)));
            assertInstanceOf(ConflictException.class, onOtherThread(device::brake));
            assertInstanceOf(ConflictException.class, onOtherThread(device::requestArm));
            assertEquals(null, onOtherThread(device::coast));
            Snapshot snapshot = driver.ticks(1);
            assertEquals(State.ARMED, snapshot.state());
            assertEquals(0, snapshot.outputLeft());
            assertEquals(null, onOtherThread(device::disarm));
            assertEquals(State.DISARMED, device.state());
        }
    }

    @Test
    void aTickFromAnotherThreadFailStops() throws InterruptedException {
        try (Device device = SimulatorDevices.open()) {
            new CycleDriver(device).ticks(3);
            long id = device.snapshot().lastCycleId() + 1;
            Throwable thrown = onOtherThread(() -> device.tick(id, Health.ALL.mask()));
            SafetyFaultException fault = assertInstanceOf(SafetyFaultException.class, thrown);
            assertEquals(Status.E_HEALTH, fault.status());
            assertEquals(FaultCause.TICK_THREAD_CHANGED, fault.fault().orElseThrow().cause());
            assertInstanceOf(ConflictException.class, onOtherThread(device::clearFault));
            device.clearFault();
        }
    }

    @Test
    void anEmergencyStopFromAnotherThreadPutsTheLoopInFaultedMode() throws Exception {
        try (Device device = SimulatorDevices.realtime()) {
            CountDownLatch faulted = new CountDownLatch(1);
            AtomicReference<Fault> fault = new AtomicReference<>();
            AtomicReference<Thread> loopThread = new AtomicReference<>();
            try (ControlLoop loop = ControlLoop.builder(device, cycle -> loopThread.set(Thread.currentThread()))
                    .onFault(record -> {
                        fault.set(record);
                        faulted.countDown();
                    }).build()) {
                loop.start();
                await(() -> loop.stats().cycles() >= 5, "the loop runs");
                assertEquals("keel-loop", loopThread.get().getName());
                assertFalse(loopThread.get().isVirtual());
                assertFalse(loopThread.get().isDaemon());
                device.emergencyStop();
                assertTrue(faulted.await(10, TimeUnit.SECONDS));
                assertEquals(FaultCause.USER_STOP, fault.get().cause());
                long cycles = loop.stats().cycles();
                await(() -> loop.stats().cycles() > cycles + 3, "the loop keeps running in FAULTED");
                assertTrue(loop.isRunning());
                assertEquals(State.FAULTED, loop.snapshot().state());
                assertEquals(fault.get(), loop.fault().orElseThrow());
                loop.clearFault().get(10, TimeUnit.SECONDS);
                await(() -> loop.snapshot().state() == State.DISARMED, "the loop leaves FAULTED");
            }
            assertEquals(State.DISARMED, device.state());
        }
    }

    @Test
    void closeRacesCallsInFlightSafely() throws InterruptedException {
        Device device = SimulatorDevices.realtime();
        AtomicReference<Throwable> sleeper = new AtomicReference<>();
        List<Throwable> callers = new CopyOnWriteArrayList<>();
        CountDownLatch started = new CountDownLatch(3);
        Thread sleeping = Thread.ofPlatform().start(() -> {
            try {
                started.countDown();
                device.sleepUntil(device.nowUs() + 5_000_000);
            } catch (RuntimeException error) {
                sleeper.set(error);
            }
        });
        Thread[] pounders = new Thread[2];
        for (int i = 0; i < pounders.length; i++) {
            pounders[i] = Thread.ofPlatform().start(() -> {
                started.countDown();
                try {
                    while (true) {
                        device.snapshot();
                        device.setRgbAll(0x000100);
                    }
                } catch (RuntimeException error) {
                    callers.add(error);
                }
            });
        }
        assertTrue(started.await(5, TimeUnit.SECONDS));
        Thread.sleep(50);
        long start = System.nanoTime();
        device.close();
        sleeping.join(5000);
        for (Thread pounder : pounders) {
            pounder.join(5000);
        }
        assertTrue(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start) < 2000, "close woke the sleeper");
        assertInstanceOf(HandleException.class, sleeper.get());
        assertEquals(2, callers.size());
        for (Throwable error : callers) {
            assertInstanceOf(HandleException.class, error);
        }
        assertEquals(Status.OK, device.closeStatus().orElseThrow());
    }

    @Test
    void virtualThreadsCannotTickCycleSleepOrRunTheLoop() throws InterruptedException {
        try (Device device = SimulatorDevices.open()) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            List<Throwable> thrown = new CopyOnWriteArrayList<>();
            Thread.ofVirtual().start(() -> {
                Runnable[] calls = {() -> device.tick(1, Health.ALL.mask()),
                    () -> device.cycle(new CycleInput(Health.ALL.mask(), 1, 0, 0, 0, 0)),
                    () -> device.sleepUntil(0), () -> loop.run(1), () -> loop.runUntil(snapshot -> true, 1)};
                for (Runnable call : calls) {
                    try {
                        call.run();
                    } catch (RuntimeException error) {
                        thrown.add(error);
                    }
                }
                thrown.add(new IllegalStateException(device.rangeMeasure().status().name()));
            }).join();
            assertEquals(6, thrown.size(), thrown.toString());
            for (Throwable error : thrown.subList(0, 5)) {
                assertInstanceOf(WrongThreadException.class, error);
            }
            assertEquals(RangeStatus.UNAVAILABLE.name(), thrown.get(5).getMessage(),
                    "other calls are allowed on a virtual thread");
            assertEquals(0, device.snapshot().lastCycleId(), "no tick reached libkeel");
        }
    }

    @Test
    void startIsAllowedFromAVirtualThread() throws InterruptedException {
        try (Device device = SimulatorDevices.realtime()) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            AtomicReference<Throwable> thrown = new AtomicReference<>();
            Thread.ofVirtual().start(() -> {
                try {
                    loop.start();
                } catch (RuntimeException error) {
                    thrown.set(error);
                }
            }).join();
            assertEquals(null, thrown.get());
            await(() -> loop.stats().cycles() >= 3, "the loop runs on its platform thread");
            assertTrue(loop.isRunning());
            assertInstanceOf(IllegalStateException.class, assertThrowsOn(loop::start));
            assertInstanceOf(IllegalStateException.class, assertThrowsOn(() -> loop.run(1)));
            loop.close();
            assertTrue(loop.isEnded());
            assertFalse(loop.isRunning());
            assertTrue(loop.join(java.time.Duration.ofSeconds(1)));
            loop.join();
        }
    }

    private static Throwable assertThrowsOn(Runnable action) {
        try {
            action.run();
        } catch (RuntimeException error) {
            return error;
        }
        return fail("expected an exception");
    }

    @Test
    void closingTheDeviceEndsAStartedLoopNormally() throws InterruptedException {
        Device device = SimulatorDevices.realtime();
        ControlLoop loop = ControlLoop.builder(device, null).build();
        loop.start();
        await(() -> loop.stats().cycles() >= 3, "the loop runs");
        device.close();
        assertTrue(loop.join(java.time.Duration.ofSeconds(5)));
        assertTrue(loop.isEnded());
        assertTrue(loop.error().isEmpty());
    }
}
