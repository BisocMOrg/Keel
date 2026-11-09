// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.state.State;
import java.util.function.Consumer;

/** The hardware boundary used by the task coordinator and deterministic tests. */
interface TaskHardware extends AutoCloseable {
    /** Starts the control thread, invoking one step per control cycle. */
    void start(Consumer<Tick> step);

    /** Stops and disarms immediately, without waiting for the control thread. */
    void stop();

    /** Returns a recorded control failure, or null while healthy. */
    Throwable failure();

    /** Returns whether the control loop has ended unexpectedly. */
    boolean ended();

    /** Opens the camera lazily and reads one frame on the calling task thread. */
    Frame image(long timeoutMs);

    /** Closes camera, controller and device, retaining any failure. */
    @Override
    void close();

    /** One control step, including the previous completed native-cycle observation. */
    interface Tick {
        /** Returns the current board state. */
        State state();
        /** Returns this step's upcoming native-cycle identifier. */
        long cycleId();
        /** Returns the previous completed native-cycle identifier. */
        long observedCycleId();
        /** Reports both zero stored motor commands and zero applied outputs. */
        boolean stationary();
        /** Requests the initial arming handshake from the native control-owner thread. */
        void arm();
        /** Supplies this cycle's signed-permille drive command. */
        void drive(int left, int right);
        /** Supplies this cycle's explicit coast command. */
        void coast();
    }
}
