// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static java.lang.foreign.ValueLayout.JAVA_INT;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.internal.NativeCallInterceptor;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import io.github.bisocmorg.keel.loop.CycleInput;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.State;
import io.github.bisocmorg.keel.support.CallRecorder;
import io.github.bisocmorg.keel.support.CycleDriver;
import io.github.bisocmorg.keel.support.SafetyGuardExtension;
import io.github.bisocmorg.keel.support.SimulatorDevices;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.function.Executable;

/** Close once and completely, use after close, the cleaner and the shutdown hook. */
class LifetimeTest {
    @Test
    void aSecondCloserWaitsUntilKeelCloseHasReturned() throws InterruptedException {
        Device device = SimulatorDevices.open();
        CountDownLatch inside = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        NativeCallInterceptor slowClose = new NativeCallInterceptor() {
            @Override
            public void before(String function, Object... arguments) {
                if (function.equals("keel_close")) {
                    inside.countDown();
                    try {
                        release.await(10, TimeUnit.SECONDS);
                    } catch (InterruptedException interruption) {
                        Thread.currentThread().interrupt();
                    }
                }
            }
        };
        SafetyGuardExtension.OBSERVERS.add(slowClose);
        try (CallRecorder recorder = new CallRecorder()) {
            Thread first = Thread.ofPlatform().start(device::close);
            assertTrue(inside.await(10, TimeUnit.SECONDS));
            AtomicBoolean secondReturned = new AtomicBoolean();
            Thread second = Thread.ofPlatform().start(() -> {
                device.close();
                secondReturned.set(true);
            });
            second.join(200);
            assertFalse(secondReturned.get(), "a second closer waits for keel_close to return");
            assertTrue(device.isClosed());
            assertTrue(device.closeStatus().isEmpty());
            recorder.calls.clear();
            assertThrows(HandleException.class, device::state, "a call during close is refused");
            assertTrue(recorder.calls.isEmpty(), "and makes no foreign call");
            release.countDown();
            first.join(10_000);
            second.join(10_000);
            assertTrue(secondReturned.get());
            assertEquals(Status.OK, device.closeStatus().orElseThrow());
            device.close();
        } finally {
            SafetyGuardExtension.OBSERVERS.remove(slowClose);
        }
        try (Device reopened = SimulatorDevices.open()) {
            assertFalse(reopened.isClosed());
        }
    }

    @Test
    void useAfterCloseMakesNoForeignCall() {
        Device device = SimulatorDevices.open();
        device.close();
        List<Executable> calls = List.of(device::state, device::snapshot, device::fault, device::capabilities,
            device::capabilitiesMask, device::nowUs, () -> device.sleepUntil(0),
            () -> device.tick(1, Health.ALL.mask()), () -> device.cycle(new CycleInput(Health.ALL.mask(), 1, 0, 0,
                0, 0)), device::requestArm, device::disarm, device::emergencyStop, device::clearFault,
            device::readSafetyInputs, device::readSafetyInputsMask, () -> device.setMotors(0, 0), device::coast,
            device::brake, () -> device.servoStart(1500, 100), () -> device.servoSetPulse(1500), device::servoEnd,
            device::rangeStart, device::rangePoll, device::rangeMeasure, () -> device.setRgb(0, 0),
            () -> device.setRgbAll(0), () -> device.setRgbFrame(0), () -> device.rgb(0),
            () -> device.setButtonLeds(0, 0), device::takeButtons, () -> device.simSet(SimItem.BUTTONS, 0),
            device::simState);
        try (CallRecorder recorder = new CallRecorder()) {
            for (Executable call : calls) {
                HandleException error = assertThrows(HandleException.class, call);
                assertEquals(Status.E_INVALID_HANDLE, error.status());
                assertTrue(error.getMessage().contains("no foreign call was made"), error.getMessage());
            }
            assertTrue(recorder.calls.isEmpty(), recorder.calls.toString());
        }
    }

    @Test
    void theRawHandleAfterCloseIsInvalid() {
        Device device = SimulatorDevices.open();
        MemorySegment token = device.closeState().token;
        device.close();
        NativeLibrary library = NativeLibrary.get();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment state = arena.allocate(JAVA_INT);
            assertEquals(Status.E_INVALID_HANDLE.code(), library.getState(token, state));
            assertEquals(Status.E_INVALID_HANDLE.code(), library.close(token, MemorySegment.NULL));
            assertEquals(Status.OK.code(), library.close(MemorySegment.NULL, MemorySegment.NULL));
        }
    }

    @Test
    void closeKeepsTheFinalFaultAndStatus() {
        Device device = SimulatorDevices.open();
        new CycleDriver(device).arm();
        device.emergencyStop();
        device.close();
        assertEquals(Status.OK, device.closeStatus().orElseThrow());
        Fault fault = device.finalFault().orElseThrow();
        assertEquals(FaultCause.USER_STOP, fault.cause());
        assertEquals(Status.OK, fault.cleanupStatus());
    }

    @Test
    void anInterruptedCloserKeepsItsInterrupt() {
        Device device = SimulatorDevices.open();
        Thread.currentThread().interrupt();
        device.close();
        assertTrue(Thread.interrupted());
        assertEquals(Status.OK, device.closeStatus().orElseThrow());
    }

    /** Opens a device and drops it, returning only its close state and handle. */
    private static CloseState openAndDrop() {
        return SimulatorDevices.open().closeState();
    }

    @Test
    void theCleanerClosesADroppedDeviceAndWarns() throws InterruptedException {
        List<String> warnings = new CopyOnWriteArrayList<>();
        Handler handler = capture(record -> warnings.add(record.getMessage()));
        Logger logger = Logger.getLogger("io.github.bisocmorg.keel");
        logger.addHandler(handler);
        try {
            CloseState state = openAndDrop();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20);
            while (!state.awaitClosed(20) && System.nanoTime() < deadline) {
                System.gc();
            }
            assertTrue(state.completed(), "the cleaner closed the device");
            assertEquals(Status.OK.code(), state.closeStatus());
            assertFalse(CloseState.OPEN.contains(state));
            try (Arena arena = Arena.ofConfined()) {
                assertEquals(Status.E_INVALID_HANDLE.code(),
                        NativeLibrary.get().getState(state.token, arena.allocate(JAVA_INT)));
            }
            assertTrue(warnings.stream().anyMatch(message -> message.startsWith("Device was not closed")),
                    warnings.toString());
        } finally {
            logger.removeHandler(handler);
        }
    }

    @Test
    void theShutdownHookStopsAndClosesEveryOpenDevice() {
        Device armed = SimulatorDevices.open();
        Device idle = SimulatorDevices.open();
        Device closed = SimulatorDevices.open();
        new CycleDriver(armed).arm();
        closed.close();
        ShutdownHook.run();
        for (Device device : List.of(armed, idle)) {
            assertTrue(device.isClosed());
            assertEquals(Status.OK, device.closeStatus().orElseThrow());
            assertEquals(FaultCause.USER_STOP, device.finalFault().orElseThrow().cause());
        }
        assertEquals(State.ARMED, armed.finalFault().orElseThrow().state());
        assertEquals(FaultCause.NONE, closed.finalFault().orElseThrow().cause());
        assertFalse(CloseState.OPEN.contains(armed.closeState()));
    }

    private static Handler capture(Consumer<LogRecord> sink) {
        return new Handler() {
            @Override
            public void publish(LogRecord record) {
                sink.accept(record);
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        };
    }
}
