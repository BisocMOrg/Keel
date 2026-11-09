// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.task.RobotSession;
import java.util.Set;

/** Demonstrates explicit arming, a latched emergency stop, and deliberate fault recovery in a session. */
public final class Safety {
    /** Utility entry point only. */
    private Safety() { }
    /** Checks whether this example may arm.
     * @param capabilities supported board capabilities
     * @param allowMotion explicit physical-motion permission
     * @return whether arming is permitted
     */
    static boolean motionAllowed(Set<Capability> capabilities, boolean allowMotion) {
        return capabilities.contains(Capability.SIMULATED) || allowMotion;
    }
    /** Runs safety diagnostics without requesting nonzero motor output.
     * @param args --simulator, --allow-motion, or --help
     */
    public static void main(String[] args) {
        boolean simulator = false, allowMotion = false;
        for (String arg : args) {
            if (arg.equals("--simulator")) simulator = true;
            else if (arg.equals("--allow-motion")) allowMotion = true;
            else if (arg.equals("--help")) { System.out.println("Usage: Safety [--simulator] [--allow-motion]"); return; }
            else { System.err.println("Usage: Safety [--simulator] [--allow-motion]"); System.exit(2); return; }
        }
        if (!simulator && !allowMotion) { System.err.println("safety: physical arming requires --allow-motion"); System.exit(2); return; }
        try (RobotSession robot = RobotSession.open(OpenOptions.builder().backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build())) {
            if (!motionAllowed(robot.capabilities(), allowMotion)) throw new IllegalStateException("Arming permission required");
            robot.arm();
            System.out.println("managed session armed; motors remain at zero");
            robot.emergencyStop();
            var fault = robot.fault();
            System.out.println("emergency stop: " + fault.status() + ", cause " + fault.cause());
            robot.clearFault();
            System.out.println("explicit fault recovery completed; rearming remains an explicit action");
            robot.disarm();
        } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); System.exit(130); }
        catch (RuntimeException failure) { System.err.println("safety: " + failure.getMessage()); System.exit(1); }
    }
}
