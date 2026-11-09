// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.internal.NativeCallInterceptor;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Makes the binding see a chosen status from one function, once armed with {@link #next()}; the real call still
 * runs. Active from construction until {@link #close()}.
 */
public final class StatusOverride implements NativeCallInterceptor, AutoCloseable {
    private final String function;
    private final int status;
    private final AtomicInteger pending = new AtomicInteger();

    /** Overrides results of {@code function} with a known status. */
    public StatusOverride(String function, Status status) {
        this(function, status.code());
    }

    /** Overrides results of {@code function} with a raw status, which may be one libkeel does not define. */
    public StatusOverride(String function, int status) {
        this.function = function;
        this.status = status;
        SafetyGuardExtension.OBSERVERS.add(this);
    }

    /** Overrides the next result of the function. */
    public void next() {
        pending.incrementAndGet();
    }

    @Override
    public int after(String name, int result) {
        if (name.equals(function) && pending.getAndUpdate(count -> Math.max(0, count - 1)) > 0) {
            return status;
        }
        return result;
    }

    @Override
    public void close() {
        SafetyGuardExtension.OBSERVERS.remove(this);
    }
}
