// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

/**
 * Status class HANDLE: the handle is not usable by this caller (closed, stale or owned by another process). Stop
 * using the device.
 *
 * <p>The binding also throws it, with {@link Status#E_INVALID_HANDLE}, for a call on a device it has closed or is
 * closing; it then makes no foreign call, so a stale handle never reaches libkeel again.
 */
public final class HandleException extends KeelException {
    private static final long serialVersionUID = 1L;

    HandleException(StatusDetails details, String message) {
        super(details, message);
    }
}
