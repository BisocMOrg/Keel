// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.state.Fault;
import java.lang.foreign.MemorySegment;

/**
 * The control loop's view of a {@link Device}: the raw calls it makes once per cycle and the device state it reads,
 * none of which is public API. {@link AccessRegistry#devices()} returns the one implementation.
 */
public interface DeviceAccess {
    /**
     * Claims the device for a control loop: one loop serves a device for the device's whole life.
     *
     * @param device the device
     * @return {@code false} if a loop already serves it
     */
    boolean attachLoop(Device device);

    /**
     * {@code keel_cycle} for the loop: the raw status, and no call once the device is closing.
     *
     * @param device the device
     * @param in the loop's {@code keel_cycle_input}
     * @param out the loop's {@code keel_snapshot}
     * @return the status; {@link Status#E_INVALID_HANDLE} when the device is closing
     */
    int cycleRaw(Device device, MemorySegment in, MemorySegment out);

    /**
     * {@code keel_sleep_until} for the loop.
     *
     * @param device the device
     * @param deadlineUs the absolute device time
     * @return the status; {@link Status#E_INVALID_HANDLE} when the device is closing
     */
    int sleepUntilRaw(Device device, long deadlineUs);

    /**
     * {@code keel_now_us} for the loop.
     *
     * @param device the device
     * @param out a {@code uint64_t} the loop owns
     * @return the device time, or -1 when the device is closing or closed
     */
    long nowUsRaw(Device device, MemorySegment out);

    /**
     * Returns whether the device is closing or closed.
     *
     * @param device the device
     * @return {@code true} once {@link Device#close()} has started
     */
    boolean closing(Device device);

    /**
     * Returns the library the device belongs to.
     *
     * @param device the device
     * @return the library
     */
    NativeLibrary library(Device device);

    /**
     * The fault record for a FAULT exception.
     *
     * @param device the device
     * @return the record, or {@code null} when the device is closing or the read fails
     */
    Fault faultOrNull(Device device);
}
