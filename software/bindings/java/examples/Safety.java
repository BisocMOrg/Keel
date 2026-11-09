// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.errors.ConflictException;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.errors.StateException;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.SnapshotFlag;
import io.github.bisocmorg.keel.state.State;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Safety: the stop paths and the thread rule, without motion.
 *
 * <ol>
 * <li>The loop runs on its own thread and owns the device; a go call ({@code brake()}) from the main thread is
 * refused with {@link ConflictException}.</li>
 * <li>A user-interface thread presses the emergency stop; the loop sees FAULTED and reports the fault record.</li>
 * <li>The fault is cleared through the loop, on the owner thread.</li>
 * <li>A request to arm within 1 s of the latch is refused with {@link StateException} (the re-arm hold-off). This
 * step runs only on the simulator or with {@code --allow-motion}, because once the hold-off has passed the same call
 * would arm.</li>
 * </ol>
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/Safety.java --simulator
 * </pre>
 *
 * <p>Exit status: 0 success, 1 failure, 2 usage, 128 + N after signal N (130 after Ctrl-C, 143 after SIGTERM).
 */
public final class Safety {
    private Safety() {
    }

    /**
     * Whether this program may ask to arm: always on the simulator, otherwise only when the operator asked.
     *
     * @param capabilities the device's capabilities
     * @param allowMotion whether {@code --allow-motion} was given
     * @return {@code true} if a request to arm is allowed
     */
    static boolean motionAllowed(Set<Capability> capabilities, boolean allowMotion) {
        return capabilities.contains(Capability.SIMULATED) || allowMotion;
    }

    /**
     * Runs the example.
     *
     * @param args {@code [--simulator] [--allow-motion]}
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
                default -> {
                    System.err.println("Usage: Safety [--simulator] [--allow-motion]");
                    return 2;
                }
            }
        }
        OpenOptions options = OpenOptions.builder().backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build();
        CountDownLatch faulted = new CountDownLatch(1);
        AtomicReference<Fault> fault = new AtomicReference<>();
        try (Device device = Keel.open(options);
                ControlLoop loop = ControlLoop.builder(device, null).onFault(record -> {
                    fault.set(record);
                    faulted.countDown();
                }).build()) {
            loop.start();
            awaitHeartbeat(loop);
            System.out.println("heartbeat running on the loop thread");

            try {
                device.brake(); // a go call from this thread, which is not the control owner
                System.err.println("a go call from a foreign thread was accepted");
                return 1;
            } catch (ConflictException expected) {
                System.out.println("foreign go call refused: " + expected.name());
            }

            Thread ui = Thread.ofPlatform().name("user-interface").start(device::emergencyStop);
            ui.join();
            if (!faulted.await(5, TimeUnit.SECONDS)) {
                System.err.println("the loop did not report the emergency stop");
                return 1;
            }
            Fault record = fault.get();
            System.out.println("emergency stop: " + record.status() + ", cause " + record.cause() + ", in state "
                    + record.state());

            loop.clearFault().get(5, TimeUnit.SECONDS);
            System.out.println("fault cleared through the loop: " + device.state());

            if (motionAllowed(device.capabilities(), allowMotion)) {
                try {
                    loop.requestArm().get(5, TimeUnit.SECONDS);
                    System.err.println("arming was accepted during the re-arm hold-off");
                    loop.stop();
                    return 1;
                } catch (ExecutionException refused) {
                    if (!(refused.getCause() instanceof StateException holdoff)) {
                        throw refused;
                    }
                    System.out.println("re-arm refused during the hold-off: " + holdoff.name());
                }
            } else {
                System.out.println("skipping the re-arm hold-off step: it needs --allow-motion on a real robot");
            }
            return device.state() == State.DISARMED ? 0 : 1;
        } catch (InterruptedException interruption) {
            Thread.currentThread().interrupt();
            return 130;
        } catch (ExecutionException | TimeoutException error) {
            System.err.println("safety: " + error);
            return 1;
        } catch (LibraryException | KeelException error) {
            System.err.println("safety: " + error.getMessage());
            return 1;
        }
    }

    private static void awaitHeartbeat(ControlLoop loop) throws InterruptedException {
        for (int i = 0; i < 500 && !loop.snapshot().flags().contains(SnapshotFlag.HEARTBEAT_RUNNING); i++) {
            Thread.sleep(10);
        }
    }
}
