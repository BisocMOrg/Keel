// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.OpenFlag;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.state.State;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;

/** Exercises task sequencing, terminal failures and independently enforced motor deadlines without hardware. */
class TaskRobotTest {
    /** Drives twice, requiring a completed native zero-drive observation between the two commands. */
    @Test void consecutiveActionsWaitForAcknowledgedZeroAndArmOnlyOnce() throws Exception {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, TaskOptions.defaults())) {
            robot.driveFor(20, -15.5, 12);
            assertTrue(hardware.stationary());
            assertTrue(hardware.lastObserved >= hardware.firstCoastAfterDrive);
            robot.turnLeftFor(12);
            assertTrue(hardware.stationary());
            assertEquals(1, hardware.armCalls);
            assertTrue(hardware.commands.contains("200:-155"));
            assertTrue(hardware.commands.contains("-200:200"));
        }
        assertTrue(hardware.closed);
    }

    /** Validates units and rejects invalid input before any command can arm a device. */
    @Test void invalidInputsDoNotArmAndZeroDutyRemainsDisarmed() throws Exception {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, TaskOptions.defaults())) {
            assertThrows(IllegalArgumentException.class, () -> robot.driveFor(Double.NaN, 0, 10));
            assertThrows(IllegalArgumentException.class, () -> robot.driveFor(101, 0, 10));
            assertThrows(IllegalArgumentException.class, () -> robot.driveFor(0, Double.POSITIVE_INFINITY, 10));
            assertThrows(IllegalArgumentException.class, () -> robot.driveFor(20, 20, -1));
            assertThrows(IllegalArgumentException.class, () -> robot.driveFor(20, 20, 30_001));
            robot.driveFor(20, 20, 0);
            robot.driveFor(0, 0, 8);
            assertEquals(0, hardware.armCalls);
            assertTrue(hardware.commands.isEmpty());
        }
    }

    /** An uncompleted arming handshake terminates rather than moving or trying again. */
    @Test void armingTimeoutIsTerminal() {
        FakeHardware hardware = new FakeHardware();
        hardware.completeArming = false;
        try (TaskRobot robot = robot(hardware, options(20, 1_000))) {
            TaskException error = assertThrows(TaskException.class, () -> robot.driveFor(10, 10, 10));
            assertEquals(TaskException.Reason.ARM_TIMEOUT, error.reason());
            assertThrows(TaskException.class, () -> robot.driveFor(10, 10, 10));
            assertEquals(1, hardware.armCalls);
            assertTrue(hardware.stopped);
            assertTrue(hardware.commands.isEmpty());
        }
    }

    /** The controller expires the task even while arbitrary student code is asleep. */
    @Test void wholeTaskDeadlineStopsHardwareWithoutCooperationFromStudentCode() throws Exception {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, options(20, 40))) {
            assertTrue(hardware.stopSeen.await(2, TimeUnit.SECONDS));
            TaskException error = assertThrows(TaskException.class, robot::verifyCompletion);
            assertEquals(TaskException.Reason.TASK_TIMEOUT, error.reason());
            assertTrue(hardware.stopped);
            assertThrows(TaskException.class, () -> robot.driveFor(20, 20, 1));
        }
    }

    /** A stop request invalidates a currently executing bounded action and every later action. */
    @Test void externalStopCannotBeOverwrittenByTheActiveAction() throws Exception {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, TaskOptions.defaults())) {
            Thread cancel = Thread.ofPlatform().daemon(true).start(() -> {
                awaitLatch(hardware.driveSeen);
                robot.stop();
            });
            TaskException error = assertThrows(TaskException.class, () -> robot.driveFor(20, 20, 5_000));
            cancel.join(2_000);
            assertEquals(TaskException.Reason.CANCELLED, error.reason());
            int commands = hardware.commands.size();
            Thread.sleep(15);
            assertEquals(commands, hardware.commands.size());
            assertTrue(hardware.stationary());
            assertThrows(TaskException.class, () -> robot.driveFor(20, 20, 10));
        }
    }

    /** A recorded native-loop failure cannot strand a blocked action or silently resume. */
    @Test void nativeLoopFailureStopsAnAction() throws Exception {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, TaskOptions.defaults())) {
            Thread fail = Thread.ofPlatform().daemon(true).start(() -> {
                awaitLatch(hardware.driveSeen);
                hardware.error = new IllegalStateException("native failure");
            });
            TaskException error = assertThrows(TaskException.class, () -> robot.driveFor(20, 20, 5_000));
            fail.join(2_000);
            assertEquals(TaskException.Reason.LOOP_FAILED, error.reason());
            assertNotNull(error.getCause());
            assertTrue(hardware.stopped);
        }
    }

    /** A board fault is terminal and the facade never sends a second arm request. */
    @Test void boardFaultDoesNotAutomaticallyRearm() throws Exception {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, TaskOptions.defaults())) {
            robot.driveFor(20, 20, 8);
            hardware.state = State.FAULTED;
            assertTrue(hardware.stopSeen.await(2, TimeUnit.SECONDS));
            TaskException error = assertThrows(TaskException.class, () -> robot.driveFor(20, 20, 10));
            assertEquals(TaskException.Reason.FAULT, error.reason());
            assertEquals(1, hardware.armCalls);
        }
    }

    /** Taking images is task-thread work; a capture failure ends authority without arming motors. */
    @Test void cameraFailureIsTerminalAndCameraIsLazy() {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, TaskOptions.defaults())) {
            assertEquals(0, hardware.imageCalls);
            TaskException error = assertThrows(TaskException.class, robot::takeImage);
            assertEquals(TaskException.Reason.CAMERA_FAILED, error.reason());
            assertEquals(1, hardware.imageCalls);
            assertEquals(0, hardware.armCalls);
            assertTrue(hardware.stopped);
        }
    }

    /** A native stop failure remains visible rather than being reported as a successful cancellation. */
    @Test void stopFailureIsReportedButCannotRestoreAuthority() {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, TaskOptions.defaults())) {
            hardware.stopFailure = new IllegalStateException("stop not confirmed");
            TaskException error = assertThrows(TaskException.class, robot::stop);
            assertEquals(TaskException.Reason.CANCELLED, error.reason());
            assertEquals(1, error.getSuppressed().length);
            assertThrows(TaskException.class, () -> robot.driveFor(20, 20, 10));
            hardware.stopFailure = null;
        }
    }

    /** Delayed camera work cannot return usable authority after the controller has timed out the task. */
    @Test void lateCameraResultCannotReviveExpiredTask() {
        FakeHardware hardware = new FakeHardware();
        hardware.slowImage = true;
        try (TaskRobot robot = robot(hardware, options(20, 25))) {
            TaskException error = assertThrows(TaskException.class, robot::takeImage);
            assertEquals(TaskException.Reason.TASK_TIMEOUT, error.reason());
            assertTrue(hardware.stopped);
            assertEquals(0, hardware.armCalls);
        }
    }

    /** Cancellation can occur from another thread, but resource teardown retains camera-owner affinity. */
    @Test void closeMustUseTheOwningTaskThread() throws Exception {
        FakeHardware hardware = new FakeHardware();
        TaskRobot robot = robot(hardware, TaskOptions.defaults());
        try {
            List<Throwable> errors = new CopyOnWriteArrayList<>();
            Thread helper = Thread.ofPlatform().start(() -> {
                try { robot.close(); } catch (Throwable error) { errors.add(error); }
            });
            helper.join();
            assertEquals(1, errors.size());
            assertTrue(errors.getFirst() instanceof IllegalStateException);
            assertFalse(hardware.closed);
        } finally {
            robot.close();
        }
    }

    /** Explicit close is idempotent and retained robot references cannot be used afterwards. */
    @Test void closingRetainedFacadeRejectsFurtherOperations() {
        FakeHardware hardware = new FakeHardware();
        TaskRobot robot = robot(hardware, TaskOptions.defaults());
        robot.close();
        robot.close();
        assertEquals(1, hardware.closeCalls);
        assertEquals(TaskException.Reason.CLOSED,
                assertThrows(TaskException.class, robot::takeImage).reason());
    }

    /** A helper thread cannot call movement or camera methods on behalf of the task. */
    @Test void operationThreadAffinityIsEnforced() throws Exception {
        FakeHardware hardware = new FakeHardware();
        try (TaskRobot robot = robot(hardware, TaskOptions.defaults())) {
            List<Throwable> errors = new CopyOnWriteArrayList<>();
            Thread helper = Thread.ofPlatform().start(() -> {
                try { robot.driveFor(20, 20, 10); } catch (Throwable error) { errors.add(error); }
            });
            helper.join();
            assertEquals(1, errors.size());
            assertTrue(errors.getFirst() instanceof IllegalStateException);
            assertEquals(0, hardware.armCalls);
            assertFalse(hardware.stopped);
        }
    }

    /** Millisecond limits must fit the monotonic deadline arithmetic and cannot select a virtual clock. */
    @Test void optionsRejectInvalidLimitsAndVirtualClock() {
        TaskOptions defaults = TaskOptions.defaults();
        assertThrows(IllegalArgumentException.class, () -> options(0, 100));
        assertThrows(IllegalArgumentException.class, () -> options(3_000, Long.MAX_VALUE));
        assertThrows(IllegalArgumentException.class, () -> defaults.withOpenOptions(OpenOptions.builder()
                .backend(Backend.SIMULATOR).flags(OpenFlag.VIRTUAL_CLOCK).build()));
    }

    /** Constructs an injected facade that exercises the same coordinator as native tasks. */
    private static TaskRobot robot(FakeHardware hardware, TaskOptions options) {
        return new TaskRobot(options, hardware, System::nanoTime);
    }

    /** Uses brief arming and task bounds while retaining the ordinary motion and camera defaults. */
    private static TaskOptions options(long armMs, long taskMs) {
        TaskOptions d = TaskOptions.defaults();
        return new TaskOptions(d.openOptions(), d.cameraOptions(), d.maxMotionMs(), armMs, taskMs, d.cameraTimeoutMs());
    }

    /** Waits a bounded time from the helper threads used to inject a fault or cancellation. */
    private static void awaitLatch(CountDownLatch latch) {
        try {
            if (!latch.await(2, TimeUnit.SECONDS)) throw new AssertionError("drive was not observed");
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
    }

    /** Models the preceding-cycle snapshot separately from the command being issued this cycle. */
    private static final class FakeHardware implements TaskHardware, TaskHardware.Tick {
        private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor(
                Thread.ofPlatform().daemon(true).factory());
        private final List<String> commands = new CopyOnWriteArrayList<>();
        private final CountDownLatch driveSeen = new CountDownLatch(1);
        private final CountDownLatch stopSeen = new CountDownLatch(1);
        private volatile State state = State.DISARMED;
        private volatile Throwable error;
        private volatile boolean stopped;
        private volatile boolean closed;
        private volatile boolean completeArming = true;
        private volatile boolean slowImage;
        private volatile RuntimeException stopFailure;
        private volatile int armCalls;
        private volatile int imageCalls;
        private volatile int closeCalls;
        private volatile long lastObserved;
        private volatile long firstCoastAfterDrive;
        private long cycle;
        private int left;
        private int right;
        private int nextLeft;
        private int nextRight;

        /** Starts a fast model clock; no native library is loaded. */
        @Override public void start(Consumer<Tick> step) {
            scheduler.scheduleAtFixedRate(() -> {
                if (stopped) return;
                cycle++;
                step.accept(this);
                left = nextLeft;
                right = nextRight;
                lastObserved = cycle;
            }, 0, 2, TimeUnit.MILLISECONDS);
        }
        /** Immediately drops model motor outputs. */
        @Override public void stop() {
            stopped = true; left = right = nextLeft = nextRight = 0; stopSeen.countDown();
            if (stopFailure != null) throw stopFailure;
        }
        /** Exposes a failure injected by a test. */
        @Override public Throwable failure() { return error; }
        /** A stopped model corresponds to an ended controller. */
        @Override public boolean ended() { return stopped; }
        /** Proves camera work occurs only on explicit request. */
        @Override public Frame image(long timeoutMs) {
            imageCalls++;
            if (slowImage) {
                try { Thread.sleep(60); } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
                return null;
            }
            throw new IllegalStateException("camera unavailable");
        }
        /** Cancels the model thread and records resource cleanup. */
        @Override public void close() { closeCalls++; closed = true; stop(); scheduler.shutdownNow(); }
        /** Returns the preceding-cycle state. */
        @Override public State state() { return state; }
        /** Identifies the next native command transaction. */
        @Override public long cycleId() { return cycle; }
        /** Identifies the last completed native transaction. */
        @Override public long observedCycleId() { return lastObserved; }
        /** Checks actual modeled outputs, not merely the queued coast. */
        @Override public boolean stationary() { return left == 0 && right == 0; }
        /** Completes or withholds the simulated handshake according to the test. */
        @Override public void arm() { armCalls++; if (completeArming) state = State.ARMED; }
        /** Stages a motor command for the end of this transaction. */
        @Override public void drive(int l, int r) { nextLeft = l; nextRight = r; commands.add(l + ":" + r); driveSeen.countDown(); }
        /** Stages coast and records its cycle for the acknowledgement assertion. */
        @Override public void coast() {
            if ((left != 0 || right != 0) && firstCoastAfterDrive == 0) firstCoastAfterDrive = cycle;
            nextLeft = nextRight = 0;
        }
    }
}
