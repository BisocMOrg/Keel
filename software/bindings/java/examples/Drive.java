// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.state.State;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Drive: open the device, arm, drive forward at 200 permille for about 2 s, disarm and close.
 *
 * <p>The loop runs on the main thread (a platform thread), which is the control owner: before the first tick every
 * thread is the owner, so {@code requestArm()} from here is allowed. On a real robot, arming needs
 * {@code --allow-motion}; the simulator needs nothing.
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/Drive.java --simulator
 * </pre>
 *
 * <p>Exit status: 0 success, 1 failure, 2 usage (or motion not allowed), 128 + N after signal N (130 after Ctrl-C,
 * 143 after SIGTERM). On SIGINT, SIGTERM or SIGHUP the binding's shutdown hook makes an emergency stop and closes the
 * device.
 */
public final class Drive {
    private static final int DUTY = 200;
    private static final int DRIVE_CYCLES = 200;

    private Drive() {
    }

    /**
     * Whether this program may arm the device: always on the simulator, otherwise only when the operator asked.
     *
     * @param capabilities the device's capabilities
     * @param allowMotion whether {@code --allow-motion} was given
     * @return {@code true} if arming is allowed
     */
    static boolean motionAllowed(Set<Capability> capabilities, boolean allowMotion) {
        return capabilities.contains(Capability.SIMULATED) || allowMotion;
    }

    /**
     * Runs the example.
     *
     * @param args {@code --simulator}, {@code --allow-motion}, {@code --help}
     */
    public static void main(String[] args) {
        System.exit(run(args));
    }

    private static int run(String[] args) {
        boolean simulator = false;
        boolean allowMotion = false;
        for (String arg : args) {
            switch (arg) {
                case "--simulator" -> simulator = true;
                case "--allow-motion" -> allowMotion = true;
                case "--help" -> {
                    System.out.println("Usage: Drive [--simulator] [--allow-motion]");
                    return 0;
                }
                default -> {
                    System.err.println("Usage: Drive [--simulator] [--allow-motion]");
                    return 2;
                }
            }
        }
        OpenOptions options = OpenOptions.builder().backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build();
        try (Device device = Keel.open(options)) {
            System.out.println("opened " + device.report().backend() + ": " + Keel.versionString());
            if (!motionAllowed(device.capabilities(), allowMotion)) {
                System.err.println("drive: motion on a real robot needs --allow-motion");
                return 2;
            }
            AtomicInteger driven = new AtomicInteger();
            ControlLoop loop = ControlLoop.builder(device, cycle -> {
                if (cycle.snapshot().state() == State.ARMED) {
                    cycle.drive(DUTY, DUTY); // permille, sent again every cycle
                    driven.incrementAndGet();
                }
            }).build();
            device.requestArm();
            System.out.println("arming");
            Snapshot last = loop.runUntil(snapshot -> driven.get() >= DRIVE_CYCLES
                    || snapshot.state() == State.FAULTED, 500);
            if (last.state() == State.FAULTED) {
                Fault fault = device.fault();
                System.err.println("stopped: " + fault.status() + ", cause " + fault.cause());
                return 1;
            }
            if (driven.get() < DRIVE_CYCLES) {
                System.err.println("arming did not complete within 5 s");
                return 1;
            }
            System.out.println("state: ARMED; drove " + driven.get() + " cycles at " + DUTY + " permille, output "
                    + last.outputLeft() + "/" + last.outputRight());
            device.disarm();
            System.out.println("state: " + device.state());
            return 0;
        } catch (LibraryException | KeelException error) {
            System.err.println("drive: " + error.getMessage());
            return 1;
        }
    }
}
