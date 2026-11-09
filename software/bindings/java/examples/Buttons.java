// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.errors.UnsupportedException;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.Cycle;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.State;

/**
 * Buttons: each button lights its indicator LED, button edges are printed, button B ends the example normally and
 * Ctrl-C is an emergency stop, as stop buttons in applications should be.
 *
 * <p>Without {@code --drive} nothing moves. With {@code --drive LEFT RIGHT MS} it also arms, drives at LEFT/RIGHT
 * permille for MS milliseconds, then disarms and ends; that option is itself the operator's intent to move.
 * {@code --duration MS} ends the example after MS milliseconds.
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/Buttons.java --simulator --duration 2000
 * </pre>
 *
 * <p>Exit status: 0 success, 1 failure, 2 usage, 128 + N after signal N (130 after Ctrl-C, 143 after SIGTERM); the
 * binding's shutdown hook then makes the emergency stop and closes the device.
 */
public final class Buttons {
    private static final int IDLE_BLUE = 0x000008;
    private static final long ARM_LIMIT_US = 5_000_000;

    private boolean drive;
    private int left;
    private int right;
    private long driveMs;
    private long durationMs;
    private boolean simulator;
    private long startedUs;
    private long drivingSinceUs = -1;
    private int lastState = State.DISARMED.code();
    private boolean done;
    private boolean failed;

    private Buttons() {
    }

    /**
     * Runs the example.
     *
     * @param args {@code [--drive LEFT RIGHT MS] [--duration MS] [--simulator]}
     */
    public static void main(String[] args) {
        System.exit(new Buttons().run(args));
    }

    private static void usage() {
        System.err.println("Usage: Buttons [--drive LEFT RIGHT MS] [--duration MS] [--simulator]");
        System.err.println("Mirrors the buttons on their LEDs; button B ends the example, Ctrl-C stops the robot.");
    }

    private boolean parse(String[] args) {
        try {
            for (int i = 0; i < args.length; i++) {
                switch (args[i]) {
                    case "--drive" -> {
                        left = bounded(args[++i], -Limits.DUTY_MAX, Limits.DUTY_MAX);
                        right = bounded(args[++i], -Limits.DUTY_MAX, Limits.DUTY_MAX);
                        driveMs = bounded(args[++i], 1, 2000);
                        drive = true;
                    }
                    case "--duration" -> durationMs = bounded(args[++i], 1, 3_600_000);
                    case "--simulator" -> simulator = true;
                    default -> {
                        return false;
                    }
                }
            }
            return true;
        } catch (ArrayIndexOutOfBoundsException | IllegalArgumentException error) {
            return false;
        }
    }

    private static int bounded(String text, int low, int high) {
        int value = Integer.parseInt(text);
        if (value < low || value > high) {
            throw new IllegalArgumentException(text);
        }
        return value;
    }

    private int run(String[] args) {
        if (!parse(args)) {
            usage();
            return 2;
        }
        OpenOptions options = OpenOptions.builder().backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build();
        try (Device device = Keel.open(options)) {
            if (!device.report().warnings().isEmpty()) {
                System.out.println("opened degraded " + device.report().warnings() + ": " + device.report().hint());
            }
            try {
                device.setRgbAll(IDLE_BLUE);
            } catch (UnsupportedException noRgb) {
                System.out.println("RGB LEDs unavailable; continuing without them");
            }
            startedUs = device.nowUs();
            ControlLoop loop = ControlLoop.builder(device, this::step).build();
            if (drive) {
                device.requestArm(); // only with --drive, the operator's decision to move
            }
            loop.runUntil(snapshot -> done, Long.MAX_VALUE);
            boolean moving = lastState == State.ARMING.code() || lastState == State.ARMED.code();
            device.disarm();
            System.out.println(moving ? "state: DISARMED" : "buttons: done");
            return failed ? 1 : 0;
        } catch (LibraryException | KeelException error) {
            System.err.println("buttons: " + error.getMessage());
            return 1;
        }
    }

    /** One cycle: report changes, mirror the buttons on their LEDs, drive while the time lasts. */
    private void step(Cycle cycle) {
        Snapshot snapshot = cycle.snapshot();
        long now = cycle.nowUs();
        if (snapshot.stateValue() != lastState) {
            System.out.println("state: " + snapshot.state());
            lastState = snapshot.stateValue();
            if (snapshot.state() == State.FAULTED) {
                Fault fault = cycle.device().fault();
                System.err.println("stopped: " + fault.status() + ", cause " + fault.cause());
                failed = true;
                done = true;
                return;
            }
        }
        if (snapshot.buttonsPressedValue() != 0 || snapshot.buttonsReleasedValue() != 0) {
            System.out.printf("buttons: held=%x pressed=%x released=%x%n", snapshot.buttonsHeldValue(),
                    snapshot.buttonsPressedValue(), snapshot.buttonsReleasedValue());
        }
        if (snapshot.buttonsHeld().contains(Button.B)) {
            done = true;
            return;
        }
        if (snapshot.buttonLedsValue() != snapshot.buttonsHeldValue()) {
            cycle.device().setButtonLeds(Button.ALL.mask(), snapshot.buttonsHeldValue());
        }
        if (drive && snapshot.state() == State.ARMED) {
            if (drivingSinceUs < 0) {
                drivingSinceUs = now;
            }
            if (now - drivingSinceUs >= driveMs * 1000) {
                done = true;
                return;
            }
            cycle.drive(left, right);
        }
        if (drive && drivingSinceUs < 0 && now - startedUs > ARM_LIMIT_US) {
            System.err.println("arming did not complete within 5 s");
            failed = true;
            done = true;
        }
        if (durationMs > 0 && now - startedUs >= durationMs * 1000) {
            done = true;
        }
    }
}
