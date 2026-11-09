// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.OpenReport;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.Cycle;
import io.github.bisocmorg.keel.loop.LoopStats;
import io.github.bisocmorg.keel.peripherals.RangeMeasurement;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.peripherals.SensorPower;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.errors.AgainException;
import io.github.bisocmorg.keel.state.State;
import java.time.Duration;
import java.util.function.Consumer;
import java.util.Set;

/** Adapts the existing native board, camera and control loop to a scoped task. */
final class NativeTaskHardware implements TaskHardware {
    private final TaskOptions options;
    private final Device device;
    private ControlLoop loop;
    private Camera camera;

    /** Opens only the board; the camera remains lazy. */
    NativeTaskHardware(TaskOptions options) {
        this.options = options;
        device = Keel.open(options.openOptions());
    }

    /** Starts the established binding controller with the coordinator as its only step. */
    @Override
    public void start(Consumer<Tick> step) {
        loop = ControlLoop.builder(device, cycle -> step.accept(new NativeTick(cycle))).build();
        loop.start();
    }

    /** Disarms immediately and asks the controller to end. */
    @Override
    public void stop() {
        if (loop != null) loop.stop();
        else device.disarm();
    }

    /** Reports the native controller's recorded exception. */
    @Override
    public Throwable failure() {
        return loop == null ? null : loop.error().orElse(null);
    }

    /** Reports that the native controller has ended. */
    @Override
    public boolean ended() {
        return loop != null && loop.isEnded();
    }

    /** Reads a camera frame on the same thread that lazily opens and starts the camera. */
    @Override
    public Frame image(long timeoutMs) {
        if (camera == null) {
            camera = Camera.open(options.cameraOptions());
            camera.start();
        }
        return camera.readFrame(Duration.ofMillis(timeoutMs));
    }

    /** Returns the latest board snapshot. */
    @Override public Snapshot snapshot() { return loop.snapshot(); }
    /** Returns board capabilities. */
    @Override public Set<Capability> capabilities() { return device.capabilities(); }
    /** Returns the opening report. */
    @Override public OpenReport report() { return device.report(); }
    /** Returns control timing counters. */
    @Override public LoopStats stats() { return loop.stats(); }
    /** Changes one RGB LED from the task thread. */
    @Override public void setRgb(int index, int color) { device.setRgb(index, color); }
    /** Changes all RGB LEDs from the task thread. */
    @Override public void setRgbAll(int color) { device.setRgbAll(color); }
    /** Changes a complete RGB frame from the task thread. */
    @Override public void setRgbFrame(int[] colors) { device.setRgbFrame(colors); }
    /** Changes button indicators from the task thread. */
    @Override public void setButtonLeds(int mask, int values) { device.setButtonLeds(mask, values); }
    /** Measures range outside the hardware control thread. */
    @Override public RangeMeasurement measureDistance() {
        long deadline = System.nanoTime() + 1_000_000_000L;
        while (loop.snapshot().sensorPower() != SensorPower.PERMITTED && loop.snapshot().sensorPower() != SensorPower.CONFIRMED
                && loop.snapshot().state() != State.FAULTED && System.nanoTime() - deadline < 0) {
            try { Thread.sleep(5); }
            catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new TaskException(TaskException.Reason.CANCELLED, "Range wait interrupted", interrupted); }
        }
        while (true) {
            try { return device.rangeMeasure(); }
            catch (AgainException pending) {
                if (System.nanoTime() - deadline >= 0) throw pending;
                try { Thread.sleep(5); }
                catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new TaskException(TaskException.Reason.CANCELLED, "Range wait interrupted", interrupted); }
            }
        }
    }
    /** Changes a diagnostic simulator input. */
    @Override public void simSet(SimItem item, int value) { device.simSet(item, value); }

    /** Closes all resources, even if one closer throws. */
    @Override
    public void close() {
        Camera ownedCamera = camera;
        ControlLoop ownedLoop = loop;
        try (device; ownedCamera; ownedLoop) { /* Closers run controller, camera, device, retaining suppressed failures. */ }
    }

    /** The previous native snapshot plus commands for the upcoming cycle. */
    private record NativeTick(Cycle cycle) implements Tick {
        /** Returns the last observed native state. */
        @Override public State state() { return cycle.snapshot().state(); }
        /** Returns the cycle that will receive commands from this step. */
        @Override public long cycleId() { return cycle.cycleId(); }
        /** Returns the last completed native cycle. */
        @Override public long observedCycleId() { return cycle.snapshot().lastCycleId(); }
        /** Requires both the stored commands and actual outputs to be zero. */
        @Override public boolean stationary() {
            var snapshot = cycle.snapshot();
            return snapshot.commandLeft() == 0 && snapshot.commandRight() == 0
                    && snapshot.outputLeft() == 0 && snapshot.outputRight() == 0;
        }
        /** Starts arming from the control-owner thread. */
        @Override public void arm() { cycle.device().requestArm(); }
        /** Sets this cycle's motor duties. */
        @Override public void drive(int left, int right) { cycle.drive(left, right); }
        /** Clears the stored motor command in this cycle. */
        @Override public void coast() { cycle.coast(); }
    }
}
