// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import io.github.bisocmorg.keel.Abi;
import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.OpenFlag;
import io.github.bisocmorg.keel.OpenResource;
import io.github.bisocmorg.keel.OpenWarning;
import io.github.bisocmorg.keel.TimingClass;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.errors.StatusClass;
import io.github.bisocmorg.keel.loop.CycleCommand;
import io.github.bisocmorg.keel.loop.CycleFlag;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.peripherals.Led;
import io.github.bisocmorg.keel.peripherals.RangeStatus;
import io.github.bisocmorg.keel.peripherals.SensorPower;
import io.github.bisocmorg.keel.sim.SimFlag;
import io.github.bisocmorg.keel.sim.SimItem;
import io.github.bisocmorg.keel.state.AckPhase;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.state.FaultFlag;
import io.github.bisocmorg.keel.state.SafetyInput;
import io.github.bisocmorg.keel.state.SnapshotFlag;
import io.github.bisocmorg.keel.state.State;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The binding's types by C enum, with the prefix each member drops: an oracle written independently of the
 * generator, and the resolver of C enumerator names for the conformance scenarios.
 */
public final class EnumeratorTable {
    /** C enum name to its Java type and the prefix stripped from each enumerator. */
    public static final Map<String, Binding> ENUMS;

    /** Every C enumerator name to its value, read from the binding's types. */
    public static final Map<String, Long> VALUES;

    /** One C enum: its Java type and the prefix its members drop. */
    public record Binding(Class<?> type, String prefix) {
    }

    static {
        Map<String, Binding> enums = new LinkedHashMap<>();
        enums.put("keel_abi", new Binding(Abi.class, "KEEL_ABI_"));
        enums.put("keel_status", new Binding(Status.class, "KEEL_"));
        enums.put("keel_status_class", new Binding(StatusClass.class, "KEEL_CLASS_"));
        enums.put("keel_state", new Binding(State.class, "KEEL_STATE_"));
        enums.put("keel_ack_phase", new Binding(AckPhase.class, "KEEL_ACK_"));
        enums.put("keel_health", new Binding(Health.class, "KEEL_HEALTH_"));
        enums.put("keel_button", new Binding(Button.class, "KEEL_BUTTON_"));
        enums.put("keel_led", new Binding(Led.class, "KEEL_LED_"));
        enums.put("keel_input", new Binding(SafetyInput.class, "KEEL_INPUT_"));
        enums.put("keel_capability", new Binding(Capability.class, "KEEL_CAP_"));
        enums.put("keel_backend", new Binding(Backend.class, "KEEL_BACKEND_"));
        enums.put("keel_open_flag", new Binding(OpenFlag.class, "KEEL_OPEN_"));
        enums.put("keel_open_warning", new Binding(OpenWarning.class, "KEEL_OPEN_WARN_"));
        enums.put("keel_resource", new Binding(OpenResource.class, "KEEL_RESOURCE_"));
        enums.put("keel_fault_cause", new Binding(FaultCause.class, "KEEL_CAUSE_"));
        enums.put("keel_fault_flag", new Binding(FaultFlag.class, "KEEL_FAULT_"));
        enums.put("keel_snapshot_flag", new Binding(SnapshotFlag.class, "KEEL_SNAP_"));
        enums.put("keel_sensor_power", new Binding(SensorPower.class, "KEEL_SENSOR_"));
        enums.put("keel_timing_class", new Binding(TimingClass.class, "KEEL_TIMING_"));
        enums.put("keel_range_status", new Binding(RangeStatus.class, "KEEL_RANGE_"));
        enums.put("keel_cycle_command", new Binding(CycleCommand.class, "KEEL_CYCLE_"));
        enums.put("keel_cycle_flag", new Binding(CycleFlag.class, "KEEL_CYCLE_"));
        enums.put("keel_limit", new Binding(Limits.class, "KEEL_"));
        enums.put("keel_sim_item", new Binding(SimItem.class, "KEEL_SIM_"));
        enums.put("keel_sim_flag", new Binding(SimFlag.class, "KEEL_SIM_"));
        ENUMS = Collections.unmodifiableMap(enums);

        Map<String, Long> values = new LinkedHashMap<>();
        for (Binding binding : enums.values()) {
            if (binding.type().isEnum()) {
                for (Object constant : binding.type().getEnumConstants()) {
                    String member = ((Enum<?>) constant).name();
                    if (!member.equals("UNRECOGNIZED")) {
                        values.put(binding.prefix() + member, (long) value(binding.type(), member));
                    }
                }
            } else {
                for (Field field : binding.type().getFields()) {
                    if (Modifier.isStatic(field.getModifiers()) && field.getType() == int.class) {
                        values.put(binding.prefix() + field.getName(), (long) value(binding.type(), field.getName()));
                    }
                }
            }
        }
        VALUES = Collections.unmodifiableMap(values);
    }

    private EnumeratorTable() {
    }

    /**
     * The value of one member of a binding type: {@code code()} for value enums, {@code mask()} for flag enums and
     * the constant for {@link Abi} and {@link Limits}.
     */
    public static int value(Class<?> type, String member) {
        try {
            if (!type.isEnum()) {
                return type.getField(member).getInt(null);
            }
            Object constant = type.getField(member).get(null);
            for (String accessor : new String[] {"code", "mask"}) {
                try {
                    return (int) type.getMethod(accessor).invoke(constant);
                } catch (NoSuchMethodException absent) {
                    // A value enum has code(), a flag enum mask().
                }
            }
            throw new AssertionError(type + " has neither code() nor mask()");
        } catch (ReflectiveOperationException error) {
            throw new AssertionError(type.getSimpleName() + "." + member, error);
        }
    }

    /** The value of a C enumerator name such as {@code "KEEL_E_CONFLICT"}. */
    public static long resolve(String name) {
        Long value = VALUES.get(name);
        if (value == null) {
            throw new AssertionError("no enumerator named " + name);
        }
        return value;
    }
}
