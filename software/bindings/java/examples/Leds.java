// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.task.RobotSession;

/** Walks RGB and indicator lights, then demonstrates that they remain usable after an emergency stop. */
public final class Leds {
    private static final int[] PALETTE = {0x200000, 0x201000, 0x202000, 0x002000, 0x000020, 0x100020};
    /** Utility entry point only. */
    private Leds() { }

    /** Runs finite lighting actions; no motors are armed.
     * @param args --simulator or --duration MS (at least 200)
     */
    public static void main(String[] args) {
        boolean simulator = false; long duration = 3000;
        try {
            for (int i = 0; i < args.length; i++) {
                if (args[i].equals("--simulator")) simulator = true;
                else if (args[i].equals("--duration")) duration = Long.parseLong(args[++i]);
                else throw new IllegalArgumentException("unknown option");
            }
            if (duration < 200 || duration > 3_600_000) throw new IllegalArgumentException("duration outside 200..3600000 ms");
        } catch (RuntimeException invalid) { System.err.println("Usage: Leds [--simulator] [--duration MS]"); System.exit(2); return; }
        try (RobotSession robot = RobotSession.open(OpenOptions.builder().backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build())) {
            boolean rgb = robot.capabilities().contains(Capability.RGB);
            long end = System.nanoTime() + duration * 1_000_000L;
            long stopAt = System.nanoTime() + duration * 500_000L;
            int offset = 0;
            while (System.nanoTime() < stopAt) {
                int[] frame = new int[6];
                for (int i = 0; i < frame.length; i++) frame[i] = PALETTE[(i + offset) % frame.length];
                if (rgb) robot.setRgbFrame(frame);
                robot.setButtonLeds(Button.ALL.mask(), 1 << (offset % 4));
                if (offset++ == 0) System.out.println("frame: " + (rgb ? "RGB1..RGB6 walking" : "skipped"));
                Thread.sleep(100);
            }
            robot.emergencyStop();
            if (rgb) robot.setRgbAll(0x400000);
            robot.setButtonLeds(Button.ALL.mask(), Button.ALL.mask());
            System.out.println("state: FAULTED; error colour shown");
            while (System.nanoTime() < end) Thread.sleep(20);
            if (rgb) robot.setRgbAll(0);
            robot.setButtonLeds(Button.ALL.mask(), 0);
            System.out.println("LEDs off");
        } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); System.exit(130); }
        catch (RuntimeException failure) { System.err.println("leds: " + failure.getMessage()); System.exit(1); }
    }
}
