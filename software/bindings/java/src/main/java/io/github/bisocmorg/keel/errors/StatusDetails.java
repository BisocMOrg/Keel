// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

import io.github.bisocmorg.keel.state.Snapshot;
import java.io.Serializable;

/**
 * What a status exception describes: the status, its class and strings, the C function and, for {@code keel_cycle},
 * the snapshot the call wrote.
 *
 * @param code the raw status
 * @param classValue the raw {@code keel_status_class} value of the status
 * @param name the status name, {@code keel_status_name(code)}
 * @param description the remedy sentence, {@code keel_status_string(code)}
 * @param function the C function that returned the status, or would have
 * @param snapshot the snapshot {@code keel_cycle} wrote, or {@code null}
 */
record StatusDetails(int code, int classValue, String name, String description, String function, Snapshot snapshot)
        implements Serializable {

    /**
     * The common start of every status message: {@code "keel_set_motors: KEEL_E_CONFLICT: <description>"}.
     *
     * @return the message
     */
    String message() {
        return function + ": " + name + ": " + description;
    }
}
