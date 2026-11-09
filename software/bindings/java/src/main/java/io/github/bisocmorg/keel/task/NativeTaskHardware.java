// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.Cycle;
import io.github.bisocmorg.keel.state.State;
import java.time.Duration;
import java.util.function.Consumer;

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
