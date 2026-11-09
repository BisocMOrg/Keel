// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * JPEG stills up to the full sensor resolution: {@link io.github.bisocmorg.keel.camera.stills.StillImage}, its
 * {@link io.github.bisocmorg.keel.camera.stills.StillInfo}, the
 * {@link io.github.bisocmorg.keel.camera.stills.StillOptions} and their flags. Up to 1920x1920 the Raspberry Pi's
 * hardware JPEG encoder does the work, above it libjpeg.
 */
package io.github.bisocmorg.keel.camera.stills;
