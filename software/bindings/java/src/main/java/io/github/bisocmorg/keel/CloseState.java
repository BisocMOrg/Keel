// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.internal.Layouts;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import io.github.bisocmorg.keel.internal.StructCodec;
import io.github.bisocmorg.keel.state.Fault;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.ref.Cleaner;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The part of a {@link Device} that outlives it: the handle, the closing flag, the close result and its completion
 * latch. It is also the close action the binding's {@link Cleaner} runs, so it never refers to the {@code Device}.
 *
 * <p>Close happens exactly once: {@link Cleaner.Cleanable#clean()} runs this action at most once, and a second
 * concurrent {@code clean()} returns at once while the first is still inside {@code keel_close}, which is why every
 * closer waits on the latch.
 */
final class CloseState implements Runnable {
    private static final System.Logger LOG = System.getLogger("io.github.bisocmorg.keel");
    private static final Cleaner CLEANER = Cleaner.create(task -> Thread.ofPlatform().daemon(true)
            .name("keel-cleaner").unstarted(task));

    /** The states of every device that is not closed yet; the shutdown hook walks it. */
    static final Set<CloseState> OPEN = ConcurrentHashMap.newKeySet();

    final MemorySegment token;
    final NativeLibrary library;
    final AtomicBoolean closing = new AtomicBoolean();
    private final CountDownLatch closed = new CountDownLatch(1);
    private volatile boolean explicit;
    private volatile boolean completed;
    private volatile int closeStatus;
    private volatile Fault finalFault;
    private Cleaner.Cleanable cleanable;

    CloseState(MemorySegment token, NativeLibrary library) {
        this.token = token;
        this.library = library;
    }

    /**
     * Registers the close action for a device and adds the state to {@link #OPEN}.
     *
     * @param owner the device whose unreachability triggers the action
     */
    void register(Object owner) {
        cleanable = CLEANER.register(owner, this);
        OPEN.add(this);
        ShutdownHook.ensureRegistered();
    }

    /**
     * Closes the device and returns once {@code keel_close} has returned, whichever thread runs it. Never throws.
     */
    void close() {
        explicit = true;
        closing.set(true);
        cleanable.clean();
        awaitClosed();
    }

    /** Waits for the close to finish; an interrupt is kept for after the wait (the wait is about 100 ms at most). */
    private void awaitClosed() {
        boolean interrupted = false;
        while (true) {
            try {
                closed.await();
                break;
            } catch (InterruptedException interruption) {
                interrupted = true;
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    /**
     * Waits a bounded time for the close to finish.
     *
     * @param timeoutMs the longest wait
     * @return whether the close has finished
     * @throws InterruptedException if the waiting thread is interrupted
     */
    boolean awaitClosed(long timeoutMs) throws InterruptedException {
        return closed.await(timeoutMs, TimeUnit.MILLISECONDS);
    }

    /** The close action: run once, by {@link #close()} or by the cleaner for a device that was never closed. */
    @Override
    public void run() {
        try {
            closing.set(true);
            if (!explicit) {
                LOG.log(System.Logger.Level.WARNING, "Device was not closed; the cleaner closed it. "
                        + "Close devices with try-with-resources.");
            }
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment fault = StructCodec.allocate(arena, Layouts.FAULT);
                int status = library.close(token, fault);
                if (status != Status.E_INVALID_ARGUMENT.code() && status != Status.E_INVALID_HANDLE.code()
                        && status != Status.E_NOT_OWNER.code()) {
                    finalFault = StructCodec.fault(fault);
                }
                closeStatus = status;
                completed = true;
                if (status != 0) {
                    Fault record = finalFault;
                    LOG.log(System.Logger.Level.WARNING, "keel_close returned {0}; cleanup status {1}",
                            Keel.statusName(status), record == null ? "unknown" : record.cleanupStatus().name());
                }
            }
        } catch (RuntimeException | Error failure) {
            // Close never throws (keel.h): a failure here is logged and the device still counts as closed.
            LOG.log(System.Logger.Level.ERROR, "closing the device failed", failure);
        } finally {
            OPEN.remove(this);
            closed.countDown();
        }
    }

    /**
     * Returns whether {@code keel_close} has returned.
     *
     * @return {@code true} once the close result is stored
     */
    boolean completed() {
        return completed;
    }

    /**
     * Returns the status {@code keel_close} returned.
     *
     * @return the status; meaningful once {@link #completed()} is {@code true}
     */
    int closeStatus() {
        return closeStatus;
    }

    /**
     * Returns the final fault record {@code keel_close} wrote.
     *
     * @return the record, or {@code null} before the close or when the close left it untouched
     */
    Fault finalFault() {
        return finalFault;
    }
}
