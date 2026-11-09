// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The simulator backend's test interface: the stimuli {@link io.github.bisocmorg.keel.sim.SimItem} that
 * {@code Device.simSet} applies, and the ground truth {@link io.github.bisocmorg.keel.sim.SimState} with its
 * {@link io.github.bisocmorg.keel.sim.SimFlag} bits that {@link io.github.bisocmorg.keel.Device#simState()} reads. On
 * hardware both calls are refused. Every type is generated from {@code keel.h}.
 */
package io.github.bisocmorg.keel.sim;
