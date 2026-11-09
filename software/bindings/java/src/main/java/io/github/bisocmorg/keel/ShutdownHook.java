// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import io.github.bisocmorg.keel.errors.Status;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The binding's single JVM shutdown hook.
 *
 * <p>The JVM runs shutdown hooks on SIGINT, SIGTERM, SIGHUP and {@code System.exit}. For every device still open,
 * the hook calls {@code keel_emergency_stop} and then closes the device as {@link Device#close()} does, waiting for
 * the close to finish, so every exit but SIGKILL ends in a Category-0 stop and a close. The registry holds no
 * {@link Device}, so a leaked device can still be collected and cleaned. Errors are logged, never thrown.
 */
final class ShutdownHook {
    private static final System.Logger LOG = System.getLogger("io.github.bisocmorg.keel");
    private static final AtomicBoolean REGISTERED = new AtomicBoolean();

    private ShutdownHook() {
    }

    /** Registers the hook at the first open. */
    static void ensureRegistered() {
        if (REGISTERED.compareAndSet(false, true)) {
            try {
                Runtime.getRuntime().addShutdownHook(Thread.ofPlatform().name("keel-shutdown")
                        .unstarted(ShutdownHook::run));
            } catch (IllegalStateException shuttingDown) {
                // The JVM is already shutting down: a device opened now is closed by its owner or its cleaner.
                LOG.log(System.Logger.Level.DEBUG, "the JVM is shutting down; no shutdown hook registered");
            }
        }
    }

    /** Stops and closes every device that is still open. */
    static void run() {
        for (CloseState state : List.copyOf(CloseState.OPEN)) {
            try {
                if (!state.closing.get()) {
                    int status = state.library.emergencyStop(state.token);
                    if (status == Status.E_INVALID_HANDLE.code()) {
                        LOG.log(System.Logger.Level.DEBUG, "already stopped by close");
                    } else if (status != 0) {
                        LOG.log(System.Logger.Level.WARNING, "keel_emergency_stop at JVM exit returned {0}",
                                Keel.statusName(status));
                    }
                }
                state.close();
            } catch (RuntimeException | Error failure) {
                LOG.log(System.Logger.Level.ERROR, "stopping a device at JVM exit failed", failure);
            }
        }
    }
}
