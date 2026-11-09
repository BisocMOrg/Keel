/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Motion (SPEC section 10): signed permille duties, the deadman timeout, the
 * 20 ms reversal dwell (a reversal is deferred, never refused), brake,
 * and the lease the backend enforces between ticks. All functions run with
 * CONTROL held.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "core/internal.h"

static int32_t direction_of(int32_t duty)
{
    return (duty > 0) - (duty < 0);
}

/* A wheel's idle time starts at the first coast or brake, never later. */
static void mark_idle(keel__device *dev, uint64_t at)
{
    unsigned i;
    for (i = 0; i < 2; ++i) {
        if (!dev->motion.idle[i]) {
            dev->motion.idle[i] = 1;
            dev->motion.idle_since_us[i] = at;
        }
    }
}

int keel__duty_valid(int32_t duty)
{
    return duty >= -KEEL_DUTY_MAX && duty <= KEEL_DUTY_MAX;
}

void keel__motion_init(keel__device *dev, uint64_t now)
{
    keel__motion_drop(dev);
    dev->motion.coasting = 1;
    dev->motion.coast_since_us = now;
}

int32_t keel__coast_write(keel__device *dev)
{
    keel_port_motor_command command;
    uint64_t done;
    int32_t rc;
    memset(&command, 0, sizeof command);
    command.mode = KEEL_MOTOR_COAST;
    command.stop_epoch = dev->handled_epoch;
    rc = dev->be.write_motors(dev->be.user, &command);
    /* The dwell runs from the completed write, including any time the
     * backend blocked, so a slow coast can never shorten it. */
    done = keel__now(dev);
    mark_idle(dev, done);
    if (!dev->motion.coasting) {
        dev->motion.coasting = 1;
        dev->motion.coast_since_us = done;
    }
    return rc;
}

int keel__motion_active(const keel__device *dev)
{
    return dev->motion.mode != KEEL_MOTOR_COAST || dev->motion.pending;
}

void keel__motion_drop(keel__device *dev)
{
    keel__motion *m = &dev->motion;
    m->mode = KEEL_MOTOR_COAST;
    m->command[0] = m->command[1] = 0;
    m->expiry_us = 0;
    m->pending = 0;
    m->pending_command[0] = m->pending_command[1] = 0;
    m->pending_expiry_us = 0;
    mark_idle(dev, keel__now(dev));
}

/* A direction change needs the wheel idle for the dwell first. */
static int reverses(const keel__device *dev, const int32_t duty[2], uint64_t now)
{
    const keel__motion *m = &dev->motion;
    unsigned i;
    for (i = 0; i < 2; ++i) {
        int32_t direction = direction_of(duty[i]);
        if (direction && m->direction[i] && direction != m->direction[i] &&
            (!m->idle[i] || now - m->idle_since_us[i] < KEEL__MS(KEEL_REVERSAL_DWELL_MS))) {
            return 1;
        }
    }
    return 0;
}

/* The backend coasts at the lease end unless a tick renews it; the lease
 * never outlives the command or the 70 ms tick window. */
static uint64_t lease_end(const keel__device *dev, uint64_t expiry)
{
    uint64_t window = dev->last_tick_us + KEEL__MS(KEEL_TICK_WINDOW_MS);
    return expiry < window ? expiry : window;
}

/* Writes DRIVE or BRAKE; a write that blocked past the tick window fails.
 * @p done receives the completion time. */
static int32_t write_command(keel__device *dev, uint32_t mode, const int32_t duty[2],
                             uint64_t expiry, uint64_t *done)
{
    keel_port_motor_command command;
    int32_t rc;
    memset(&command, 0, sizeof command);
    command.mode = mode;
    command.left = duty[0];
    command.right = duty[1];
    command.stop_epoch = dev->handled_epoch;
    command.valid_until_us = lease_end(dev, expiry);
    rc = dev->be.write_motors(dev->be.user, &command);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_MOTOR_WRITE, 0);
    }
    rc = keel__clock(dev, done);
    return rc != KEEL_OK ? rc : keel__window(dev, *done);
}

static int32_t drive(keel__device *dev, const int32_t duty[2], uint64_t expiry)
{
    keel__motion *m = &dev->motion;
    uint64_t done;
    unsigned i;
    int32_t rc = write_command(dev, KEEL_MOTOR_DRIVE, duty, expiry, &done);
    if (rc != KEEL_OK) {
        return rc;
    }
    m->mode = KEEL_MOTOR_DRIVE;
    m->command[0] = duty[0];
    m->command[1] = duty[1];
    m->expiry_us = expiry;
    m->pending = 0;
    m->expired = 0;
    m->coasting = 0;
    for (i = 0; i < 2; ++i) {
        if (duty[i]) {
            m->direction[i] = direction_of(duty[i]);
            m->idle[i] = 0;
        } else if (!m->idle[i]) {
            m->idle[i] = 1;
            m->idle_since_us[i] = done;
        }
    }
    return KEEL_OK;
}

/* Drives now, or coasts both motors and defers a reversal until the first
 * tick or command after the dwell (a newer command replaces it). */
static int32_t command(keel__device *dev, const int32_t duty[2], uint64_t expiry, uint64_t now)
{
    keel__motion *m = &dev->motion;
    int32_t rc;
    if (!reverses(dev, duty, now)) {
        return drive(dev, duty, expiry);
    }
    rc = keel__coast_write(dev);
    if (rc != KEEL_OK) {
        keel__motion_drop(dev);
        return keel__hook_failed(dev, rc, KEEL_CAUSE_MOTOR_WRITE, 0);
    }
    m->mode = KEEL_MOTOR_COAST;
    m->command[0] = m->command[1] = 0;
    m->expiry_us = 0;
    m->pending = 1;
    m->pending_command[0] = duty[0];
    m->pending_command[1] = duty[1];
    m->pending_expiry_us = expiry;
    m->expired = 0;
    return KEEL_OK;
}

/* Thread, FAULTED and state checks of a go call, from cached state only. */
static int32_t admit(keel__device *dev)
{
    if (!keel__is_owner(dev)) {
        return keel__reject(dev, KEEL_E_CONFLICT);
    }
    if (dev->state == KEEL_STATE_FAULTED) {
        return KEEL_E_FAULTED;
    }
    if (dev->state == KEEL_STATE_SERVO) {
        return keel__reject(dev, KEEL_E_CONFLICT);
    }
    if (dev->state != KEEL_STATE_ARMED) {
        return keel__reject(dev, KEEL_E_STATE);
    }
    return KEEL_OK;
}

int32_t keel__armed_io(keel__device *dev, uint64_t *now)
{
    int32_t rc = keel__fresh_inputs(dev, now);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = keel__window(dev, *now);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!(dev->inputs & KEEL_IN_READY)) {
        return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_READY_LOST, 0, 0);
    }
    if (!(dev->inputs & KEEL_IN_ARMED)) {
        return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_ARMED_LOST, 0, 0);
    }
    return KEEL_OK;
}

int32_t keel__motion_refuse(keel__device *dev, int32_t status)
{
    if (dev->state == KEEL_STATE_ARMED) {
        /* A refused command must never leave the wheels driving. */
        int32_t rc = keel__coast_write(dev);
        keel__motion_drop(dev);
        dev->motion.expired = 0;
        if (rc != KEEL_OK) {
            return keel__hook_failed(dev, rc, KEEL_CAUSE_MOTOR_WRITE, 0);
        }
        rc = keel__sample(dev); /* a latched backend failure is the truth */
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    return keel__reject(dev, status);
}

int32_t keel__motion_zero(keel__device *dev)
{
    int32_t rc;
    if (dev->state != KEEL_STATE_ARMED) {
        return KEEL_OK;
    }
    rc = keel__coast_write(dev);
    keel__motion_drop(dev);
    dev->motion.expired = 0;
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_MOTOR_WRITE, 0);
    }
    return keel__sample(dev); /* a stop checks the backend latch after stopping */
}

int32_t keel__motion_set(keel__device *dev, int32_t left, int32_t right, const uint64_t *tick_now)
{
    int32_t duty[2];
    uint64_t now;
    int32_t rc = admit(dev);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (tick_now) {
        now = *tick_now; /* keel_cycle(): the tick's sample was just evaluated */
    } else {
        rc = keel__armed_io(dev, &now);
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    duty[0] = left;
    duty[1] = right;
    return command(dev, duty, now + dev->command_timeout_us, now);
}

int32_t keel__motion_brake(keel__device *dev, const uint64_t *tick_now)
{
    static const int32_t no_duty[2] = {0, 0};
    keel__motion *m = &dev->motion;
    uint64_t now, done;
    int32_t rc;
    if (!(dev->caps & KEEL_CAP_BRAKE)) {
        return keel__motion_refuse(dev, KEEL_E_UNSUPPORTED);
    }
    rc = admit(dev);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (tick_now) {
        now = *tick_now;
    } else {
        rc = keel__armed_io(dev, &now);
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    rc = write_command(dev, KEEL_MOTOR_BRAKE, no_duty, now + dev->command_timeout_us, &done);
    if (rc != KEEL_OK) {
        return rc;
    }
    m->mode = KEEL_MOTOR_BRAKE;
    m->command[0] = m->command[1] = 0;
    m->expiry_us = now + dev->command_timeout_us;
    m->pending = 0;
    m->expired = 0;
    /* Braking counts as idle time for a later reversal, but as PWM activity
     * for the MCU's servo grant. */
    m->coasting = 0;
    mark_idle(dev, done);
    return KEEL_OK;
}

int32_t keel__motion_tick(keel__device *dev, uint64_t now)
{
    keel__motion *m = &dev->motion;
    uint64_t done;
    int32_t rc;
    if (m->pending) {
        if (m->pending_expiry_us < now + KEEL__EXPIRY_MARGIN_US) {
            m->pending = 0;
            m->expired = 1;
            return KEEL_OK;
        }
        if (reverses(dev, m->pending_command, now)) {
            return KEEL_OK;
        }
        return drive(dev, m->pending_command, m->pending_expiry_us);
    }
    if (m->mode == KEEL_MOTOR_COAST) {
        return KEEL_OK;
    }
    if (m->expiry_us < now + KEEL__EXPIRY_MARGIN_US) {
        /* The deadman timeout is not a fault: coast and stay ARMED. */
        rc = keel__coast_write(dev);
        keel__motion_drop(dev);
        m->expired = 1;
        return rc == KEEL_OK ? KEEL_OK : keel__hook_failed(dev, rc, KEEL_CAUSE_MOTOR_WRITE, 0);
    }
    return write_command(dev, m->mode, m->command, m->expiry_us, &done);
}
