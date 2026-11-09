// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import io.github.bisocmorg.keel.errors.KeelException;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import io.github.bisocmorg.keel.loop.CycleInput;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.Fault;
import io.github.bisocmorg.keel.support.EnumeratorTable;
import io.github.bisocmorg.keel.support.JsonReader;
import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.atomic.AtomicReference;
import java.util.stream.Stream;
import org.junit.jupiter.api.DynamicTest;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.TestFactory;

/**
 * Replays libkeel's language-neutral conformance scenarios ({@code tests/bindings/scenarios.json})
 * through the public API. Each C function name maps to its method here, which proves the mapping complete; a
 * status is the code of the exception a method threw, or {@code KEEL_OK}.
 */
class ConformanceTest {
    private static final Path SCENARIOS = Path.of(System.getProperty("keel.source.dir"), "tests", "bindings",
            "scenarios.json");

    @SuppressWarnings("unchecked")
    private static Map<String, Object> document() throws IOException {
        return (Map<String, Object>) JsonReader.parse(Files.readString(SCENARIOS));
    }

    @SuppressWarnings("unchecked")
    private static List<Map<String, Object>> scenarios() throws IOException {
        return (List<Map<String, Object>>) document().get("scenarios");
    }

    @Test
    void theFileIsLicensedAndCoversEveryFunction() throws IOException {
        Map<String, Object> document = document();
        assertEquals("SPDX-License-Identifier: MIT", document.get("license"));
        assertEquals("Copyright (c) 2026 BisocM", document.get("copyright"));
        assertEquals(1L, document.get("format"));
        Set<String> used = new TreeSet<>(Set.of("keel_open"));
        for (Map<String, Object> scenario : scenarios()) {
            for (Object item : (List<?>) scenario.get("steps")) {
                Map<?, ?> step = (Map<?, ?>) item;
                if (step.containsKey("call")) {
                    used.add((String) step.get("call"));
                } else if (step.containsKey("sim")) {
                    used.add("keel_sim_set");
                }
            }
        }
        assertEquals(new TreeSet<>(NativeLibrary.functionNames()), used, "every function appears in a scenario");
    }

    @TestFactory
    Stream<DynamicTest> everyScenarioHolds() throws IOException {
        return scenarios().stream().map(scenario -> DynamicTest.dynamicTest((String) scenario.get("name"),
                () -> new Replay(scenario).run()));
    }

    /** One scenario on one simulator device; the test thread is the owner thread. */
    private static final class Replay {
        private final Map<String, Object> scenario;
        private Device device;
        private long cycleId;
        private long nextUs;
        private int status;
        private Object output;
        private String where = "";

        Replay(Map<String, Object> scenario) {
            this.scenario = scenario;
        }

        @SuppressWarnings("unchecked")
        void run() {
            Map<String, Object> opening = (Map<String, Object>) scenario.get("open");
            // Scenarios run on the simulator only, so no replay can reach a robot's board.
            assertEquals("KEEL_BACKEND_SIMULATOR", opening.get("backend"), "a scenario must open the simulator");
            List<OpenFlag> flags = new ArrayList<>();
            for (Object flag : (List<Object>) opening.getOrDefault("flags", List.of())) {
                flags.add(OpenFlag.valueOf(((String) flag).substring("KEEL_OPEN_".length())));
            }
            try (Device opened = Keel.open(OpenOptions.builder().backend(Backend.SIMULATOR)
                    .flags(flags.toArray(OpenFlag[]::new)).build())) {
                device = opened;
                cycleId = device.snapshot().lastCycleId();
                nextUs = device.nowUs();
                int number = 0;
                for (Object item : (List<Object>) scenario.get("steps")) {
                    Map<String, Object> step = (Map<String, Object>) item;
                    where = scenario.get("name") + " step " + ++number;
                    if (step.containsKey("call")) {
                        call((String) step.get("call"), (List<Object>) step.getOrDefault("args", List.of()),
                                (String) step.getOrDefault("thread", "owner"));
                    } else if (step.containsKey("cycles")) {
                        cycles(((Long) step.get("cycles")).intValue(), (String) step.getOrDefault("command",
                                "KEEL_CYCLE_NONE"), number(step.getOrDefault("left", 0L)),
                                number(step.getOrDefault("right", 0L)));
                    } else if (step.containsKey("sim")) {
                        call("keel_sim_set", List.of(step.get("sim"), step.get("value")), "owner");
                    } else if (step.containsKey("expect")) {
                        expect(step);
                    } else {
                        fail(where + ": unknown step " + step);
                    }
                }
            }
        }

        private static int number(Object value) {
            return ((Long) value).intValue();
        }

        private Object argument(Object value) {
            if (value instanceof String name) {
                return EnumeratorTable.resolve(name);
            }
            if (value instanceof List<?> items) {
                return items.stream().map(this::argument).toList();
            }
            if (value instanceof Map<?, ?> special) {
                if ("next".equals(special.get("cycle_id"))) {
                    return ++cycleId;
                }
                if (special.containsKey("now_plus_us")) {
                    return device.nowUs() + (Long) special.get("now_plus_us");
                }
                fail(where + ": unknown argument " + value);
            }
            return value;
        }

        private void call(String name, List<Object> raw, String thread) {
            List<Object> arguments = raw.stream().map(this::argument).toList();
            if (thread.equals("other")) {
                AtomicReference<RuntimeException> failure = new AtomicReference<>();
                Thread worker = Thread.ofPlatform().start(() -> {
                    try {
                        invoke(name, arguments);
                    } catch (RuntimeException error) {
                        failure.set(error);
                    }
                });
                try {
                    worker.join();
                } catch (InterruptedException interruption) {
                    Thread.currentThread().interrupt();
                    fail(interruption);
                }
                if (failure.get() != null) {
                    throw failure.get();
                }
            } else {
                invoke(name, arguments);
            }
        }

        private static int integer(List<Object> arguments, int index) {
            return (int) (long) (Long) arguments.get(index);
        }

        /** One call through the public API; the status is the code of the exception it threw, or OK. */
        private void invoke(String name, List<Object> a) {
            status = 0;
            output = null;
            try {
                output = switch (name) {
                    case "keel_abi_version" -> (long) (Keel.abiVersion().major() << 16 | Keel.abiVersion().minor());
                    case "keel_version_string" -> Keel.versionString();
                    case "keel_status_name" -> Keel.statusName(integer(a, 0));
                    case "keel_status_string" -> Keel.statusString(integer(a, 0));
                    case "keel_status_class" -> (long) Keel.statusClass(integer(a, 0)).code();
                    case "keel_fault_cause_name" -> Keel.faultCauseName(integer(a, 0));
                    case "keel_close" -> closeDevice();
                    case "keel_get_capabilities" -> (long) device.capabilitiesMask();
                    case "keel_now_us" -> device.nowUs();
                    case "keel_sleep_until" -> run(() -> device.sleepUntil((Long) a.get(0)));
                    case "keel_tick" -> run(() -> device.tick((Long) a.get(0), integer(a, 1)));
                    case "keel_cycle" -> device.cycle(new CycleInput(integer(a, 0), (Long) a.get(1), integer(a, 2),
                            integer(a, 3), integer(a, 4), integer(a, 5)));
                    case "keel_request_arm" -> run(device::requestArm);
                    case "keel_disarm" -> run(device::disarm);
                    case "keel_emergency_stop" -> run(device::emergencyStop);
                    case "keel_clear_fault" -> run(device::clearFault);
                    case "keel_get_state" -> (long) device.state().code();
                    case "keel_get_snapshot" -> device.snapshot();
                    case "keel_get_fault" -> device.fault();
                    case "keel_read_safety_inputs" -> (long) device.readSafetyInputsMask();
                    case "keel_set_motors" -> run(() -> device.setMotors(integer(a, 0), integer(a, 1)));
                    case "keel_brake" -> run(device::brake);
                    case "keel_servo_start" -> run(() -> device.servoStart(integer(a, 0), integer(a, 1)));
                    case "keel_servo_set_pulse" -> run(() -> device.servoSetPulse(integer(a, 0)));
                    case "keel_servo_end" -> run(device::servoEnd);
                    case "keel_range_start" -> run(device::rangeStart);
                    case "keel_range_poll" -> device.rangePoll();
                    case "keel_range_measure" -> device.rangeMeasure();
                    case "keel_set_rgb" -> run(() -> device.setRgb(integer(a, 0), integer(a, 1)));
                    case "keel_set_rgb_all" -> run(() -> device.setRgbAll(integer(a, 0)));
                    case "keel_set_rgb_frame" -> run(() -> device.setRgbFrame(((List<?>) a.get(0)).stream()
                            .mapToInt(colour -> (int) (long) (Long) colour).toArray()));
                    case "keel_get_rgb" -> Integer.toUnsignedLong(device.rgb(integer(a, 0)));
                    case "keel_set_button_leds" -> run(() -> device.setButtonLeds(integer(a, 0), integer(a, 1)));
                    case "keel_take_buttons" -> device.takeButtons();
                    case "keel_sim_set" -> run(() -> device.simSet(SimItem.of(integer(a, 0)), integer(a, 1)));
                    case "keel_sim_get_state" -> device.simState();
                    default -> fail(where + ": no method for " + name);
                };
            } catch (KeelException error) {
                status = error.code();
                output = error.snapshot().orElse(null);
            }
        }

        private static Object run(Runnable call) {
            call.run();
            return null;
        }

        private Object closeDevice() {
            device.close();
            status = device.closeStatus().orElseThrow().code();
            return device.finalFault().orElse(null);
        }

        private void cycles(int count, String command, int left, int right) {
            int first = 0;
            for (int i = 0; i < count; i++) {
                int result = 0;
                try {
                    device.cycle(new CycleInput(Health.ALL.mask(), ++cycleId, (int) EnumeratorTable.resolve(command),
                            left, right, 0));
                } catch (KeelException error) {
                    result = error.code();
                }
                if (first == 0) {
                    first = result;
                }
                nextUs = Math.max(nextUs + Limits.TICK_PERIOD_MS * 1000L, device.nowUs());
                device.sleepUntil(nextUs);
            }
            status = first;
            output = null;
        }

        @SuppressWarnings("unchecked")
        private void expect(Map<String, Object> step) {
            assertEquals(EnumeratorTable.resolve((String) step.get("expect")), status,
                    where + ": status " + Keel.statusName(status));
            if (step.containsKey("output")) {
                check(output, step.get("output"), where + " output");
            }
            if (step.containsKey("state")) {
                assertEquals(EnumeratorTable.resolve((String) step.get("state")), device.state().code(),
                        where + ": state");
            }
            if (step.containsKey("fault")) {
                Map<String, Object> wanted = (Map<String, Object>) step.get("fault");
                Fault fault = device.fault();
                assertEquals(EnumeratorTable.resolve((String) wanted.get("cause")), fault.causeValue(),
                        where + ": cause");
                assertEquals(EnumeratorTable.resolve((String) wanted.get("status")), fault.statusValue(),
                        where + ": fault status");
                long flags = 0;
                for (Object flag : (List<Object>) wanted.get("flags")) {
                    flags |= EnumeratorTable.resolve((String) flag);
                }
                assertEquals(flags, fault.flagsValue(), where + ": fault flags");
            }
            if (step.containsKey("snapshot")) {
                check(device.snapshot(), step.get("snapshot"), where + " snapshot");
            }
        }

        private void check(Object actual, Object expected, String at) {
            if (expected instanceof Map<?, ?> fields) {
                for (Map.Entry<?, ?> field : fields.entrySet()) {
                    check(component(actual, (String) field.getKey(), at), field.getValue(), at + "." + field.getKey());
                }
            } else if (expected instanceof List<?> names) {
                long bits = 0;
                for (Object name : names) {
                    bits |= EnumeratorTable.resolve((String) name);
                }
                long value = ((Number) actual).longValue();
                assertEquals(bits, value & bits, at + ": " + value + " lacks " + names);
            } else if (expected instanceof String text && actual instanceof String) {
                assertEquals(text, actual, at);
            } else {
                long value = expected instanceof String name ? EnumeratorTable.resolve(name) : (Long) expected;
                assertTrue(actual instanceof Number, at + ": " + actual);
                assertEquals(value, ((Number) actual).longValue(), at);
            }
        }

        /** A record component by C field name: the raw {@code xValue} component of a typed field, else {@code x}. */
        private static Object component(Object record, String field, String at) {
            String camel = camel(field);
            for (String name : new String[] {camel + "Value", camel}) {
                try {
                    Method accessor = record.getClass().getMethod(name);
                    return accessor.invoke(record);
                } catch (NoSuchMethodException absent) {
                    // Try the plain name next.
                } catch (ReflectiveOperationException error) {
                    throw new AssertionError(at, error);
                }
            }
            return fail(at + ": " + record.getClass().getSimpleName() + " has no field " + field);
        }

        private static String camel(String field) {
            StringBuilder result = new StringBuilder();
            boolean upper = false;
            for (char character : field.toCharArray()) {
                if (character == '_') {
                    upper = true;
                } else {
                    result.append(upper ? Character.toUpperCase(character) : character);
                    upper = false;
                }
            }
            return result.toString();
        }
    }
}
