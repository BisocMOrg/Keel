// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.errors.AgainException;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.Cycle;
import io.github.bisocmorg.keel.peripherals.RangeMeasurement;
import io.github.bisocmorg.keel.peripherals.RangeStatus;
import io.github.bisocmorg.keel.peripherals.SensorPower;
import io.github.bisocmorg.keel.sim.SimItem;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Ranging: ultrasonic measurements the way a loop takes them, with {@code rangeStart()} and {@code rangePoll()} in
 * the step, then one {@code rangeMeasure()} from another thread while DISARMED.
 *
 * <p>Results are statuses, never sentinel distances: only {@link RangeStatus#VALID} carries a distance, and
 * {@link RangeStatus#NO_ECHO} means "nothing detected", never "clear". The sensor is powered by the safety MCU once
 * the heartbeat runs; nothing moves.
 *
 * <p>{@code --count N} sets the number of measurements the loop takes (1 to 100, default 5). On the simulator an object
 * is placed 500 mm ahead.
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/Ranging.java --simulator
 * </pre>
 *
 * <p>Exit status: 0 success, 1 failure, 2 usage, 128 + N after signal N (130 after Ctrl-C, 143 after SIGTERM).
 */
public final class Ranging {
    private static final int SIMULATED_DISTANCE_MM = 500;
    private static final long SPACING_US = (Limits.RANGE_INTERVAL_MS + 10) * 1000L;

    private final CountDownLatch measured = new CountDownLatch(1);
    private boolean simulator;
    private int measurements = 5;
    private volatile boolean ranging = true;
    private long lastSequence;
    private long nextStartUs;
    private int completed;

    private Ranging() {
    }

    /**
     * Runs the example.
     *
     * @param args {@code [--simulator] [--count N]}
     */
    public static void main(String[] args) {
        System.exit(new Ranging().run(args));
    }

    private int run(String[] args) {
        try {
            for (int i = 0; i < args.length; i++) {
                switch (args[i]) {
                    case "--simulator" -> simulator = true;
                    case "--count" -> {
                        measurements = Integer.parseInt(args[++i]);
                        if (measurements < 1 || measurements > 100) {
                            throw new IllegalArgumentException(args[i]);
                        }
                    }
                    default -> throw new IllegalArgumentException(args[i]);
                }
            }
        } catch (ArrayIndexOutOfBoundsException | IllegalArgumentException error) {
            System.err.println("Usage: Ranging [--simulator] [--count N]   (N: 1..100)");
            return 2;
        }
        OpenOptions options = OpenOptions.builder().backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build();
        try (Device device = Keel.open(options)) {
            if (!device.capabilities().contains(Capability.RANGE)) {
                System.err.println("ranging: this device has no ranging capability");
                return 1;
            }
            if (device.capabilities().contains(Capability.SIMULATED)) {
                device.simSet(SimItem.RANGE_MM, SIMULATED_DISTANCE_MM);
            }
            try (ControlLoop loop = ControlLoop.builder(device, this::step).build()) {
                loop.start();
                if (!measured.await(10 + measurements, TimeUnit.SECONDS)) {
                    System.err.println("ranging: the measurements did not complete in time");
                    return 1;
                }
                Thread.sleep(Limits.RANGE_INTERVAL_MS + 10);
                // The main thread is not the control owner, so the blocking form is allowed here while DISARMED.
                System.out.println("range_measure from the main thread: " + describe(device.rangeMeasure()));
            }
            return 0;
        } catch (InterruptedException interruption) {
            Thread.currentThread().interrupt();
            return 130;
        } catch (LibraryException | KeelException error) {
            System.err.println("ranging: " + error.getMessage());
            return 1;
        }
    }

    private void step(Cycle cycle) {
        if (!ranging) {
            return;
        }
        SensorPower power = cycle.snapshot().sensorPower();
        if (power != SensorPower.PERMITTED && power != SensorPower.CONFIRMED) {
            return; // the heartbeat has not qualified sensor power yet
        }
        Device device = cycle.device();
        RangeMeasurement result = device.rangePoll();
        if (result.sequence() != lastSequence && result.status() != RangeStatus.PENDING) {
            lastSequence = result.sequence();
            System.out.println("range " + result.sequence() + ": " + describe(result));
            if (++completed == measurements) {
                ranging = false;
                measured.countDown();
                return;
            }
        }
        if (result.status() != RangeStatus.PENDING && cycle.nowUs() >= nextStartUs) {
            try {
                device.rangeStart();
                nextStartUs = cycle.nowUs() + SPACING_US;
            } catch (AgainException spacing) {
                nextStartUs = cycle.nowUs() + Limits.TICK_PERIOD_MS * 1000L;
            }
        }
    }

    private static String describe(RangeMeasurement result) {
        return switch (result.status()) {
            case VALID -> "VALID " + result.distanceMm() + " mm";
            case NO_ECHO -> "NO_ECHO (nothing detected; never \"clear\")";
            case UNAVAILABLE -> "UNAVAILABLE (sensor unpowered or locked out)";
            default -> result.status().name();
        };
    }
}
