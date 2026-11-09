// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The Java binding of libkeel 1.x: open a Keel device, run its control loop, and stop it from anywhere.
 *
 * <p>libkeel is the only safety logic. The binding passes every call through unchanged and maps every non-OK status
 * to one exception type per status class; the rules it implements are those of libkeel's {@code keel.h} and
 * {@code docs/bindings.md}.
 *
 * <p>This package is the entry point and the session: {@link io.github.bisocmorg.keel.Keel} loads the library,
 * reports its metadata and opens a {@link io.github.bisocmorg.keel.Device}, which has one method per function of
 * {@code keel.h}. The open options, the open report and its enums, the backends, the capabilities and the constants
 * {@link io.github.bisocmorg.keel.Abi} and {@link io.github.bisocmorg.keel.Limits} live here too. The other packages:
 *
 * <ul>
 * <li>{@link io.github.bisocmorg.keel.errors}: {@link io.github.bisocmorg.keel.errors.KeelException}, one subclass
 * per status class, {@link io.github.bisocmorg.keel.errors.LibraryException} and the status enums.</li>
 * <li>{@link io.github.bisocmorg.keel.loop}: {@link io.github.bisocmorg.keel.loop.ControlLoop}, one
 * {@code keel_cycle} per period on a dedicated platform thread with honest health, and the cycle's types.</li>
 * <li>{@link io.github.bisocmorg.keel.state}: the safety state, the snapshot, the safety inputs and the fault
 * record.</li>
 * <li>{@link io.github.bisocmorg.keel.peripherals}: the buttons, the RGB LEDs, the ultrasonic ranger and the sensor
 * power.</li>
 * <li>{@link io.github.bisocmorg.keel.sim}: the simulator's stimuli and ground truth.</li>
 * <li>{@link io.github.bisocmorg.keel.camera}: the Raspberry Pi camera, through its own library
 * {@code libkeel-camera.so.1}; it needs no device and never loads libkeel.so.</li>
 * </ul>
 *
 * <p>The enums, the constants and the data records are generated from {@code keel.h} and carry its documentation.
 * Value enums are open: {@code of(int)} returns {@code UNRECOGNIZED} for a value a newer library added, and the raw
 * value stays available: the {@code ...Value()} record components,
 * {@link io.github.bisocmorg.keel.errors.KeelException#code()} and the raw mask methods. A newer library never breaks
 * this binding, and no value is lost.
 */
package io.github.bisocmorg.keel;
