// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
/**
 * Sequential student tasks over the existing native binding. Tasks own their resource lifetime; motor actions
 * are finite, automatically arm once, and complete only after zero-drive acknowledgement. Percentage duties
 * and millisecond durations are shared with Python's keel.task API. Student navigation and computer vision
 * remain ordinary application code; this package contains no box-detection or path-planning algorithm.
 */
package io.github.bisocmorg.keel.task;
