// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The outputs libkeel-camera writes itself, from its own threads: files, a TCP connection, a listening TCP port that
 * serves several receivers, and UDP. {@code Camera.startOutput} starts one with
 * {@link io.github.bisocmorg.keel.camera.outputs.OutputOptions} and returns its id;
 * {@link io.github.bisocmorg.keel.camera.outputs.OutputStats} and
 * {@link io.github.bisocmorg.keel.camera.outputs.OutputState} report it.
 */
package io.github.bisocmorg.keel.camera.outputs;
