// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.OpenReport;
import io.github.bisocmorg.keel.OpenResource;
import io.github.bisocmorg.keel.TimingClass;
import io.github.bisocmorg.keel.internal.AccessRegistry;
import io.github.bisocmorg.keel.internal.ErrorAccess;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.state.Snapshot;
import java.util.function.Supplier;

/**
 * Maps a status to its exception by class (bindings.md 6): libkeel decides the class, and undefined codes and unknown
 * classes read as FAULT.
 *
 * <p>The exception constructors are package-private, so the other packages of the binding build exceptions through
 * this factory, which {@link KeelException} registers as {@link AccessRegistry#errors()}.
 */
final class ErrorFactory implements ErrorAccess {
    @Override
    public KeelException of(int code, String function, Supplier<Fault> fault, Snapshot snapshot) {
        return of(code, NativeLibrary.get().statusClass(code), function, fault, null, snapshot);
    }

    @Override
    public KeelException of(int code, int classValue, String function, Supplier<Fault> fault, OpenReport report,
            Snapshot snapshot) {
        StatusDetails details = details(code, classValue, function, snapshot);
        return switch (StatusClass.of(classValue)) {
            case REJECTED -> rejected(details, details.message());
            case PERIPHERAL -> new PeripheralException(details);
            case HANDLE -> new HandleException(details, details.message());
            case SETUP -> new SetupException(details, report != null ? report : unreported(code));
            // FAULT, and a class this binding does not know: the conservative reading libkeel applies itself.
            case OK, FAULT, UNRECOGNIZED -> new SafetyFaultException(details, fault.get());
        };
    }

    @Override
    public SetupException setup(int code, OpenReport report) {
        return new SetupException(details(code, NativeLibrary.get().statusClass(code), "keel_open", null), report);
    }

    @Override
    public HandleException closed(String function) {
        StatusDetails details = details(Status.E_INVALID_HANDLE.code(), StatusClass.HANDLE.code(), function, null);
        return new HandleException(details, details.message() + " (the binding refused the call because the device "
                + "is closed or closing; no foreign call was made)");
    }

    @Override
    public HandleException loopEnded(String function) {
        StatusDetails details = details(Status.E_INVALID_HANDLE.code(), StatusClass.HANDLE.code(), function, null);
        return new HandleException(details, details.message() + " (the control loop has ended, so the request was "
                + "not run; no foreign call was made)");
    }

    @Override
    public InvalidArgumentException dutyRefused(String caller, int left, int right) {
        StatusDetails details = details(Status.E_INVALID_ARGUMENT.code(), StatusClass.REJECTED.code(), "keel_cycle",
                null);
        return new InvalidArgumentException(details, details.message() + " (" + caller + " refused duty (" + left
                + ", " + right + "): the control loop accepts -" + Limits.DUTY_MAX + ".." + Limits.DUTY_MAX
                + " and coasts instead; no foreign call was made)");
    }

    private static RejectedException rejected(StatusDetails details, String message) {
        return switch (Status.of(details.code())) {
            case E_INVALID_ARGUMENT -> new InvalidArgumentException(details, message);
            case E_STATE -> new StateException(details, message);
            case E_AGAIN -> new AgainException(details, message);
            case E_UNSUPPORTED -> new UnsupportedException(details, message);
            case E_CONFLICT -> new ConflictException(details, message);
            default -> new RejectedException(details, message);
        };
    }

    private static StatusDetails details(int code, int classValue, String function, Snapshot snapshot) {
        return new StatusDetails(code, classValue, Keel.statusName(code), Keel.statusString(code), function, snapshot);
    }

    /** A report for a SETUP-class status that did not come from {@code keel_open}: only the status is known. */
    private static OpenReport unreported(int code) {
        return new OpenReport(code, OpenResource.NONE.code(), 0, -1, Backend.AUTO.code(), 0, 0,
                TimingClass.NONE.code(), 0, "", "", "");
    }
}
