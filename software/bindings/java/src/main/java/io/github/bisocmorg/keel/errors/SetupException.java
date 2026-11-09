// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.errors;

import io.github.bisocmorg.keel.OpenReport;
import io.github.bisocmorg.keel.OpenResource;
import java.util.ArrayList;
import java.util.List;

/**
 * {@code keel_open} failed: no device was created.
 *
 * <p>keel.h: "Every non-OK status means no device was created [...] and a binding raises its setup error carrying
 * {@code report}, whatever the class." The report names the resource that failed and the remedy, for example
 * {@code "BCM8 is used by spi0"} and a {@code keel-setup} command.
 */
public final class SetupException extends KeelException {
    private static final long serialVersionUID = 1L;

    /** The open report libkeel wrote for this failure. */
    private final OpenReport report;

    SetupException(StatusDetails details, OpenReport report) {
        super(details, details.message() + describe(report));
        this.report = report;
    }

    private static String describe(OpenReport report) {
        List<String> parts = new ArrayList<>();
        OpenResource resource = report.resource();
        if (resource == OpenResource.GPIO_LINE && report.line() >= 0) {
            parts.add("BCM" + report.line() + (report.consumer().isEmpty() ? " is unavailable"
                    : " is used by " + report.consumer()));
        } else if (report.resourceValue() != 0) {
            parts.add("resource " + resource.name());
        }
        if (!report.path().isEmpty()) {
            parts.add("path " + report.path());
        }
        if (report.osError() != 0) {
            parts.add("errno " + report.osError());
        }
        if (!report.hint().isEmpty()) {
            parts.add("remedy: " + report.hint());
        }
        return parts.isEmpty() ? "" : "; " + String.join("; ", parts);
    }

    /**
     * Returns the open report libkeel wrote for this failure.
     *
     * @return the report, never {@code null}
     */
    public OpenReport report() {
        return report;
    }
}
