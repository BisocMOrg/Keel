// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import io.github.bisocmorg.keel.internal.NativeCallInterceptor;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Records the device functions the binding calls, for tests that assert which calls were made. Active from
 * construction until {@link #close()}.
 */
public final class CallRecorder implements NativeCallInterceptor, AutoCloseable {
    /** The C function names, in call order. */
    public final List<String> calls = new CopyOnWriteArrayList<>();

    /** Starts recording. */
    public CallRecorder() {
        SafetyGuardExtension.OBSERVERS.add(this);
    }

    @Override
    public void before(String function, Object... arguments) {
        calls.add(function);
    }

    @Override
    public void close() {
        SafetyGuardExtension.OBSERVERS.remove(this);
    }
}
