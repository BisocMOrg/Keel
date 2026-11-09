// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.task.KeelTask;
import io.github.bisocmorg.keel.task.TaskOptions;
import java.util.Set;

/** A sequential task: automatically arm, drive at 20 percent for two seconds, stop and close. */
public final class Drive extends KeelTask {
    private final boolean allowMotion;

    /** Stores the operator's explicit hardware-motion permission. */
    private Drive(boolean allowMotion) { this.allowMotion = allowMotion; }

    /** Checks the simulator capability or explicit physical-motion permission.
     * @param capabilities board capabilities
     * @param allowMotion explicit permission for physical motion
     * @return true when this example may move
     */
    static boolean motionAllowed(Set<Capability> capabilities, boolean allowMotion) {
        return capabilities.contains(Capability.SIMULATED) || allowMotion;
    }

    /** Runs the bounded student action without state polling or a control loop.
     * @throws Exception if arming, motion or cleanup fails
     */
    @Override protected void run() throws Exception {
        System.out.println("opened " + keel.report().backend() + ": " + Keel.versionString());
        if (!motionAllowed(keel.capabilities(), allowMotion)) throw new IllegalArgumentException("motion on a real robot needs --allow-motion");
        System.out.println("arming automatically for bounded drive");
        keel.driveFor(20, 20, 2000);
        System.out.println("drove 2000 ms at 20 percent; output " + keel.snapshot().outputLeft() + "/" + keel.snapshot().outputRight());
    }

    /** Parses --simulator or --allow-motion and executes the task.
     * @param args example options; --help prints usage
     */
    public static void main(String[] args) {
        boolean simulator = false, allowMotion = false;
        for (String arg : args) {
            if (arg.equals("--simulator")) simulator = true;
            else if (arg.equals("--allow-motion")) allowMotion = true;
            else if (arg.equals("--help")) { System.out.println("Usage: Drive [--simulator] [--allow-motion]"); return; }
            else { System.err.println("Usage: Drive [--simulator] [--allow-motion]"); System.exit(2); return; }
        }
        if (!simulator && !allowMotion) { System.err.println("drive: motion on a real robot needs --allow-motion"); System.exit(2); return; }
        try {
            Keel.run(new Drive(allowMotion), TaskOptions.defaults().withOpenOptions(OpenOptions.builder()
                    .backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build()));
            System.out.println("task complete; robot disarmed and closed");
        } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); System.exit(130); }
        catch (Exception failure) { System.err.println("drive: " + failure.getMessage()); System.exit(1); }
    }
}
