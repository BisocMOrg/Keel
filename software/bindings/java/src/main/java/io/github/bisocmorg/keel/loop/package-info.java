// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The control loop of libkeel's binding rules. {@link io.github.bisocmorg.keel.loop.ControlLoop} runs one
 * {@code keel_cycle} per period on a dedicated platform thread, on the device clock, with honest health, and hands
 * each {@link io.github.bisocmorg.keel.loop.Cycle} to the application's step; the package also holds the cycle input,
 * its command, flags and health bits, and the loop's statistics.
 */
package io.github.bisocmorg.keel.loop;
