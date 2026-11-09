// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The Raspberry Pi camera on the robot, through libkeel-camera: {@link io.github.bisocmorg.keel.camera.Camera}.
 *
 * <p>The camera connects to the Raspberry Pi's camera connector, not to the Keel board, so it has a library of its
 * own, {@code libkeel-camera.so.1} ({@code camera.h}). This package binds it the way the rest of the module binds
 * libkeel: thin wrappers, every status thrown as an exception of
 * {@link io.github.bisocmorg.keel.camera.errors}, the constants and records generated from the header. It needs
 * neither libkeel.so nor a {@link io.github.bisocmorg.keel.Device}. Camera Module 2 (IMX219, fixed focus) and Camera
 * Module 3 (IMX708, autofocus; also Wide and NoIR) are supported; any other camera libcamera drives works for capture.
 *
 * <pre>{@code
 * try (Camera camera = Camera.open(CameraOptions.builder().pixelFormat(PixelFormat.BGR888).build())) {
 *     camera.start();
 *     for (Frame frame : camera.frames()) {
 *         ByteBuffer pixels = frame.data();       // 640x480, rows of info().stride0() bytes, B, G, R
 *         ...
 *     }
 * }
 * }</pre>
 *
 * <p>This package holds the camera, its options and listing, the camera's info, sensor modes and counters, and the
 * focus, white-balance and flip options. The other types are grouped by what they describe:
 *
 * <ul>
 * <li>{@link io.github.bisocmorg.keel.camera.frames}: raw frames, their layout and pixel formats.</li>
 * <li>{@link io.github.bisocmorg.keel.camera.encoding}: the hardware encoders and the encoded chunks.</li>
 * <li>{@link io.github.bisocmorg.keel.camera.outputs}: the files and network streams the library writes itself.</li>
 * <li>{@link io.github.bisocmorg.keel.camera.stills}: JPEG stills.</li>
 * <li>{@link io.github.bisocmorg.keel.camera.errors}: the exceptions and the error record.</li>
 * </ul>
 *
 * <p>The camera library is loaded on the first camera call ({@code libkeel-camera.so.1}, or the absolute path in
 * {@code KEEL_CAMERA_LIBRARY}); on the robot it comes with the {@code libkeel-camera1} package. The JVM grants the
 * module native access as for libkeel.
 */
package io.github.bisocmorg.keel.camera;
