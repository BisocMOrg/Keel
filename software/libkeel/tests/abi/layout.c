/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Frozen struct layouts of the libkeel 1.x ABI and the 1.x porting interface.
 *
 * Compiled (never run) for every target in tests/abi/check_headers.sh:
 * x86-64, i386, aarch64 LP64 and ILP32 where the toolchain supports it, and
 * as C++11. Every field's offset is pinned, and each field must start exactly
 * where the previous one ends, so no compiler-inserted padding can exist on
 * any target. Changing a number here is an ABI break (new major version).
 */
#include <stddef.h>
#include <stdint.h>

#include "keel/keel.h"
#include "keel/port.h"

#ifdef __cplusplus
#define KEEL_LAYOUT_ASSERT(cond, msg) static_assert(cond, msg)
#define KEEL_MEMBER_SIZE(T, m) sizeof(T::m)
#else
#define KEEL_LAYOUT_ASSERT(cond, msg) _Static_assert(cond, msg)
#define KEEL_MEMBER_SIZE(T, m) sizeof(((T *)0)->m)
#endif

#define KEEL_MEMBER_END(T, m) (offsetof(T, m) + KEEL_MEMBER_SIZE(T, m))
/* The first field sits at offset 0. */
#define FIRST(T, m) KEEL_LAYOUT_ASSERT(offsetof(T, m) == 0, #T "." #m " must be at offset 0");
/* A field sits at its frozen offset, directly after the previous field. */
#define NEXT(T, prev, m, off)                                                                      \
    KEEL_LAYOUT_ASSERT(offsetof(T, m) == (off), #T "." #m " offset changed");                    \
    KEEL_LAYOUT_ASSERT(KEEL_MEMBER_END(T, prev) == offsetof(T, m), #T ": padding before " #m);
/* The struct ends directly after its last field, at its frozen size. */
#define LAST(T, m, size)                                                                           \
    KEEL_LAYOUT_ASSERT(sizeof(T) == (size), "sizeof " #T " changed");                             \
    KEEL_LAYOUT_ASSERT(KEEL_MEMBER_END(T, m) == sizeof(T), #T ": tail padding after " #m);

/* keel.h */
FIRST(keel_open_options, struct_size)
NEXT(keel_open_options, struct_size, backend, 4)
NEXT(keel_open_options, backend, flags, 8)
NEXT(keel_open_options, flags, command_timeout_ms, 12)
NEXT(keel_open_options, command_timeout_ms, gpiochip_path, 16)
NEXT(keel_open_options, gpiochip_path, i2c_path, 144)
LAST(keel_open_options, i2c_path, 272)

FIRST(keel_open_report, struct_size)
NEXT(keel_open_report, struct_size, status, 4)
NEXT(keel_open_report, status, resource, 8)
NEXT(keel_open_report, resource, os_error, 12)
NEXT(keel_open_report, os_error, line, 16)
NEXT(keel_open_report, line, backend, 20)
NEXT(keel_open_report, backend, capabilities, 24)
NEXT(keel_open_report, capabilities, warnings, 28)
NEXT(keel_open_report, warnings, timing_class, 32)
NEXT(keel_open_report, timing_class, realtime_os_error, 36)
NEXT(keel_open_report, realtime_os_error, path, 40)
NEXT(keel_open_report, path, consumer, 168)
NEXT(keel_open_report, consumer, hint, 200)
LAST(keel_open_report, hint, 456)

FIRST(keel_fault, struct_size)
NEXT(keel_fault, struct_size, status, 4)
NEXT(keel_fault, status, cause, 8)
NEXT(keel_fault, cause, state, 12)
NEXT(keel_fault, state, ack_phase, 16)
NEXT(keel_fault, ack_phase, inputs, 20)
NEXT(keel_fault, inputs, os_error, 24)
NEXT(keel_fault, os_error, cleanup_status, 28)
NEXT(keel_fault, cleanup_status, detail, 32)
NEXT(keel_fault, detail, time_us, 40)
NEXT(keel_fault, time_us, fault_count, 48)
NEXT(keel_fault, fault_count, flags, 52)
NEXT(keel_fault, flags, cleanup_os_error, 56)
NEXT(keel_fault, cleanup_os_error, reserved0, 60)
LAST(keel_fault, reserved0, 64)

FIRST(keel_snapshot, struct_size)
NEXT(keel_snapshot, struct_size, state, 4)
NEXT(keel_snapshot, state, flags, 8)
NEXT(keel_snapshot, flags, inputs, 12)
NEXT(keel_snapshot, inputs, time_us, 16)
NEXT(keel_snapshot, time_us, inputs_sampled_us, 24)
NEXT(keel_snapshot, inputs_sampled_us, last_cycle_id, 32)
NEXT(keel_snapshot, last_cycle_id, owner_thread_id, 40)
NEXT(keel_snapshot, owner_thread_id, tick_budget_us, 48)
NEXT(keel_snapshot, tick_budget_us, ack_phase, 52)
NEXT(keel_snapshot, ack_phase, command_left, 56)
NEXT(keel_snapshot, command_left, command_right, 60)
NEXT(keel_snapshot, command_right, output_left, 64)
NEXT(keel_snapshot, output_left, output_right, 68)
NEXT(keel_snapshot, output_right, command_remaining_ms, 72)
NEXT(keel_snapshot, command_remaining_ms, buttons_held, 76)
NEXT(keel_snapshot, buttons_held, buttons_pressed, 80)
NEXT(keel_snapshot, buttons_pressed, buttons_released, 84)
NEXT(keel_snapshot, buttons_released, button_leds, 88)
NEXT(keel_snapshot, button_leds, sensor_power, 92)
NEXT(keel_snapshot, sensor_power, servo_pulse_us, 96)
NEXT(keel_snapshot, servo_pulse_us, servo_lease_remaining_ms, 100)
NEXT(keel_snapshot, servo_lease_remaining_ms, servo_cooldown_remaining_ms, 104)
NEXT(keel_snapshot, servo_cooldown_remaining_ms, fault_cause, 108)
NEXT(keel_snapshot, fault_cause, last_rejection, 112)
NEXT(keel_snapshot, last_rejection, heartbeat_restarts, 116)
NEXT(keel_snapshot, heartbeat_restarts, stale_ack_restarts, 120)
NEXT(keel_snapshot, stale_ack_restarts, timing_class, 124)
NEXT(keel_snapshot, timing_class, pwm_late_wakes, 128)
NEXT(keel_snapshot, pwm_late_wakes, pwm_max_lateness_us, 132)
NEXT(keel_snapshot, pwm_max_lateness_us, pwm_lease_expiries, 136)
NEXT(keel_snapshot, pwm_lease_expiries, fault_count, 140)
LAST(keel_snapshot, fault_count, 144)

FIRST(keel_buttons, struct_size)
NEXT(keel_buttons, struct_size, held, 4)
NEXT(keel_buttons, held, pressed, 8)
NEXT(keel_buttons, pressed, released, 12)
LAST(keel_buttons, released, 16)

FIRST(keel_range, struct_size)
NEXT(keel_range, struct_size, status, 4)
NEXT(keel_range, status, distance_mm, 8)
NEXT(keel_range, distance_mm, echo_us, 12)
NEXT(keel_range, echo_us, measured_at_us, 16)
NEXT(keel_range, measured_at_us, sequence, 24)
NEXT(keel_range, sequence, reserved0, 28)
LAST(keel_range, reserved0, 32)

FIRST(keel_cycle_input, struct_size)
NEXT(keel_cycle_input, struct_size, health, 4)
NEXT(keel_cycle_input, health, cycle_id, 8)
NEXT(keel_cycle_input, cycle_id, command, 16)
NEXT(keel_cycle_input, command, left, 20)
NEXT(keel_cycle_input, left, right, 24)
NEXT(keel_cycle_input, right, flags, 28)
LAST(keel_cycle_input, flags, 32)

FIRST(keel_sim_state, struct_size)
NEXT(keel_sim_state, struct_size, pins, 4)
NEXT(keel_sim_state, pins, mcu_stage, 8)
NEXT(keel_sim_state, mcu_stage, mcu_faults, 12)
NEXT(keel_sim_state, mcu_faults, flags, 16)
NEXT(keel_sim_state, flags, bridge_left, 20)
NEXT(keel_sim_state, bridge_left, bridge_right, 24)
NEXT(keel_sim_state, bridge_right, servo_pulse_us, 28)
NEXT(keel_sim_state, servo_pulse_us, now_us, 32)
NEXT(keel_sim_state, now_us, heartbeat_falls, 40)
NEXT(keel_sim_state, heartbeat_falls, reserved0, 44)
LAST(keel_sim_state, reserved0, 48)

/* port.h value types (the backend table holds pointers and is target-specific). */
FIRST(keel_port_motor_command, mode)
NEXT(keel_port_motor_command, mode, left, 4)
NEXT(keel_port_motor_command, left, right, 8)
NEXT(keel_port_motor_command, right, stop_epoch, 12)
NEXT(keel_port_motor_command, stop_epoch, valid_until_us, 16)
LAST(keel_port_motor_command, valid_until_us, 24)

FIRST(keel_port_servo_command, mode)
NEXT(keel_port_servo_command, mode, pulse_us, 4)
NEXT(keel_port_servo_command, pulse_us, stop_epoch, 8)
NEXT(keel_port_servo_command, stop_epoch, reserved0, 12)
NEXT(keel_port_servo_command, reserved0, valid_until_us, 16)
LAST(keel_port_servo_command, valid_until_us, 24)

FIRST(keel_port_fault, struct_size)
NEXT(keel_port_fault, struct_size, status, 4)
NEXT(keel_port_fault, status, cause, 8)
NEXT(keel_port_fault, cause, os_error, 12)
NEXT(keel_port_fault, os_error, detail, 16)
NEXT(keel_port_fault, detail, time_us, 24)
LAST(keel_port_fault, time_us, 32)

FIRST(keel_port_stats, struct_size)
NEXT(keel_port_stats, struct_size, timing_class, 4)
NEXT(keel_port_stats, timing_class, wakes, 8)
NEXT(keel_port_stats, wakes, late_wakes, 16)
NEXT(keel_port_stats, late_wakes, very_late_wakes, 20)
NEXT(keel_port_stats, very_late_wakes, max_lateness_us, 24)
NEXT(keel_port_stats, max_lateness_us, last_lateness_us, 28)
NEXT(keel_port_stats, last_lateness_us, lease_expiries, 32)
NEXT(keel_port_stats, lease_expiries, suppressed_writes, 36)
LAST(keel_port_stats, suppressed_writes, 40)

FIRST(keel_port_edge, time_us)
NEXT(keel_port_edge, time_us, rising, 8)
NEXT(keel_port_edge, rising, reserved0, 12)
LAST(keel_port_edge, reserved0, 16)

/* The scalar head of the backend table is identical on every target. */
FIRST(keel_port_backend, struct_size)
NEXT(keel_port_backend, struct_size, port_version, 4)
NEXT(keel_port_backend, port_version, capabilities, 8)
NEXT(keel_port_backend, capabilities, flags, 12)
NEXT(keel_port_backend, flags, user, 16)

/* Fixed-size arrays match their documented enumerators. */
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_open_options, gpiochip_path) == KEEL_PATH_MAX, "path");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_open_options, i2c_path) == KEEL_PATH_MAX, "path");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_open_report, path) == KEEL_PATH_MAX, "path");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_open_report, consumer) == KEEL_CONSUMER_MAX, "consumer");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_open_report, hint) == KEEL_HINT_MAX, "hint");

/* Version identity: the packed ABI value follows the version macros. */
KEEL_LAYOUT_ASSERT(KEEL_ABI_VERSION == 0x00010000, "ABI version is 1.0");
KEEL_LAYOUT_ASSERT(KEEL_ABI_MAJOR == KEEL_VERSION_MAJOR, "ABI major");
KEEL_LAYOUT_ASSERT(KEEL_ABI_MINOR == KEEL_VERSION_MINOR, "ABI minor");
