// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.OpenFlag;
import io.github.bisocmorg.keel.OpenOptions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Opens simulator devices: on the virtual clock, which a {@link CycleDriver} advances, or on the real-time clock. */
public final class SimulatorDevices {
    /** One tick period in microseconds. */
    public static final long PERIOD_US = Limits.TICK_PERIOD_MS * 1000L;

    private SimulatorDevices() {
    }

    /** A simulator on its virtual clock, with extra open flags. */
    public static Device open(OpenFlag... extra) {
        List<OpenFlag> flags = new ArrayList<>(Arrays.asList(extra));
        flags.add(OpenFlag.VIRTUAL_CLOCK);
        return Keel.open(OpenOptions.builder().backend(Backend.SIMULATOR).flags(flags.toArray(OpenFlag[]::new))
                .build());
    }

    /** A simulator on the real-time clock. */
    public static Device realtime() {
        return Keel.open(OpenOptions.builder().backend(Backend.SIMULATOR).build());
    }
}
