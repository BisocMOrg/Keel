// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.task;

import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.OpenReport;
import io.github.bisocmorg.keel.loop.LoopStats;
import io.github.bisocmorg.keel.peripherals.RangeMeasurement;
import io.github.bisocmorg.keel.state.Snapshot;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.State;
import java.util.Set;
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

    /** Returns the latest immutable board observation. */
    default Snapshot snapshot() { throw new UnsupportedOperationException(); }
    /** Returns board capabilities. */
    default Set<Capability> capabilities() { throw new UnsupportedOperationException(); }
    /** Returns the opening report. */
    default OpenReport report() { throw new UnsupportedOperationException(); }
    /** Returns control timing counters. */
    default LoopStats stats() { throw new UnsupportedOperationException(); }
    /** Changes one RGB LED. */
    default void setRgb(int index, int color) { throw new UnsupportedOperationException(); }
    /** Changes the complete RGB frame. */
    default void setRgbFrame(int[] colors) { throw new UnsupportedOperationException(); }
    /** Changes all RGB LEDs. */
    default void setRgbAll(int color) { throw new UnsupportedOperationException(); }
    /** Changes button indicators. */
    default void setButtonLeds(int mask, int values) { throw new UnsupportedOperationException(); }
    /** Takes a bounded range measurement off the control thread. */
    default RangeMeasurement measureDistance() { throw new UnsupportedOperationException(); }
    /** Changes a simulator input without exposing the native device. */
    default void simSet(SimItem item, int value) { throw new UnsupportedOperationException(); }

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
