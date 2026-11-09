// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The binding's private layer: loading libkeel, the downcall handles, struct layouts and conversion, the registry
 * through which the binding's packages reach each other's non-public members, and the interceptor the test suites
 * install. The camera's private layer is the subpackage {@code internal.camera}. The module exports neither; they are
 * not part of the API.
 */
package io.github.bisocmorg.keel.internal;
