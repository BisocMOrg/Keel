// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.OpenOptions;

/**
 * Opens the board and prints the open report's warnings, for {@code HardwareTest} to run under
 * {@code prlimit --rtprio=0}. It opens and closes the device and nothing else.
 */
public final class HardwareProbe {
    private HardwareProbe() {
    }

    /**
     * Opens the board, prints its warnings and closes it.
     *
     * @param args unused
     */
    public static void main(String[] args) {
        try (Device device = Keel.open(OpenOptions.builder().backend(Backend.R3).build())) {
            System.out.println("warnings " + device.report().warnings() + "; hint: " + device.report().hint());
        }
    }
}
