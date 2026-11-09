// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.errors.UnsupportedException;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.Cycle;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.state.State;

/**
 * LEDs: a colour frame walks along the six RGB LEDs and the button LEDs light in turn. Halfway through, an emergency
 * stop latches FAULTED and every RGB LED shows an error colour: LEDs work in every state.
 *
 * <p>A board whose LED driver is missing opens without {@link Capability#RGB}; the RGB calls then throw
 * {@link UnsupportedException}, which the example reports once, and the button LEDs still run. Nothing moves.
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/Leds.java --simulator
 * </pre>
 *
 * <p>Exit status: 0 success, 1 failure, 2 usage, 128 + N after signal N (130 after Ctrl-C, 143 after SIGTERM).
 */
public final class Leds {
    private static final int[] PALETTE = {0x200000, 0x201000, 0x202000, 0x002000, 0x000020, 0x100020};
    private static final int ERROR_RED = 0x400000;
    private static final int STEP_CYCLES = 10;

    private long durationMs = 3000;
    private boolean simulator;
    private boolean rgb = true;
    private long startedUs;
    private boolean stopped;
    private boolean errorShown;
    private boolean done;

    private Leds() {
    }

    /**
     * Runs the example.
     *
     * @param args {@code [--duration MS] [--simulator]}
     */
    public static void main(String[] args) {
        System.exit(new Leds().run(args));
    }

    private int run(String[] args) {
        try {
            for (int i = 0; i < args.length; i++) {
                switch (args[i]) {
                    case "--duration" -> durationMs = Math.max(200, Integer.parseInt(args[++i]));
                    case "--simulator" -> simulator = true;
                    default -> throw new IllegalArgumentException(args[i]);
                }
            }
        } catch (ArrayIndexOutOfBoundsException | IllegalArgumentException error) {
            System.err.println("Usage: Leds [--duration MS] [--simulator]");
            return 2;
        }
        OpenOptions options = OpenOptions.builder().backend(simulator ? Backend.SIMULATOR : Backend.AUTO).build();
        try (Device device = Keel.open(options)) {
            if (!device.capabilities().contains(Capability.RGB)) {
                System.out.println("RGB LEDs unavailable (" + device.report().hint() + "); button LEDs only");
                rgb = false;
            }
            startedUs = device.nowUs();
            ControlLoop loop = ControlLoop.builder(device, this::step).build();
            loop.runUntil(snapshot -> done, Long.MAX_VALUE);
            if (rgb) {
                device.setRgbFrame(new int[Limits.LED_COUNT]);
            }
            device.setButtonLeds(Button.ALL.mask(), 0);
            System.out.println("LEDs off");
            return errorShown ? 0 : 1;
        } catch (UnsupportedException noRgb) {
            System.out.println("RGB LEDs unavailable; done");
            return 0;
        } catch (LibraryException | KeelException error) {
            System.err.println("leds: " + error.getMessage());
            return 1;
        }
    }

    private void step(Cycle cycle) {
        Device device = cycle.device();
        long elapsedMs = (cycle.nowUs() - startedUs) / 1000;
        if (elapsedMs >= durationMs) {
            done = true;
            return;
        }
        if (cycle.snapshot().state() == State.FAULTED) {
            if (!errorShown) {
                showAll(device, ERROR_RED);
                System.out.println("state: FAULTED; error colour shown");
                errorShown = true;
            }
            return;
        }
        if (!stopped && elapsedMs >= durationMs / 2) {
            device.emergencyStop();
            stopped = true;
            return;
        }
        if (cycle.cycleId() % STEP_CYCLES == 0) {
            int offset = (int) (cycle.cycleId() / STEP_CYCLES);
            int[] frame = new int[Limits.LED_COUNT];
            for (int led = 0; led < frame.length; led++) {
                frame[led] = PALETTE[(led + offset) % PALETTE.length];
            }
            if (rgb) {
                device.setRgbFrame(frame);
            }
            Button lit = Button.values()[offset % Limits.BUTTON_COUNT];
            device.setButtonLeds(Button.ALL.mask(), lit.mask());
            if (offset == 1) {
                System.out.println("frame: " + (rgb ? "RGB1..RGB6 walking" : "skipped") + ", button LED " + lit);
            }
        }
    }

    private void showAll(Device device, int colour) {
        if (rgb) {
            device.setRgbAll(colour);
        }
        device.setButtonLeds(Button.ALL.mask(), Button.ALL.mask());
    }
}
