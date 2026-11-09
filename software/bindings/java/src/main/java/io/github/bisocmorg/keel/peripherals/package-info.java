// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The board's peripherals: the four buttons and their debounced
 * {@link io.github.bisocmorg.keel.peripherals.ButtonState}, the six RGB LEDs, the ultrasonic ranger's
 * {@link io.github.bisocmorg.keel.peripherals.RangeMeasurement} and the sensor power state. The servo has no types of
 * its own: its limits are in {@link io.github.bisocmorg.keel.Limits} and its capability is
 * {@link io.github.bisocmorg.keel.Capability#SERVO}. Every type is generated from {@code keel.h}.
 */
package io.github.bisocmorg.keel.peripherals;
