// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.errors.AgainException;
import io.github.bisocmorg.keel.errors.ConflictException;
import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.errors.InvalidArgumentException;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.PeripheralException;
import io.github.bisocmorg.keel.errors.RejectedException;
import io.github.bisocmorg.keel.errors.SafetyFaultException;
import io.github.bisocmorg.keel.errors.SetupException;
import io.github.bisocmorg.keel.errors.StateException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.errors.StatusClass;
import io.github.bisocmorg.keel.errors.UnsupportedException;
import io.github.bisocmorg.keel.internal.AccessRegistry;
import io.github.bisocmorg.keel.internal.ErrorAccess;
import io.github.bisocmorg.keel.internal.Layouts;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import io.github.bisocmorg.keel.internal.StructCodec;
import io.github.bisocmorg.keel.loop.CycleCommand;
import io.github.bisocmorg.keel.loop.CycleInput;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.FaultFlag;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.SnapshotFlag;
import io.github.bisocmorg.keel.state.State;
import io.github.bisocmorg.keel.support.CycleDriver;
import io.github.bisocmorg.keel.support.SimulatorDevices;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Map;
import org.junit.jupiter.api.Test;

/** The status-to-exception mapping, messages, setup reports, injected faults and struct-size refusals (7, 13.3). */
class ErrorsTest {
    private static final ErrorAccess ERRORS = AccessRegistry.errors();

    private static final Map<StatusClass, Class<? extends KeelException>> BY_CLASS = Map.of(
            StatusClass.REJECTED, RejectedException.class, StatusClass.FAULT, SafetyFaultException.class,
            StatusClass.PERIPHERAL, PeripheralException.class, StatusClass.SETUP, SetupException.class,
            StatusClass.HANDLE, HandleException.class);

    private static final Map<Status, Class<? extends RejectedException>> REFINED = Map.of(
            Status.E_INVALID_ARGUMENT, InvalidArgumentException.class, Status.E_STATE, StateException.class,
            Status.E_AGAIN, AgainException.class, Status.E_UNSUPPORTED, UnsupportedException.class,
            Status.E_CONFLICT, ConflictException.class);

    private static final Map<StatusClass, String> LABELS = Map.of(StatusClass.REJECTED, "rejected",
            StatusClass.FAULT, "fault", StatusClass.PERIPHERAL, "peripheral", StatusClass.SETUP, "setup",
            StatusClass.HANDLE, "handle");

    /** Compiles only because KeelException is abstract and sealed: no default branch is needed. */
    private static String classify(KeelException error) {
        return switch (error) {
            case RejectedException rejected -> "rejected " + rejected.name();
            case SafetyFaultException fault -> "fault " + fault.name();
            case PeripheralException peripheral -> "peripheral " + peripheral.name();
            case SetupException setup -> "setup " + setup.name();
            case HandleException handle -> "handle " + handle.name();
        };
    }

    private static KeelException mapped(int code, int classValue) {
        return ERRORS.of(code, classValue, "keel_test", () -> null, null, null);
    }

    @Test
    void everyClassMapsToItsType() {
        for (Status status : Status.values()) {
            if (status == Status.OK || status == Status.UNRECOGNIZED) {
                continue;
            }
            StatusClass statusClass = Keel.statusClass(status.code());
            KeelException error = mapped(status.code(), statusClass.code());
            assertInstanceOf(BY_CLASS.get(statusClass), error, status.name());
            if (REFINED.containsKey(status)) {
                assertSame(REFINED.get(status), error.getClass(), status.name());
            } else if (statusClass == StatusClass.REJECTED) {
                assertSame(RejectedException.class, error.getClass(), status.name());
            }
            assertEquals(status, error.status());
            assertEquals(status.code(), error.code());
            assertEquals(statusClass, error.statusClass());
            assertEquals("keel_test", error.function());
            assertTrue(error.snapshot().isEmpty());
            assertEquals(LABELS.get(statusClass) + " KEEL_" + status.name(), classify(error));
        }
    }

    @Test
    void undefinedCodesAndUnknownClassesReadAsFault() {
        KeelException undefined = ERRORS.of(-99, NativeLibrary.get().statusClass(-99), "keel_test", () -> null, null,
                null);
        assertInstanceOf(SafetyFaultException.class, undefined);
        assertEquals(Status.UNRECOGNIZED, undefined.status());
        assertEquals(-99, undefined.code());
        assertEquals("KEEL_E_UNKNOWN", undefined.name());
        KeelException unknownClass = mapped(Status.E_STATE.code(), 99);
        assertInstanceOf(SafetyFaultException.class, unknownClass);
        assertEquals(StatusClass.UNRECOGNIZED, unknownClass.statusClass());
        assertTrue(((SafetyFaultException) unknownClass).fault().isEmpty());
        assertTrue(unknownClass.getMessage().endsWith("; the fault record could not be read"));
        KeelException unknownRejected = mapped(-1234, StatusClass.REJECTED.code());
        assertSame(RejectedException.class, unknownRejected.getClass());
        SetupException setupClass = (SetupException) mapped(Status.E_IN_USE.code(), StatusClass.SETUP.code());
        assertEquals(Status.E_IN_USE, setupClass.report().status());
    }

    @Test
    void messagesNameTheFunctionTheStatusAndTheRemedy() {
        KeelException conflict = mapped(Status.E_CONFLICT.code(), StatusClass.REJECTED.code());
        assertEquals("keel_test: KEEL_E_CONFLICT: " + Keel.statusString(Status.E_CONFLICT.code()),
                conflict.getMessage());
        assertEquals(Keel.statusString(Status.E_CONFLICT.code()), conflict.description());
        Fault late = new Fault(Status.E_DEADLINE.code(), FaultCause.TICK_LATE.code(), State.ARMED.code(), 0, 3, 0, 0,
                84213, 1_000_000, 1, 0, 0);
        KeelException fault = ERRORS.of(Status.E_DEADLINE.code(), StatusClass.FAULT.code(), "keel_tick", () -> late,
                null, null);
        assertTrue(fault.getMessage().startsWith("keel_tick: KEEL_E_DEADLINE: "), fault.getMessage());
        assertTrue(fault.getMessage().endsWith("; cause TICK_LATE, detail 84213"), fault.getMessage());
        assertEquals(late, ((SafetyFaultException) fault).fault().orElseThrow());
        OpenReport busy = new OpenReport(Status.E_IN_USE.code(), OpenResource.GPIO_LINE.code(), 16, 8, 1, 0, 0, 0, 0,
                "/dev/gpiochip0", "spi0", "set dtparam=spi=off");
        SetupException setup = ERRORS.setup(Status.E_IN_USE.code(), busy);
        assertTrue(setup.getMessage().endsWith("; BCM8 is used by spi0; path /dev/gpiochip0; errno 16; remedy: set "
                + "dtparam=spi=off"), setup.getMessage());
        OpenReport anonymous = new OpenReport(Status.E_IN_USE.code(), OpenResource.GPIO_LINE.code(), 0, 8, 1, 0, 0, 0,
                0, "", "", "");
        assertTrue(ERRORS.setup(Status.E_IN_USE.code(), anonymous).getMessage().endsWith("; BCM8 is unavailable"));
        OpenReport resource = new OpenReport(Status.E_NOT_FOUND.code(), OpenResource.BACKEND.code(), 0, -1, 1, 0, 0, 0,
                0, "", "", "");
        assertTrue(ERRORS.setup(Status.E_NOT_FOUND.code(), resource).getMessage().endsWith("; resource BACKEND"));
        HandleException closed = ERRORS.closed("keel_get_state");
        assertEquals(Status.E_INVALID_HANDLE, closed.status());
        assertTrue(closed.getMessage().contains("no foreign call was made"), closed.getMessage());
        InvalidArgumentException duty = ERRORS.dutyRefused("Cycle.drive", 1001, 0);
        assertEquals(StatusClass.REJECTED, duty.statusClass());
        assertTrue(duty.getMessage().contains("Cycle.drive refused duty (1001, 0)"), duty.getMessage());
    }

    @Test
    void everyFailedOpenIsASetupExceptionWithItsReport() {
        SetupException backend = assertThrows(SetupException.class,
                () -> Device.open(NativeLibrary.get(), 99, 0, 0, new byte[0], new byte[0]));
        assertEquals(Status.E_INVALID_ARGUMENT, backend.status());
        assertEquals(StatusClass.REJECTED, backend.statusClass());
        assertEquals(Status.E_INVALID_ARGUMENT, backend.report().status());
        assertEquals("keel_open", backend.function());
        SetupException timeout = assertThrows(SetupException.class, () -> Keel.open(OpenOptions.builder()
                .backend(Backend.SIMULATOR).commandTimeoutMs(5000).build()));
        assertEquals(Status.E_INVALID_ARGUMENT, timeout.report().status());
        SetupException path = assertThrows(SetupException.class, () -> Keel.open(OpenOptions.builder()
                .backend(Backend.SIMULATOR).gpioChipPath("/dev/gpiochip0").build()));
        assertEquals(Status.E_INVALID_ARGUMENT, path.status());
        SetupException port = assertThrows(SetupException.class, () -> Keel.open(OpenOptions.builder()
                .backend(Backend.PORT).build()));
        assertEquals(Status.E_INVALID_ARGUMENT, port.report().status());
    }

    @Test
    void openOptionsAreValidatedBeforeAnyCall() {
        String tooLong = "/dev/" + "x".repeat(Limits.PATH_MAX);
        assertThrows(IllegalArgumentException.class, () -> OpenOptions.builder().gpioChipPath(tooLong).build());
        assertThrows(IllegalArgumentException.class, () -> OpenOptions.builder().i2cPath("/dev/i2c\0-1").build());
        assertThrows(IllegalArgumentException.class, () -> OpenOptions.builder().backend(Backend.UNRECOGNIZED).build());
        OpenOptions options = OpenOptions.builder().backend(Backend.SIMULATOR)
                .flags(OpenFlag.VIRTUAL_CLOCK, OpenFlag.NO_SERVO).commandTimeoutMs(200).i2cPath("/dev/i2c-1").build();
        assertEquals(OpenFlag.maskOf(OpenFlag.VIRTUAL_CLOCK, OpenFlag.NO_SERVO), options.flagsMask());
        assertThrows(UnsupportedOperationException.class, () -> options.flags().clear());
        assertEquals(OpenOptions.defaults(), OpenOptions.builder().build());
        assertEquals(0, OpenOptions.defaults().flagsMask());
        assertEquals(OpenFlag.NO_RANGE.mask(),
                OpenOptions.builder().flags(OpenFlag.NO_RANGE, OpenFlag.NO_RANGE).build().flagsMask());
        assertEquals(0, OpenOptions.builder().flags(OpenFlag.NO_RANGE).flags().build().flagsMask());
    }

    @Test
    void aFaultCarriesItsRecord() {
        try (Device device = SimulatorDevices.open()) {
            new CycleDriver(device).ticks(5);
            long id = device.snapshot().lastCycleId() + 1;
            SafetyFaultException error = assertThrows(SafetyFaultException.class,
                    () -> device.tick(id, Health.CONTROL_OK.mask()));
            Fault fault = error.fault().orElseThrow();
            assertEquals(Status.E_HEALTH, error.status());
            assertEquals(FaultCause.HEALTH_FLAGS, fault.cause());
            assertEquals(Health.maskOf(Health.IO_OK, Health.NO_EXCEPTION), fault.detail());
            assertEquals(State.FAULTED, device.state());
            SafetyFaultException faulted = assertThrows(SafetyFaultException.class, device::requestArm);
            assertEquals(Status.E_FAULTED, faulted.status());
            assertEquals(FaultCause.HEALTH_FLAGS, faulted.fault().orElseThrow().cause());
        }
    }

    @Test
    void aFailedCycleCarriesItsSnapshot() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            driver.drive(2, 300, 300);
            long id = device.snapshot().lastCycleId();
            CycleInput overshoot = new CycleInput(Health.ALL.mask(), id + 1, CycleCommand.DRIVE.code(), 1001, 0, 0);
            InvalidArgumentException duty = assertThrows(InvalidArgumentException.class,
                    () -> device.cycle(overshoot));
            assertTrue(duty.snapshot().isEmpty(), "an INVALID_ARGUMENT output stays untouched");
            Snapshot coasting = device.snapshot();
            assertEquals(State.ARMED, coasting.state());
            assertEquals(0, coasting.outputLeft(), "a refused duty while ARMED coasts");
            SafetyFaultException fault = assertThrows(SafetyFaultException.class,
                    () -> device.cycle(new CycleInput(0, id + 2, CycleCommand.NONE.code(), 0, 0, 0)));
            assertEquals(State.FAULTED, fault.snapshot().orElseThrow().state());
            assertEquals(FaultCause.HEALTH_FLAGS, fault.fault().orElseThrow().cause());
        }
    }

    @Test
    void injectedControlIoFailures() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.ticks(5);
            device.simSet(SimItem.FAIL_GPIO_WRITES, 1);
            SafetyFaultException error = assertThrows(SafetyFaultException.class, () -> driver.ticks(10));
            assertEquals(Status.E_IO, error.status());
            assertEquals(FaultCause.GPIO_WRITE, error.fault().orElseThrow().cause());
        }
        try (Device device = SimulatorDevices.open()) {
            new CycleDriver(device).ticks(5);
            device.simSet(SimItem.FAIL_GPIO_READS, 1);
            SafetyFaultException error = assertThrows(SafetyFaultException.class, device::takeButtons);
            assertEquals(Status.E_IO, error.status());
            assertEquals(FaultCause.GPIO_READ, error.fault().orElseThrow().cause());
        }
    }

    @Test
    void injectedPwmFailures() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            driver.drive(3, 300, 300);
            device.simSet(SimItem.PWM_LATENESS_US, 40_000);
            SafetyFaultException error = assertThrows(SafetyFaultException.class, () -> driver.drive(5, 300, 300));
            assertEquals(Status.E_PWM, error.status());
            assertEquals(FaultCause.PWM_OVERRUN, error.fault().orElseThrow().cause());
        }
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            driver.drive(3, 300, 300);
            device.simSet(SimItem.PWM_STALL, 1);
            SafetyFaultException error = assertThrows(SafetyFaultException.class, () -> driver.drive(10, 300, 300));
            assertEquals(Status.E_PWM, error.status());
            assertEquals(FaultCause.PWM_STALL, error.fault().orElseThrow().cause());
        }
    }

    @Test
    void injectedFeedbackFailures() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            device.simSet(SimItem.MCU_RESET, 1);
            SafetyFaultException error = assertThrows(SafetyFaultException.class, () -> driver.ticks(10));
            assertEquals(Status.E_FEEDBACK, error.status());
            assertEquals(FaultCause.READY_LOST, error.fault().orElseThrow().cause());
        }
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.ticks(5);
            device.simSet(SimItem.ARMED_STUCK_HIGH, 1);
            SafetyFaultException error = assertThrows(SafetyFaultException.class, () -> driver.ticks(10));
            Fault fault = error.fault().orElseThrow();
            assertEquals(FaultCause.ARMED_UNEXPECTED, fault.cause());
            assertTrue(fault.sticky());
            assertTrue(device.snapshot().flags().contains(SnapshotFlag.FAULT_STICKY));
            SafetyFaultException sticky = assertThrows(SafetyFaultException.class, device::clearFault);
            assertEquals(Status.E_FAULTED, sticky.status());
            assertEquals(State.FAULTED, device.state());
        }
    }

    @Test
    void anInjectedLedFailureIsPeripheral() {
        try (Device device = SimulatorDevices.open()) {
            device.setRgb(0, 0x102030);
            device.simSet(SimItem.FAIL_I2C, 1);
            PeripheralException error = assertThrows(PeripheralException.class, () -> device.setRgbAll(0xFF0000));
            assertEquals(Status.E_PERIPHERAL, error.status());
            assertEquals(State.DISARMED, device.state());
            assertEquals(0x102030, device.rgb(0), "the cache keeps the last colour written successfully");
        }
    }

    @Test
    void capabilitiesAndOwnershipRefusals() {
        try (Device device = SimulatorDevices.open(OpenFlag.NO_SERVO, OpenFlag.NO_RANGE)) {
            assertFalse(device.capabilities().contains(Capability.SERVO));
            assertThrows(UnsupportedException.class, () -> device.servoStart(1500, 100));
            assertThrows(UnsupportedException.class, () -> device.servoSetPulse(1500));
            device.servoEnd();
            assertThrows(UnsupportedException.class, device::rangeStart);
            assertThrows(UnsupportedException.class, device::rangePoll);
            assertThrows(UnsupportedException.class, device::rangeMeasure);
        }
        try (Device device = SimulatorDevices.open()) {
            new CycleDriver(device).arm();
            ConflictException conflict = assertThrows(ConflictException.class, device::rangeMeasure);
            assertEquals(Status.E_CONFLICT, conflict.status());
            assertThrows(InvalidArgumentException.class, () -> device.servoStart(100, 100));
        }
    }

    @Test
    void fixedDetailIsTheTwosComplementOfAStatus() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment segment = StructCodec.allocate(arena, Layouts.FAULT);
            segment.set(JAVA_INT, 4, Status.E_INTERNAL.code());
            segment.set(JAVA_INT, 8, FaultCause.HAL_CONTRACT.code());
            segment.set(JAVA_LONG, 32, Status.E_FAULTED.code());
            segment.set(JAVA_INT, 52, FaultFlag.STICKY.mask());
            Fault fault = StructCodec.fault(segment);
            assertEquals(FaultCause.HAL_CONTRACT, fault.cause());
            assertEquals(-10L, fault.detail());
            assertEquals(Status.E_FAULTED, Status.of((int) fault.detail()));
            assertEquals("18446744073709551606", Long.toUnsignedString(fault.detail()));
            assertTrue(fault.sticky());
        }
    }

    /** Struct sizes through the private layer. */
    @Test
    void structSizesAreCheckedByTheLibrary() {
        NativeLibrary library = NativeLibrary.get();
        try (Device device = SimulatorDevices.open(); Arena arena = Arena.ofConfined()) {
            MemorySegment token = device.closeState().token;
            for (int size : new int[] {4, Limits.STRUCT_SIZE_MAX + 1}) {
                MemorySegment out = arena.allocate(Limits.STRUCT_SIZE_MAX + 64);
                out.fill((byte) 0xAB);
                out.set(JAVA_INT, 0, size);
                assertEquals(Status.E_INVALID_ARGUMENT.code(), library.getSnapshot(token, out));
                assertEquals((byte) 0xAB, out.get(JAVA_BYTE, 4), "an output with a bad struct_size stays untouched");
                assertEquals((byte) 0xAB, out.get(JAVA_BYTE, Layouts.SNAPSHOT.byteSize() - 1));
            }
            MemorySegment snapshot = StructCodec.allocate(arena, Layouts.SNAPSHOT);
            long before = device.snapshot().lastCycleId();

            MemorySegment zeroTail = arena.allocate(Layouts.CYCLE_INPUT.byteSize() + 8, 8);
            zeroTail.set(JAVA_INT, 0, (int) zeroTail.byteSize());
            zeroTail.set(JAVA_INT, 4, Health.ALL.mask());
            zeroTail.set(JAVA_LONG, 8, before + 1);
            assertEquals(0, library.cycle(token, zeroTail, snapshot), "a zero tail is accepted");
            assertEquals(before + 1, StructCodec.snapshot(snapshot).lastCycleId());

            MemorySegment tail = arena.allocate(Layouts.CYCLE_INPUT.byteSize() + 8, 8);
            MemorySegment.copy(zeroTail, 0, tail, 0, zeroTail.byteSize());
            tail.set(JAVA_LONG, 8, before + 2);
            tail.set(JAVA_BYTE, Layouts.CYCLE_INPUT.byteSize() + 3, (byte) 1);
            snapshot.fill((byte) 0);
            snapshot.set(JAVA_INT, 0, (int) Layouts.SNAPSHOT.byteSize());
            assertEquals(Status.E_UNSUPPORTED.code(), library.cycle(token, tail, snapshot));
            assertEquals(before + 1, device.snapshot().lastCycleId(), "no tick ran");
            assertEquals(before + 1, StructCodec.snapshot(snapshot).lastCycleId(), "the output receives the snapshot");

            MemorySegment shortInput = StructCodec.allocate(arena, Layouts.CYCLE_INPUT);
            StructCodec.writeCycleInput(shortInput, new CycleInput(Health.ALL.mask(), before + 3, 0, 0, 0, 0));
            shortInput.set(JAVA_INT, 0, 4);
            assertEquals(Status.E_INVALID_ARGUMENT.code(), library.cycle(token, shortInput, snapshot));
            assertEquals(before + 1, device.snapshot().lastCycleId(), "no tick ran");

            MemorySegment options = arena.allocate(Layouts.OPEN_OPTIONS.byteSize() + 8, 8);
            options.set(JAVA_INT, 0, (int) options.byteSize());
            options.set(JAVA_INT, 4, Backend.SIMULATOR.code());
            options.set(JAVA_BYTE, Layouts.OPEN_OPTIONS.byteSize(), (byte) 1);
            MemorySegment report = StructCodec.allocate(arena, Layouts.OPEN_REPORT);
            MemorySegment handle = arena.allocate(ADDRESS);
            int status = library.open(options, handle, report);
            assertEquals(Status.E_INVALID_ARGUMENT.code(), status);
            SetupException setup = ERRORS.setup(status, StructCodec.openReport(report));
            assertEquals(OpenResource.OPTIONS, setup.report().resource());
            assertEquals(Status.E_INVALID_ARGUMENT, setup.report().status());
        }
    }

    @Test
    void exceptionsAndTheirRecordsSerialize() throws IOException, ClassNotFoundException {
        SetupException setup = assertThrows(SetupException.class, () -> Keel.open(OpenOptions.builder()
                .backend(Backend.SIMULATOR).commandTimeoutMs(5000).build()));
        SetupException copy = roundTrip(setup);
        assertEquals(setup.report(), copy.report());
        assertEquals(setup.getMessage(), copy.getMessage());
        assertEquals(setup.code(), copy.code());
        try (Device device = SimulatorDevices.open()) {
            new CycleDriver(device).ticks(3);
            long id = device.snapshot().lastCycleId() + 1;
            SafetyFaultException fault = assertThrows(SafetyFaultException.class,
                    () -> device.cycle(new CycleInput(0, id, 0, 0, 0, 0)));
            SafetyFaultException faultCopy = roundTrip(fault);
            assertEquals(fault.fault(), faultCopy.fault());
            assertEquals(fault.snapshot(), faultCopy.snapshot());
        }
    }

    @SuppressWarnings("unchecked")
    private static <T> T roundTrip(T value) throws IOException, ClassNotFoundException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ObjectOutputStream out = new ObjectOutputStream(bytes)) {
            out.writeObject(value);
        }
        try (ObjectInputStream in = new ObjectInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            return (T) in.readObject();
        }
    }
}
