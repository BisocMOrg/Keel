// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The device's safety state as libkeel reports it: the coarse {@link io.github.bisocmorg.keel.state.State}, the
 * {@link io.github.bisocmorg.keel.state.Snapshot} of state and diagnostics, the READY and ARMED
 * {@link io.github.bisocmorg.keel.state.SafetyInput} bits, the arming acknowledgement phase, and the
 * {@link io.github.bisocmorg.keel.state.Fault} record with its cause and flags. Every type is generated from
 * {@code keel.h}.
 */
package io.github.bisocmorg.keel.state;
