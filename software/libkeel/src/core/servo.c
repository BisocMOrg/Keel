/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Servo bursts on J5 (SPEC section 14), with CONTROL held throughout.
 *
 * The MCU accepts a servo request rise only while ARMED with no PWM activity
 * and outside its 2 s cooldown, grants a lease of at most 500 ms and treats
 * motors and servo as mutually exclusive. The host keeps inside that
 * contract with margins: leases of at most 400 ms, a 2200 ms cooldown, 20 ms
 * of coasting before a request, and a pulse that starts only once the grant
 * is confirmed, so no signal is driven into an unpowered servo.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "core/internal.h"

/* Moves the earliest next burst to @p from + the host cooldown, never earlier. */
static void push_cooldown(keel__device *dev, uint64_t from)
{
    uint64_t until = from + KEEL__MS(KEEL_SERVO_COOLDOWN_MS);
    if (until > dev->servo.cooldown_until_us) {
        dev->servo.cooldown_until_us = until;
    }
}

void keel__servo_init(keel__device *dev, uint64_t now)
{
    memset(&dev->servo, 0, sizeof dev->servo);
    /* The host cannot see when the MCU booted; its cooldown runs from boot. */
    push_cooldown(dev, now);
}

void keel__servo_ready_low(keel__device *dev, uint64_t now)
{
    push_cooldown(dev, now);
}

static int32_t servo_write(keel__device *dev, uint32_t mode, uint32_t pulse_us)
{
    keel_port_servo_command command;
    memset(&command, 0, sizeof command);
    command.mode = mode;
    command.pulse_us = pulse_us;
    command.stop_epoch = dev->handled_epoch;
    command.valid_until_us = mode == KEEL_PORT_SERVO_OFF ? 0u : dev->servo.lease_end_us;
    return dev->be.servo(dev->be.user, &command);
}

int32_t keel__servo_off(keel__device *dev)
{
    int32_t rc;
    if (!dev->be.servo) {
        return KEEL_OK;
    }
    rc = servo_write(dev, KEEL_PORT_SERVO_OFF, 0);
    if (dev->servo.active) {
        /* The MCU starts its 2000 ms cooldown at the request-LOW edge, which
         * this call has completed; timing from here keeps the host later
         * even on a fast MCU clock. */
        dev->servo.active = 0;
        dev->servo.granted = 0;
        push_cooldown(dev, keel__now(dev));
    }
    return rc;
}

/* Pulse off and request LOW, then ENABLE LOW: a burst always ends disarmed,
 * and driving again needs a new acknowledgement. */
static int32_t end_burst(keel__device *dev)
{
    int32_t rc = keel__servo_off(dev);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, 0);
    }
    rc = keel__enable_low(dev);
    if (rc == KEEL_OK) {
        keel__enter_disarmed(dev);
    }
    return rc;
}

int32_t keel__servo_start(keel__device *dev, uint32_t pulse_us, uint32_t lease_ms)
{
    keel__servo *s = &dev->servo;
    uint64_t now;
    int32_t rc;

    if (!keel__is_owner(dev)) {
        return keel__reject(dev, KEEL_E_CONFLICT);
    }
    if (dev->state == KEEL_STATE_FAULTED) {
        return KEEL_E_FAULTED;
    }
    if (dev->state == KEEL_STATE_SERVO) {
        return keel__reject(dev, KEEL_E_CONFLICT); /* a lease is never extended */
    }
    if (dev->state != KEEL_STATE_ARMED) {
        return keel__reject(dev, KEEL_E_STATE);
    }
    /* Driving, braking and a pending reversal are all PWM activity to the MCU. */
    if (keel__motion_active(dev)) {
        return keel__reject(dev, KEEL_E_CONFLICT);
    }
    now = keel__now(dev);
    if (now < s->cooldown_until_us ||
        now - dev->motion.coast_since_us < KEEL__MS(KEEL_SERVO_IDLE_MS)) {
        return keel__reject(dev, KEEL_E_AGAIN);
    }
    rc = keel__armed_io(dev, &now);
    if (rc != KEEL_OK) {
        return rc;
    }
    keel__motion_drop(dev); /* the stored command and its timeout end here */
    s->lease_end_us = now + KEEL__MS(lease_ms);
    s->pulse_us = pulse_us;
    s->granted = 0;
    s->active = 1;
    rc = servo_write(dev, KEEL_PORT_SERVO_REQUEST, 0);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, 0);
    }
    /* The grant window runs from the completed write. */
    rc = keel__clock(dev, &s->requested_us);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev->state = KEEL_STATE_SERVO;
    return KEEL_OK;
}

int32_t keel__servo_set_pulse(keel__device *dev, uint32_t pulse_us)
{
    keel__servo *s = &dev->servo;
    uint64_t now;
    int32_t rc;

    if (!keel__is_owner(dev)) {
        return keel__reject(dev, KEEL_E_CONFLICT);
    }
    if (dev->state == KEEL_STATE_FAULTED) {
        return KEEL_E_FAULTED;
    }
    if (dev->state != KEEL_STATE_SERVO) {
        return keel__reject(dev, KEEL_E_STATE);
    }
    rc = keel__fresh_inputs(dev, &now);
    if (rc != KEEL_OK) {
        return rc;
    }
    s->pulse_us = pulse_us;
    /* Before the grant the width is only stored; after the lease the next
     * tick ends the burst. The lease itself never changes. */
    if (!s->granted || now >= s->lease_end_us) {
        return KEEL_OK;
    }
    rc = servo_write(dev, KEEL_PORT_SERVO_PULSE, pulse_us);
    return rc == KEEL_OK ? KEEL_OK : keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, 0);
}

int32_t keel__servo_end(keel__device *dev)
{
    uint64_t now;
    int32_t rc;
    if (dev->state != KEEL_STATE_SERVO) {
        return KEEL_OK;
    }
    rc = end_burst(dev);
    if (rc != KEEL_OK) {
        return rc;
    }
    /* A stop call checks the backend latch after its stop (SPEC 12.3). */
    return keel__fresh_inputs(dev, &now);
}

int32_t keel__servo_tick(keel__device *dev, uint64_t now)
{
    keel__servo *s = &dev->servo;
    int32_t rc;
    if (now >= s->lease_end_us) {
        return end_burst(dev);
    }
    if (!s->granted) {
        if (dev->inputs & KEEL_IN_ARMED) {
            /* The MCU drops the motor permit on the step after the rise. */
            return now - s->requested_us > KEEL__MS(KEEL_SERVO_GRANT_MS)
                       ? keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_SERVO_NOT_GRANTED, 0, 0)
                       : KEEL_OK;
        }
        /* ARMED = 0 with READY = 1: the MCU granted the burst and powers J5. */
        rc = servo_write(dev, KEEL_PORT_SERVO_PULSE, s->pulse_us);
        if (rc != KEEL_OK) {
            return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, 0);
        }
        s->granted = 1;
        return KEEL_OK;
    }
    if (dev->inputs & KEEL_IN_ARMED) {
        /* Motor and servo permits at once: the MCU's exclusion has failed. */
        return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_DUAL_PERMIT, 0, 0);
    }
    return KEEL_OK;
}
