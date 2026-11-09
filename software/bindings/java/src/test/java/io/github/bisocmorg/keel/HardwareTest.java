// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.errors.SetupException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.peripherals.ButtonState;
import io.github.bisocmorg.keel.peripherals.Led;
import io.github.bisocmorg.keel.peripherals.SensorPower;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.SafetyInput;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.SnapshotFlag;
import io.github.bisocmorg.keel.state.State;
import io.github.bisocmorg.keel.support.HardwareProbe;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.List;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIfEnvironmentVariable;

/**
 * The robot stage, on the keel_r3 backend with the installed {@code libkeel.so.1}: run with
 * {@code KEEL_TEST_HARDWARE=1 mvn -B -Phardware test} and {@code KEEL_LIBRARY} unset.
 *
 * <p>Nothing here moves the robot: no test asks to arm, and the suite's motion guard refuses every call that could
 * move it, the loop's own calls included.
 */
@Tag("hardware")
@EnabledIfEnvironmentVariable(named = "KEEL_TEST_HARDWARE", matches = "1")
class HardwareTest {
    private static Device open() {
        return Keel.open(OpenOptions.builder().backend(Backend.R3).build());
    }

    @Test
    void theBoardOpensWithoutWarnings() {
        try (Device device = open()) {
            OpenReport report = device.report();
            assertEquals(Backend.R3, report.backend());
            assertTrue(report.warnings().isEmpty(), "keel-setup applied: " + report.hint());
            assertEquals(TimingClass.REALTIME, report.timingClass());
            Set<Capability> capabilities = device.capabilities();
            assertFalse(capabilities.contains(Capability.SERVO), "J5 is not fitted on R3");
            assertFalse(capabilities.contains(Capability.SIMULATED));
            assertTrue(capabilities.contains(Capability.REALTIME_PWM));
            assertTrue(Keel.versionString().contains("r3"), Keel.versionString());
        }
    }

    @Test
    void aSecondOpenFindsTheBoardInUse() {
        try (Device first = open()) {
            SetupException error = assertThrows(SetupException.class, HardwareTest::open);
            assertEquals(Status.E_IN_USE, error.status());
            assertTrue(error.report().line() >= 0, error.getMessage());
            assertFalse(error.report().consumer().isEmpty(), error.getMessage());
            assertFalse(first.isClosed());
        }
    }

    @Test
    void refusedRealTimePriorityOpensWithBestEffortTiming() throws Exception {
        String classPath = Path.of(HardwareTest.class.getProtectionDomain().getCodeSource().getLocation().toURI())
                + File.pathSeparator + System.getProperty("keel.classes.dir");
        String java = Path.of(System.getProperty("java.home"), "bin", "java").toString();
        Process process = new ProcessBuilder(List.of("prlimit", "--rtprio=0", "--", java,
                "--enable-native-access=ALL-UNNAMED", "-cp", classPath, HardwareProbe.class.getName()))
                .redirectErrorStream(true).start();
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        assertTrue(process.waitFor(60, TimeUnit.SECONDS));
        assertEquals(0, process.exitValue(), output);
        assertTrue(output.contains("warnings [BEST_EFFORT_TIMING]"), output);
        assertTrue(output.contains("WARNING") && output.contains("opened degraded"), output);
    }

    @Test
    void ledsAndButtonLedsAcceptCommands() throws InterruptedException {
        try (Device device = open()) {
            device.setRgbFrame(0x200000, 0x002000, 0x000020, 0x202000, 0x002020, 0x200020);
            device.setButtonLeds(Button.ALL.mask(), Button.ALL.mask());
            System.out.println("operator: RGB1..RGB6 show red, green, blue, yellow, cyan and magenta, and every "
                    + "button LED is lit");
            Thread.sleep(2000);
            device.setRgbAll(0);
            device.setButtonLeds(Button.ALL.mask(), 0);
            assertEquals(0, device.rgb(Led.RGB1.code()));
        }
    }

    @Test
    void takeButtonsReadsTheButtons() {
        try (Device device = open()) {
            ButtonState buttons = device.takeButtons();
            System.out.println("buttons held: " + buttons.held());
        }
    }

    @Test
    void aDisarmedLoopQualifiesTheHeartbeat() {
        try (Device device = open()) {
            ControlLoop loop = ControlLoop.builder(device, null).build();
            Snapshot snapshot = loop.run(300);
            assertEquals(State.DISARMED, snapshot.state());
            assertTrue(snapshot.flags().contains(SnapshotFlag.HEARTBEAT_RUNNING));
            assertTrue(device.readSafetyInputs().contains(SafetyInput.READY));
            assertTrue(snapshot.sensorPower() == SensorPower.PERMITTED
                    || snapshot.sensorPower() == SensorPower.CONFIRMED, snapshot.sensorPower().name());
            loop.stop();
        }
    }

    @Test
    void anEmergencyStopIsClearedThroughTheLoop() throws Exception {
        try (Device device = open(); ControlLoop loop = ControlLoop.builder(device, null).build()) {
            loop.start();
            waitFor(() -> loop.snapshot().flags().contains(SnapshotFlag.HEARTBEAT_RUNNING));
            device.emergencyStop();
            assertEquals(FaultCause.USER_STOP, device.fault().cause());
            waitFor(() -> loop.snapshot().state() == State.FAULTED);
            loop.clearFault().get(5, TimeUnit.SECONDS);
            waitFor(() -> loop.snapshot().state() == State.DISARMED
                    && loop.snapshot().flags().contains(SnapshotFlag.HEARTBEAT_RUNNING));
        }
    }

    private static void waitFor(java.util.function.BooleanSupplier condition) throws InterruptedException {
        for (int i = 0; i < 500 && !condition.getAsBoolean(); i++) {
            Thread.sleep(10);
        }
        assertTrue(condition.getAsBoolean());
    }
}
