// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The private layer of the camera packages: loading libkeel-camera, its downcall handles, its struct layouts and
 * conversion, and the registry of the camera's exception factory. The module does not export this package; it is not
 * part of the API. Nothing here loads libkeel.so.
 */
package io.github.bisocmorg.keel.internal.camera;
