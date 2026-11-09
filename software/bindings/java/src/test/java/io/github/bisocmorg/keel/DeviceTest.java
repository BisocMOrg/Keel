// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.errors.AgainException;
import io.github.bisocmorg.keel.errors.InvalidArgumentException;
import io.github.bisocmorg.keel.errors.SafetyFaultException;
import io.github.bisocmorg.keel.errors.StateException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.loop.CycleCommand;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.peripherals.ButtonState;
import io.github.bisocmorg.keel.peripherals.Led;
import io.github.bisocmorg.keel.peripherals.RangeMeasurement;
import io.github.bisocmorg.keel.peripherals.RangeStatus;
import io.github.bisocmorg.keel.peripherals.SensorPower;
import io.github.bisocmorg.keel.sim.SimFlag;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.sim.SimState;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.SafetyInput;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.SnapshotFlag;
import io.github.bisocmorg.keel.state.State;
import io.github.bisocmorg.keel.support.CycleDriver;
import io.github.bisocmorg.keel.support.SimulatorDevices;
import java.util.EnumSet;
import java.util.Set;
import org.junit.jupiter.api.Test;

/** Every one of the 37 functions through the public API, on the virtual-clock simulator. */
class DeviceTest {
    @Test
    void openReportsTheSimulator() {
        try (Device device = SimulatorDevices.open()) {
            OpenReport report = device.report();
            assertEquals(Status.OK, report.status());
            assertEquals(Backend.SIMULATOR, report.backend());
            assertEquals(OpenResource.NONE, report.resource());
            assertEquals(TimingClass.NONE, report.timingClass());
            assertTrue(report.warnings().isEmpty());
            assertEquals(-1, report.line());
            assertTrue(report.capabilities().containsAll(EnumSet.of(Capability.MOTORS, Capability.BRAKE,
                    Capability.SIMULATED, Capability.SERVO, Capability.RANGE, Capability.RGB)));
            assertEquals(report.capabilitiesValue(), device.capabilitiesMask());
            assertEquals(report.capabilities(), device.capabilities());
            assertEquals("Device[SIMULATOR, open]", device.toString());
            assertFalse(device.isClosed());
            assertTrue(device.closeStatus().isEmpty());
            assertTrue(device.finalFault().isEmpty());
        }
    }

    @Test
    void theDeviceClockIsVirtual() {
        try (Device device = SimulatorDevices.open()) {
            long start = device.nowUs();
            device.sleepUntil(start + 250_000);
            assertEquals(start + 250_000, device.nowUs());
            assertEquals(start + 250_000, device.simState().nowUs());
            assertThrows(InvalidArgumentException.class,
                    () -> device.sleepUntil(device.nowUs() + (Limits.SLEEP_AHEAD_MAX_MS + 1) * 1000L));
        }
    }

    @Test
    void armDriveAndDisarm() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            assertEquals(State.DISARMED, device.state());
            Snapshot armed = driver.arm();
            assertEquals(State.ARMED, armed.state());
            assertEquals(EnumSet.of(SafetyInput.READY, SafetyInput.ARMED), device.readSafetyInputs());
            assertEquals(SafetyInput.maskOf(SafetyInput.READY, SafetyInput.ARMED), device.readSafetyInputsMask());
            Snapshot driving = driver.drive(3, 400, -400);
            assertEquals(400, driving.outputLeft());
            assertEquals(-400, driving.outputRight());
            assertEquals(400, driving.commandLeft());
            assertTrue(driving.commandRemainingMs() > 0);
            SimState sim = device.simState();
            assertEquals(400, sim.bridgeLeft());
            assertEquals(-400, sim.bridgeRight());
            assertTrue(sim.flags().contains(SimFlag.MOTOR_PERMIT));
            assertTrue(sim.heartbeatFalls() > 0);
            device.setMotors(500, -500);
            assertEquals(500, device.snapshot().outputLeft());
            device.coast();
            assertEquals(0, device.snapshot().outputLeft());
            assertEquals(State.ARMED, device.state());
            device.disarm();
            assertEquals(State.DISARMED, device.state());
            assertThrows(StateException.class, () -> device.setMotors(100, 100));
        }
    }

    @Test
    void manualTicksFollowTheHealthRule() {
        try (Device device = SimulatorDevices.open()) {
            long next = device.nowUs();
            for (long id = 1; id <= 5; id++) {
                device.tick(id, Health.ALL.mask());
                next += SimulatorDevices.PERIOD_US;
                device.sleepUntil(next);
            }
            Snapshot snapshot = device.snapshot();
            assertEquals(5, snapshot.lastCycleId());
            assertTrue(snapshot.flags().contains(SnapshotFlag.HEARTBEAT_RUNNING));
            assertTrue(snapshot.ownerThreadId() != 0);
            SafetyFaultException repeated = assertThrows(SafetyFaultException.class,
                    () -> device.tick(5, Health.ALL.mask()));
            assertEquals(FaultCause.CYCLE_NOT_INCREASING, repeated.fault().orElseThrow().cause());
        }
    }

    @Test
    void aReversalWaitsForItsDwell() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            driver.drive(3, 300, 300);
            Snapshot reversing = driver.drive(1, -300, -300);
            assertTrue(reversing.flags().contains(SnapshotFlag.REVERSAL_PENDING));
            assertEquals(0, reversing.outputLeft());
            Snapshot reversed = driver.drive(3, -300, -300);
            assertFalse(reversed.flags().contains(SnapshotFlag.REVERSAL_PENDING));
            assertEquals(-300, reversed.outputLeft());
        }
    }

    @Test
    void anUnrenewedCommandExpires() {
        try (Device device = Keel.open(OpenOptions.builder().backend(Backend.SIMULATOR)
                .flags(OpenFlag.VIRTUAL_CLOCK).commandTimeoutMs(100).build())) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            assertEquals(300, driver.drive(1, 300, 300).outputLeft());
            Snapshot expired = driver.ticks(15);
            assertTrue(expired.flags().contains(SnapshotFlag.COMMAND_EXPIRED));
            assertEquals(0, expired.outputLeft());
            assertEquals(State.ARMED, expired.state());
        }
    }

    @Test
    void brakeHoldsWhileArmed() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            device.brake();
            Snapshot braking = device.snapshot();
            assertTrue(braking.flags().contains(SnapshotFlag.BRAKING));
            assertTrue(device.simState().flags().containsAll(Set.of(SimFlag.LEFT_BRAKE, SimFlag.RIGHT_BRAKE)));
            assertTrue(driver.cycle(CycleCommand.BRAKE, 0, 0).flags().contains(SnapshotFlag.BRAKING));
        }
    }

    @Test
    void servoBurstStartsChangesAndEnds() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            driver.arm();
            driver.until(snapshot -> snapshot.servoCooldownRemainingMs() == 0, 400);
            driver.ticks(3);
            device.servoStart(1500, 200);
            assertEquals(State.SERVO, device.state());
            Snapshot granted = driver.until(snapshot -> snapshot.flags().contains(SnapshotFlag.SERVO_GRANTED), 10);
            assertEquals(1500, granted.servoPulseUs());
            device.servoSetPulse(1200);
            assertEquals(1200, driver.ticks(1).servoPulseUs());
            assertEquals(1200, device.simState().servoPulseUs());
            assertThrows(InvalidArgumentException.class, () -> device.servoSetPulse(Limits.SERVO_PULSE_MAX_US + 1));
            device.servoEnd();
            assertEquals(State.DISARMED, device.state());
            device.servoEnd();
            assertThrows(StateException.class, () -> device.servoSetPulse(1500));
        }
    }

    @Test
    void rangingStartPollAndMeasure() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            RangeMeasurement idle = device.rangePoll();
            assertEquals(RangeStatus.IDLE, idle.status());
            assertFalse(idle.valid());
            driver.ticks(30);
            device.simSet(SimItem.RANGE_MM, 750);
            RangeMeasurement measured = device.rangeMeasure();
            assertEquals(RangeStatus.VALID, measured.status());
            assertEquals(750, measured.distanceMm());
            assertTrue(measured.valid());
            assertTrue(measured.echoUs() > 0);
            assertEquals(SensorPower.CONFIRMED, device.snapshot().sensorPower());
            driver.ticks(7);
            device.simSet(SimItem.RANGE_MM, 1200);
            device.rangeStart();
            assertThrows(AgainException.class, device::rangeStart);
            driver.ticks(4);
            RangeMeasurement polled = device.rangePoll();
            assertEquals(RangeStatus.VALID, polled.status());
            assertEquals(1200, polled.distanceMm());
            assertEquals(measured.sequence() + 1, polled.sequence());
            driver.ticks(7);
            device.simSet(SimItem.RANGE_MM, -1);
            assertEquals(RangeStatus.NO_ECHO, device.rangeMeasure().status());
        }
    }

    @Test
    void ledsAndTheirFrameCache() {
        try (Device device = SimulatorDevices.open()) {
            device.setRgb(Led.RGB1.code(), 0x102030);
            assertEquals(0x102030, device.rgb(Led.RGB1.code()));
            device.setRgb(Led.RGB2.code(), 0xFF405060);
            assertEquals(0x405060, device.rgb(Led.RGB2.code()), "bits 24-31 are ignored");
            device.setRgbAll(0x000008);
            for (Led led : Led.values()) {
                if (led != Led.UNRECOGNIZED) {
                    assertEquals(0x000008, device.rgb(led.code()));
                }
            }
            device.setRgbFrame(1, 2, 3, 4, 5, 6);
            assertEquals(6, device.rgb(Led.RGB6.code()));
            device.setRgbFrame(0x0A0B0C);
            assertEquals(0x0A0B0C, device.rgb(Led.RGB1.code()));
            assertEquals(2, device.rgb(Led.RGB2.code()));
            assertThrows(InvalidArgumentException.class, () -> device.setRgbFrame());
            assertThrows(InvalidArgumentException.class, () -> device.setRgbFrame(1, 2, 3, 4, 5, 6, 7));
            assertThrows(InvalidArgumentException.class, () -> device.setRgb(Limits.LED_COUNT, 0));
            assertThrows(InvalidArgumentException.class, () -> device.rgb(-1));
        }
    }

    @Test
    void buttonsAndTheirLeds() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            device.setButtonLeds(Button.maskOf(Button.A, Button.X), Button.maskOf(Button.A));
            assertEquals(EnumSet.of(Button.A), device.snapshot().buttonLeds());
            assertThrows(InvalidArgumentException.class, () -> device.setButtonLeds(0x10, 0));
            device.simSet(SimItem.BUTTONS, Button.maskOf(Button.B));
            driver.ticks(4);
            Snapshot peek = device.snapshot();
            assertEquals(EnumSet.of(Button.B), peek.buttonsHeld());
            assertEquals(EnumSet.of(Button.B), peek.buttonsPressed());
            ButtonState taken = device.takeButtons();
            assertEquals(EnumSet.of(Button.B), taken.held());
            assertEquals(EnumSet.of(Button.B), taken.pressed());
            assertTrue(taken.released().isEmpty());
            assertTrue(device.takeButtons().pressed().isEmpty());
            device.simSet(SimItem.BUTTONS, 0);
            driver.ticks(4);
            assertEquals(EnumSet.of(Button.B), device.takeButtons().released());
        }
    }

    @Test
    void simulatorStimulusAndState() {
        try (Device device = SimulatorDevices.open()) {
            new CycleDriver(device).ticks(30);
            SimState state = device.simState();
            assertEquals(0, state.bridgeLeft());
            assertTrue(state.flags().containsAll(Set.of(SimFlag.READY, SimFlag.SENSOR_PERMIT)), state.toString());
            assertFalse(state.flags().contains(SimFlag.MOTOR_PERMIT));
            assertThrows(IllegalArgumentException.class, () -> device.simSet(SimItem.UNRECOGNIZED, 0));
        }
    }

    @Test
    void emergencyStopLatchesAndClearFaultLeavesFaulted() {
        try (Device device = SimulatorDevices.open()) {
            CycleDriver driver = new CycleDriver(device);
            Fault none = device.fault();
            assertEquals(Status.OK, none.status());
            assertEquals(FaultCause.NONE, none.cause());
            assertFalse(none.recorded());
            driver.arm();
            device.emergencyStop();
            assertEquals(State.FAULTED, device.state());
            Fault fault = device.fault();
            assertEquals(Status.E_FAULTED, fault.status());
            assertEquals(FaultCause.USER_STOP, fault.cause());
            assertEquals(State.ARMED, fault.state());
            assertTrue(fault.recorded());
            assertFalse(fault.sticky());
            assertFalse(fault.cleared());
            assertEquals(1, fault.faultCount());
            device.clearFault();
            assertEquals(State.DISARMED, device.state());
            assertTrue(device.fault().cleared());
            assertTrue(device.snapshot().flags().contains(SnapshotFlag.REARM_HOLDOFF));
            assertThrows(StateException.class, device::requestArm);
        }
    }

    @Test
    void closeStoresTheResult() {
        Device device = SimulatorDevices.open();
        device.close();
        assertTrue(device.isClosed());
        assertEquals(Status.OK, device.closeStatus().orElseThrow());
        Fault fault = device.finalFault().orElseThrow();
        assertEquals(Status.OK, fault.cleanupStatus());
        assertEquals("Device[SIMULATOR, closed]", device.toString());
    }
}
