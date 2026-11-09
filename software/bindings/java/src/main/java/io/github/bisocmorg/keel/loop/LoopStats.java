// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.loop;

import io.github.bisocmorg.keel.errors.Status;
import java.util.Map;

/**
 * Counters of a {@link ControlLoop}, from {@link ControlLoop#stats()}.
 *
 * @param cycles the {@code keel_cycle} calls made
 * @param overruns the cycles that ended more than one period late; the loop then resynchronises instead of catching
 *     up
 * @param maxGapUs the largest interval between two {@code keel_cycle} calls, in microseconds of device time
 * @param maxStepUs the longest application work of one cycle (queued requests and the step), in microseconds
 * @param rejected the REJECTED results of {@code keel_cycle} per status, for example {@link Status#E_STATE} for a
 *     drive command while arming
 * @param stepErrors the steps that threw
 * @param faults the transitions into FAULTED the loop saw
 */
public record LoopStats(long cycles, long overruns, long maxGapUs, long maxStepUs, Map<Status, Long> rejected,
        long stepErrors, long faults) {

    /**
     * Copies the per-status counts so the record is immutable.
     *
     * @throws NullPointerException if {@code rejected} is {@code null}
     */
    public LoopStats {
        rejected = Map.copyOf(rejected);
    }
}
