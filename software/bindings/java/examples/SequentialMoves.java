// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.task.KeelTask;
import io.github.bisocmorg.keel.task.TaskOptions;

/**
 * A student's sequential task: bounded drive, zero-drive pause, bounded turn and another drive.
 * No ARMED checks or control-loop code are needed. On physical hardware, lift the wheels clear for the first run.
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/SequentialMoves.java --simulator
 * </pre>
 */
public final class SequentialMoves extends KeelTask {
    /** Creates an unused example task. */
    public SequentialMoves() { }

    /**
     * Runs the student's sequence; each movement has ended with zero drive before the next statement starts.
     *
     * @throws Exception if interrupted or any task/hardware operation fails
     */
    @Override
    protected void run() throws Exception {
        keel.driveFor(20, 20, 500);
        Thread.sleep(500);
        keel.turnLeftFor(300);
        keel.driveFor(15, 15, 300);
    }

    /**
     * Runs the task with an explicit simulator or physical-motion choice. Missing permission exits before
     * loading native code or opening hardware.
     *
     * @param args --simulator for no hardware, or --allow-motion for the real robot
     * @throws Exception if opening or executing the task fails
     */
    public static void main(String[] args) throws Exception {
        System.exit(executeExample(args));
    }

    /** Parses the explicit motion choice before running any native code; usage errors return status 2. */
    private static int executeExample(String[] args) throws Exception {
        boolean simulator = false;
        boolean allowMotion = false;
        for (String arg : args) {
            if (arg.equals("--simulator")) simulator = true;
            else if (arg.equals("--allow-motion")) allowMotion = true;
            else if (arg.equals("--help")) {
                System.out.println("Usage: SequentialMoves --simulator | --allow-motion");
                return 0;
            }
            else {
                System.err.println("Usage: SequentialMoves --simulator | --allow-motion");
                return 2;
            }
        }
        if (!simulator && !allowMotion) {
            System.err.println("Physical movement requires --allow-motion; use --simulator to run without hardware.");
            return 2;
        }
        TaskOptions options = TaskOptions.defaults().withOpenOptions(OpenOptions.builder()
                .backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build());
        Keel.run(new SequentialMoves(), options);
        return 0;
    }
}
