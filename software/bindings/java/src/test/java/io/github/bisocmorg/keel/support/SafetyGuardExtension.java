// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import static java.lang.foreign.ValueLayout.JAVA_INT;

import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.internal.NativeCallInterceptor;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import io.github.bisocmorg.keel.internal.StructCodec;
import io.github.bisocmorg.keel.loop.CycleCommand;
import java.lang.foreign.MemorySegment;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import org.junit.jupiter.api.extension.BeforeAllCallback;
import org.junit.jupiter.api.extension.ExtensionContext;

/**
 * Keeps every test from moving a robot by accident. Registered for the whole suite through
 * {@code META-INF/services} and JUnit's extension autodetection.
 *
 * <p>For every test class it installs one interceptor into the binding's private layer:
 * <ul>
 * <li>by default, a device that does not report {@link Capability#SIMULATED} is closed at once and fails the test,
 * before {@code request_arm} could be called;</li>
 * <li>for classes tagged {@code hardware}, a motion guard instead refuses every call that could move the robot:
 * {@code keel_request_arm}, {@code keel_set_motors} with a non-zero duty, {@code keel_brake},
 * {@code keel_servo_start}, and {@code keel_cycle} with {@code BRAKE} or a non-zero {@code DRIVE}. It sits below the
 * public API, so the control loop's own calls are covered too.</li>
 * </ul>
 * Tests that observe calls add a {@link NativeCallInterceptor} to {@link #OBSERVERS}; the guard runs first.
 */
public final class SafetyGuardExtension implements BeforeAllCallback {
    /** Extra interceptors of individual tests, called after the guard's own checks. */
    public static final List<NativeCallInterceptor> OBSERVERS = new CopyOnWriteArrayList<>();

    private static final Guard GUARD = new Guard();

    /** Creates the extension; JUnit instantiates it. */
    public SafetyGuardExtension() {
    }

    @Override
    public void beforeAll(ExtensionContext context) {
        GUARD.motionOnly = context.getTags().contains("hardware");
        OBSERVERS.clear();
        NativeLibrary.install(GUARD);
    }

    private static final class Guard implements NativeCallInterceptor {
        private volatile boolean motionOnly;

        @Override
        public void before(String function, Object... arguments) {
            if (motionOnly) {
                refuseMotion(function, arguments);
            }
            for (NativeCallInterceptor observer : OBSERVERS) {
                observer.before(function, arguments);
            }
        }

        @Override
        public int after(String function, int status) {
            int result = status;
            for (NativeCallInterceptor observer : OBSERVERS) {
                result = observer.after(function, result);
            }
            return result;
        }

        @Override
        public void opened(MemorySegment device, MemorySegment report) {
            if (motionOnly) {
                return;
            }
            int capabilities = StructCodec.openReport(report).capabilitiesValue();
            if ((capabilities & Capability.SIMULATED.mask()) == 0) {
                NativeLibrary.get().close(device, MemorySegment.NULL);
                throw new AssertionError("a test opened a device that is not the simulator; it was closed at once. "
                        + "Open Backend.SIMULATOR explicitly, never AUTO.");
            }
        }

        private static void refuseMotion(String function, Object... arguments) {
            boolean motion = switch (function) {
                case "keel_request_arm", "keel_brake", "keel_servo_start" -> true;
                case "keel_set_motors" -> (int) arguments[1] != 0 || (int) arguments[2] != 0;
                case "keel_cycle" -> {
                    MemorySegment in = (MemorySegment) arguments[1];
                    int command = in.get(JAVA_INT, 16);
                    yield command == CycleCommand.BRAKE.code() || (command == CycleCommand.DRIVE.code()
                            && (in.get(JAVA_INT, 20) != 0 || in.get(JAVA_INT, 24) != 0));
                }
                default -> false;
            };
            if (motion) {
                throw new AssertionError("the hardware motion guard refused " + function
                        + ": the robot stage runs without motion");
            }
        }
    }
}
