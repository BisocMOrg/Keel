// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import io.github.bisocmorg.keel.support.CallRecorder;
import io.github.bisocmorg.keel.support.CameraSourceExtension;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;

/** The task facade through real native simulator calls and the synthetic camera backend. */
class KeelTaskIntegrationTest {
    /** Exercises the student peripheral methods without ever requesting arming. */
    @Test void peripheralTasksDoNotNeedAControllerOrArming() throws Exception {
        KeelTask task = new KeelTask() {
            @Override protected void run() throws Exception {
                assertTrue(keel.capabilities().contains(Capability.SIMULATED));
                assertNotNull(keel.report());
                keel.setRgb(0, 0x120000);
                keel.setRgbAll(0x001200);
                keel.setRgbFrame(1, 2, 3, 4, 5, 6);
                keel.setButtonLeds(15, 5);
                keel.simSet(SimItem.RANGE_MM, 500);
                assertEquals(500, keel.measureDistance().distanceMm());
                assertNotNull(keel.snapshot());
                assertTrue(keel.stats().cycles() > 0);
            }
        };
        try (CallRecorder recorder = new CallRecorder()) {
            Keel.run(task, simulator());
            assertEquals(0, recorder.calls.stream().filter("keel_request_arm"::equals).count());
        }
    }
    /** Uses the simulator's real-time clock, keeping hardware physically inaccessible to these tests. */
    private static TaskOptions simulator() {
        return TaskOptions.defaults().withOpenOptions(OpenOptions.builder().backend(Backend.SIMULATOR).build());
    }

    /** Runs the ordinary student sequence and confirms one automatic arming and complete resource cleanup. */
    @Test void executesSequentialMovesAndClosesItsScope() throws Exception {
        AtomicReference<TaskRobot> retained = new AtomicReference<>();
        Thread calling = Thread.currentThread();
        KeelTask task = new KeelTask() {
            /** Executes every student statement on the caller's platform thread. */
            @Override protected void run() throws Exception {
                assertSame(calling, Thread.currentThread());
                retained.set(keel);
                keel.driveFor(20, 20, 30);
                Thread.sleep(20);
                keel.turnRightFor(30);
            }
        };
        try (CallRecorder recorder = new CallRecorder()) {
            Keel.run(task, simulator());
            assertEquals(1, recorder.calls.stream().filter("keel_request_arm"::equals).count());
            assertTrue(recorder.calls.contains("keel_close"));
        }
        assertNull(task.keel);
        assertEquals(TaskException.Reason.CLOSED,
                assertThrows(TaskException.class, () -> retained.get().driveFor(20, 20, 10)).reason());
    }

    /** Preserves a student's checked exception and still closes the underlying board. */
    @Test void studentExceptionClosesResourcesAndIsPreserved() {
        Exception expected = new Exception("student analysis failed");
        KeelTask task = new KeelTask() {
            /** Deliberately throws a checked student failure. */
            @Override protected void run() throws Exception { throw expected; }
        };
        try (CallRecorder recorder = new CallRecorder()) {
            assertSame(expected, assertThrows(Exception.class, () -> Keel.run(task, simulator())));
            assertTrue(recorder.calls.contains("keel_disarm"));
            assertTrue(recorder.calls.contains("keel_close"));
        }
        assertNull(task.keel);
    }

    /** Rejects reusing a task instance before a second native board can be opened. */
    @Test void taskInstancesAreSingleUse() throws Exception {
        KeelTask task = new KeelTask() {
            /** Represents an empty student task. */
            @Override protected void run() { }
        };
        Keel.run(task, simulator());
        assertThrows(IllegalStateException.class, () -> Keel.run(task, simulator()));
    }

    /** An ordinary sleep does not extend the independently enforced task lifetime. */
    @Test void totalTimeoutIsReportedAfterStudentCodeReturns() {
        TaskOptions d = simulator();
        TaskOptions bounded = new TaskOptions(d.openOptions(), d.cameraOptions(), d.maxMotionMs(),
                d.armTimeoutMs(), 50, d.cameraTimeoutMs());
        KeelTask task = new KeelTask() {
            /** Waits beyond the task's configured hardware-authority lifetime. */
            @Override protected void run() throws Exception { Thread.sleep(100); }
        };
        assertEquals(TaskException.Reason.TASK_TIMEOUT,
                assertThrows(TaskException.class, () -> Keel.run(task, bounded)).reason());
    }

    /** Reads actual synthetic frames without requiring the student to initialize or close the camera. */
    @Test
    @ExtendWith(CameraSourceExtension.class)
    void imagesUseBgrAndOutliveTheTaskScope() throws Exception {
        AtomicReference<Frame> frame = new AtomicReference<>();
        KeelTask task = new KeelTask() {
            /** Takes two frames from the task-owned camera. */
            @Override protected void run() {
                frame.set(keel.takeImage());
                assertNotNull(keel.takeImage());
            }
        };
        try (CallRecorder recorder = new CallRecorder()) {
            Keel.run(task, simulator());
            assertEquals(0, recorder.calls.stream().filter("keel_request_arm"::equals).count());
        }
        assertEquals(PixelFormat.BGR888, frame.get().pixelFormat());
        assertTrue(frame.get().toByteArray().length > 0);
    }
}
