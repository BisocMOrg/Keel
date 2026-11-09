// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.peripherals.RangeMeasurement;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.task.KeelTask;
import io.github.bisocmorg.keel.task.TaskOptions;

/** Reads bounded ultrasonic measurements in a sequential task, without a hardware control loop. */
public final class Ranging extends KeelTask {
    private final int count;

    /** Creates a task with a finite number of measurements. */
    private Ranging(int count) { this.count = count; }

    /** Reads typed range results; no motor arming is requested.
     * @throws Exception if a peripheral fails or the task is interrupted
     */
    @Override protected void run() throws Exception {
        if (!keel.capabilities().contains(Capability.RANGE)) throw new IllegalStateException("No ranging capability");
        if (keel.capabilities().contains(Capability.SIMULATED)) keel.simSet(SimItem.RANGE_MM, 500);
        for (int i = 1; i <= count; i++) {
            RangeMeasurement result = keel.measureDistance();
            System.out.println("range " + i + ": " + describe(result));
            Thread.sleep(Limits.RANGE_INTERVAL_MS + 10);
        }
        System.out.println("range_measure from the main thread: " + describe(keel.measureDistance()));
    }

    /** Runs the distance example; no motors are armed.
     * @param args --simulator or --count N (1..100)
     */
    public static void main(String[] args) {
        boolean simulator = false; int count = 5;
        try {
            for (int i = 0; i < args.length; i++) {
                if (args[i].equals("--simulator")) simulator = true;
                else if (args[i].equals("--count")) count = Integer.parseInt(args[++i]);
                else throw new IllegalArgumentException("unknown option");
            }
            if (count < 1 || count > 100) throw new IllegalArgumentException("count");
        } catch (RuntimeException invalid) { System.err.println("Usage: Ranging [--simulator] [--count N]"); System.exit(2); return; }
        try {
            Keel.run(new Ranging(count), TaskOptions.defaults().withOpenOptions(OpenOptions.builder()
                    .backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build()));
        } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); System.exit(130); }
        catch (Exception failure) { System.err.println("ranging: " + failure.getMessage()); System.exit(1); }
    }
    /** Formats valid distance separately from no-echo, invalid and unavailable results. */
    private static String describe(RangeMeasurement result) {
        return result.valid() ? "VALID " + result.distanceMm() + " mm" : result.status().name();
    }
}
