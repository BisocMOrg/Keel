// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.task.RobotSession;

/** Interactive buttons using a managed session; B stops the finite drive and closes the example. */
public final class Buttons {
    /** Utility entry point only. */
    private Buttons() { }

    /** Parses bounded options, mirrors held buttons and optionally starts one finite drive.
     * @param args --simulator, --duration MS, or --drive LEFT RIGHT MS (duties remain permille for CLI compatibility)
     */
    public static void main(String[] args) {
        boolean simulator = false, drive = false;
        int left = 0, right = 0; long duration = 0, driveMs = 0;
        try {
            for (int i = 0; i < args.length; i++) {
                switch (args[i]) {
                    case "--simulator" -> simulator = true;
                    case "--duration" -> duration = bounded(args[++i], 1, 3_600_000);
                    case "--drive" -> { left = (int) bounded(args[++i], -1000, 1000); right = (int) bounded(args[++i], -1000, 1000); driveMs = bounded(args[++i], 1, 2000); drive = true; }
                    default -> throw new IllegalArgumentException("unknown option");
                }
            }
        } catch (RuntimeException invalid) { System.err.println("Usage: Buttons [--simulator] [--duration MS] [--drive LEFT RIGHT MS]"); System.exit(2); return; }
        try (RobotSession robot = RobotSession.open(OpenOptions.builder().backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build())) {
            try { robot.setRgbAll(0x000008); } catch (io.github.bisocmorg.keel.errors.UnsupportedException unavailable) { System.out.println("RGB LEDs unavailable; continuing without them"); }
            if (drive) { robot.arm(); robot.refreshDrive(left / 10.0, right / 10.0, driveMs); System.out.println("bounded drive started"); duration = duration > 0 ? Math.min(duration, driveMs + 100) : driveMs + 100; }
            long end = duration > 0 ? System.nanoTime() + duration * 1_000_000L : Long.MAX_VALUE;
            while (System.nanoTime() < end) {
                int held = robot.snapshot().buttonsHeldValue();
                int pressed = robot.buttonsPressed(), released = robot.buttonsReleased();
                if (pressed != 0 || released != 0) System.out.printf("buttons: held=%x pressed=%x released=%x%n", held, pressed, released);
                robot.setButtonLeds(Button.ALL.mask(), held);
                if (((held | pressed) & Button.B.mask()) != 0) break;
                Thread.sleep(10);
            }
            robot.disarm();
            System.out.println(drive ? "drive stopped; robot disarmed" : "buttons: done");
        } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); System.exit(130); }
        catch (RuntimeException failure) { System.err.println("buttons: " + failure.getMessage()); System.exit(1); }
    }

    /** Parses an integer within the CLI's explicit bounds. */
    private static long bounded(String text, long low, long high) {
        long value = Long.parseLong(text);
        if (value < low || value > high) throw new IllegalArgumentException("value outside bounds");
        return value;
    }
}
