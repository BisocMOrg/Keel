// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import java.lang.foreign.MemorySegment;

/**
 * Observes the calls the binding makes into libkeel, can refuse them by throwing, and can rewrite the statuses they
 * return.
 *
 * <p>This is the binding's private test layer: the test suites install an interceptor to keep a test from ever
 * opening real hardware, to refuse motion on the robot and to prove that a closed device makes no foreign call.
 * Production code never installs one, and with none installed {@link NativeLibrary} makes no extra work.
 */
public interface NativeCallInterceptor {
    /**
     * Runs before every call that takes a device handle, on the calling thread. Throwing refuses the call.
     *
     * @param function the C function name
     * @param arguments the call's arguments in C order, the device handle first; integers are boxed and struct
     *     pointers are their segments
     */
    default void before(String function, Object... arguments) {
    }

    /**
     * Runs after every call that takes a device handle and returns a status.
     *
     * @param function the C function name
     * @param status the status libkeel returned
     * @return the status the binding acts on, normally {@code status}
     */
    default int after(String function, int status) {
        return status;
    }

    /**
     * Runs after {@code keel_open} created a device, before the binding wraps it. Throwing makes the open fail;
     * the interceptor must then close the device itself.
     *
     * @param device the new device handle
     * @param report the open report libkeel wrote
     */
    default void opened(MemorySegment device, MemorySegment report) {
    }
}
