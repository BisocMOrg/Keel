// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * Raw camera frames: {@link io.github.bisocmorg.keel.camera.frames.Frame}, its
 * {@link io.github.bisocmorg.keel.camera.frames.FrameInfo}, the
 * {@link io.github.bisocmorg.keel.camera.frames.PixelFormat} and the
 * {@link io.github.bisocmorg.keel.camera.frames.StreamInfo} of a camera's configured stream. A frame is backed by a
 * direct buffer libkeel-camera wrote into, readable as a {@link java.nio.ByteBuffer}, a
 * {@link java.lang.foreign.MemorySegment} or, through {@code java.desktop}, a {@link java.awt.image.BufferedImage}.
 */
package io.github.bisocmorg.keel.camera.frames;
