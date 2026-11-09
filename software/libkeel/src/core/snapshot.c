/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Snapshot assembly (SPEC section 20) from cached state, with CONTROL held.
 * No I/O: the only backend calls are the clock and the timing statistics;
 * ranging contributes through atomics, never through the RANGE lock.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "core/internal.h"

/* SPEC 9.6: the Pi has no sensor-power feedback, so power is inferred from
 * the heartbeat and READY, and confirmed by a recent echo. */
uint32_t keel__sensor_power(const keel__device *dev, uint64_t now)
{
    if (dev->state == KEEL_STATE_FAULTED || !dev->have_fall ||
        now - dev->last_fall_us > KEEL__MS(KEEL_TICK_WINDOW_MS)) {
        /* The MCU's 80 ms window ends earlier on a fast MCU clock. */
        return KEEL_SENSOR_OFF;
    }
    if (dev->ready_low) {
        return now - dev->ready_low_since_us >= KEEL__MS(KEEL__READY_LOW_OFF_MS)
                   ? KEEL_SENSOR_OFF
                   : KEEL_SENSOR_UNKNOWN;
    }
    if (dev->hb_falls < 2u) {
        return KEEL_SENSOR_UNKNOWN;
    }
    if (atomic_load(&dev->echo_seen)) {
        uint64_t rise = atomic_load(&dev->echo_rise_us);
        if (now >= rise && now - rise <= KEEL__MS(KEEL__ECHO_CONFIRM_MS)) {
            return KEEL_SENSOR_CONFIRMED;
        }
    }
    return KEEL_SENSOR_PERMITTED;
}

static uint32_t snapshot_flags(const keel__device *dev, uint64_t now)
{
    const keel__motion *m = &dev->motion;
    uint32_t flags = 0;
    if (dev->state != KEEL_STATE_FAULTED && dev->have_fall &&
        now - dev->last_fall_us <= KEEL__MS(KEEL__MCU_HEARTBEAT_MS)) {
        flags |= KEEL_SNAP_HEARTBEAT_RUNNING;
    }
    if (m->expired) {
        flags |= KEEL_SNAP_COMMAND_EXPIRED;
    }
    if (m->pending) {
        flags |= KEEL_SNAP_REVERSAL_PENDING;
    }
    if (m->mode == KEEL_MOTOR_BRAKE) {
        flags |= KEEL_SNAP_BRAKING;
    }
    if (dev->hb_restarted) {
        flags |= KEEL_SNAP_HEARTBEAT_RESTARTED;
    }
    if (dev->have_latch && now - dev->latched_at_us < KEEL__MS(KEEL_REARM_HOLDOFF_MS)) {
        flags |= KEEL_SNAP_REARM_HOLDOFF;
    }
    if (dev->caps & KEEL_CAP_SIMULATED) {
        flags |= KEEL_SNAP_SIMULATED;
    }
    if (dev->record.flags & KEEL_FAULT_STICKY) {
        flags |= KEEL_SNAP_FAULT_STICKY;
    }
    if (dev->state == KEEL_STATE_SERVO && dev->servo.granted) {
        flags |= KEEL_SNAP_SERVO_GRANTED;
    }
    return flags;
}

static int32_t clamp_i32(int64_t value)
{
    return value > INT32_MAX ? INT32_MAX : value < INT32_MIN ? INT32_MIN : (int32_t)value;
}

static uint32_t remaining_ms(uint64_t until, uint64_t now)
{
    uint64_t ms = until > now ? (until - now) / 1000u : 0u;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

void keel__snapshot(const keel__device *dev, keel_snapshot *s)
{
    const keel__motion *m = &dev->motion;
    keel_port_stats stats;
    uint64_t now = keel__now(dev);

    memset(s, 0, sizeof *s);
    s->struct_size = sizeof *s;
    s->state = dev->state;
    s->flags = snapshot_flags(dev, now);
    s->inputs = dev->inputs & (KEEL_IN_READY | KEEL_IN_ARMED);
    s->time_us = now;
    s->inputs_sampled_us = dev->inputs_sampled_us;
    s->last_cycle_id = dev->last_cycle_id;
    s->owner_thread_id = dev->have_owner ? dev->owner_thread : 0u;
    if (dev->have_tick) {
        s->tick_budget_us = clamp_i32((int64_t)(dev->last_tick_us + KEEL__MS(KEEL_TICK_WINDOW_MS)) -
                                      (int64_t)now);
    }
    s->ack_phase = dev->ack_phase;
    s->command_left = m->pending ? m->pending_command[0] : m->command[0];
    s->command_right = m->pending ? m->pending_command[1] : m->command[1];
    if (m->mode == KEEL_MOTOR_DRIVE) {
        s->output_left = m->command[0];
        s->output_right = m->command[1];
    }
    if (m->pending) {
        s->command_remaining_ms = remaining_ms(m->pending_expiry_us, now);
    } else if (m->mode != KEEL_MOTOR_COAST) {
        s->command_remaining_ms = remaining_ms(m->expiry_us, now);
    }
    s->buttons_held = dev->buttons.held;
    s->buttons_pressed = dev->buttons.pressed;
    s->buttons_released = dev->buttons.released;
    s->button_leds = dev->button_leds;
    s->sensor_power = keel__sensor_power(dev, now);
    if (dev->state == KEEL_STATE_SERVO) {
        s->servo_pulse_us = dev->servo.pulse_us;
        s->servo_lease_remaining_ms = remaining_ms(dev->servo.lease_end_us, now);
    }
    if (dev->caps & KEEL_CAP_SERVO) {
        s->servo_cooldown_remaining_ms = remaining_ms(dev->servo.cooldown_until_us, now);
    }
    s->fault_cause = dev->record.cause;
    s->last_rejection = atomic_load(&dev->last_rejection);
    s->heartbeat_restarts = dev->heartbeat_restarts;
    s->stale_ack_restarts = dev->stale_ack_restarts;
    s->fault_count = dev->fault_count;
    memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;
    if (dev->be.get_stats && dev->be.get_stats(dev->be.user, &stats) == KEEL_OK) {
        s->timing_class = stats.timing_class;
        s->pwm_late_wakes = stats.late_wakes;
        s->pwm_max_lateness_us = stats.max_lateness_us;
        s->pwm_lease_expiries = stats.lease_expiries;
    }
}
