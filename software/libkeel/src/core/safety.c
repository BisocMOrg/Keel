/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Safety state machine (SPEC sections 8, 9, 11 and 12): CONTROL sections and
 * the stop epoch, the clock check, fault latching with status pass-through
 * and backend-latch priority, the heartbeat, the arming acknowledgement and
 * the stop verbs.
 *
 * The heartbeat and acknowledgement timings: falls 30-70 ms apart with a
 * 1 ms HIGH, emitted only from ticks; ENABLE LOW for at least 150 ms and
 * until READY, then HIGH, LOW and HIGH for 150 ms each, every level timed
 * from the completion of its write, with the motors coasting throughout.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "core/internal.h"

/* The host keeps heartbeat falls within the 70 ms tick window (MCU: 80 ms). */
#define HEARTBEAT_MAX_US KEEL__MS(KEEL_TICK_WINDOW_MS)

static int strict_state(uint32_t state)
{
    return state == KEEL_STATE_ARMING || state == KEEL_STATE_ARMED || state == KEEL_STATE_SERVO;
}

/* A stop step's status as reported to the caller and in cleanup_status:
 * OK, or FAULT-class (a contract-breaking value reads as KEEL_E_INTERNAL). */
static int32_t stop_status(int32_t rc)
{
    return rc == KEEL_OK || keel__is_fault_status(rc) ? rc : KEEL_E_INTERNAL;
}

/* Copies the backend's latched record; non-zero only for a usable record. */
static int backend_fault(const keel__device *dev, keel_port_fault *fault)
{
    memset(fault, 0, sizeof *fault);
    fault->struct_size = sizeof *fault;
    return dev->be.get_fault(dev->be.user, fault) == KEEL_OK && keel__is_fault_status(fault->status) &&
           fault->cause != KEEL_CAUSE_NONE && fault->cause <= KEEL_CAUSE_USER_STOP;
}

/* Updates the ENABLE shadow after a LOW write; the ARMED_UNEXPECTED grace
 * runs from the moment ENABLE really fell, not from later re-assertions. */
static void enable_lowered(keel__device *dev)
{
    if (dev->outputs & KEEL_OUT_ENABLE) {
        dev->outputs &= ~(uint32_t)KEEL_OUT_ENABLE;
        dev->enable_low_since_us = keel__now(dev);
    }
}

void keel__enter_disarmed(keel__device *dev)
{
    dev->state = KEEL_STATE_DISARMED;
    dev->ack_phase = KEEL_ACK_NONE;
    dev->hb_strict = 0;
    keel__motion_drop(dev);
}

/* A stop call bumps the epoch and lowers ENABLE (and coasts) in the backend
 * before it waits for CONTROL. Whichever CONTROL section runs first applies
 * the state effect, so no section acts on a state the stop has ended. */
static void apply_pending_stop(keel__device *dev)
{
    uint32_t epoch = atomic_load(&dev->stop_epoch);
    if (epoch == dev->handled_epoch) {
        return;
    }
    dev->handled_epoch = epoch;
    dev->stopped_state = dev->state;
    dev->stopped_ack = dev->ack_phase;
    enable_lowered(dev);
    if (strict_state(dev->state)) {
        keel__enter_disarmed(dev);
    }
}

void keel__lock(keel__device *dev, uint32_t domain)
{
    if (dev->be.lock) {
        dev->be.lock(dev->be.user, domain);
    }
}

void keel__unlock(keel__device *dev, uint32_t domain)
{
    if (dev->be.unlock) {
        dev->be.unlock(dev->be.user, domain);
    }
}

int32_t keel__control_enter(keel__device *dev)
{
    keel__lock(dev, KEEL_LOCK_CONTROL);
    if (atomic_load(&dev->closing)) {
        keel__unlock(dev, KEEL_LOCK_CONTROL);
        return KEEL_E_INVALID_HANDLE;
    }
    apply_pending_stop(dev);
    return KEEL_OK;
}

void keel__control_leave(keel__device *dev)
{
    keel__unlock(dev, KEEL_LOCK_CONTROL);
}

int32_t keel__reject(keel__device *dev, int32_t status)
{
    atomic_store(&dev->last_rejection, status);
    return status;
}

int keel__is_owner(const keel__device *dev)
{
    return !dev->be.thread_id || !dev->have_owner ||
           dev->be.thread_id(dev->be.user) == dev->owner_thread;
}

int keel__thread_allowed(const keel__device *dev)
{
    return !dev->single_threaded || dev->be.thread_id(dev->be.user) == dev->attach_thread;
}

uint64_t keel__now(const keel__device *dev)
{
    uint64_t now = dev->be.now_us(dev->be.user);
    return now < dev->observed_us ? dev->observed_us : now;
}

int32_t keel__clock(keel__device *dev, uint64_t *now)
{
    uint64_t t = dev->be.now_us(dev->be.user);
    if (t < dev->observed_us) {
        uint64_t step_back = dev->observed_us - t;
        *now = dev->observed_us;
        return keel__latch(dev, KEEL_E_INTERNAL, KEEL_CAUSE_CLOCK, 0, step_back);
    }
    dev->observed_us = t;
    *now = t;
    return KEEL_OK;
}

int32_t keel__sample(keel__device *dev)
{
    uint32_t raw = 0;
    int32_t rc = dev->be.read_inputs(dev->be.user, &raw);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_READ, 0);
    }
    dev->inputs = raw & (KEEL_IN_READY | KEEL_IN_ARMED | KEEL_IN_BUTTONS);
    /* The backend latch has priority over everything this call would
     * evaluate: its cause (for example a PWM worker overrun) is the truth. */
    if ((raw & KEEL_IN_BACKEND_FAULT) && dev->state != KEEL_STATE_FAULTED) {
        return keel__latch_backend(dev);
    }
    return KEEL_OK;
}

void keel__note_inputs(keel__device *dev, uint64_t now)
{
    dev->inputs_sampled_us = now;
    if (dev->inputs & KEEL_IN_READY) {
        dev->ready_low = 0;
        return;
    }
    if (!dev->ready_low) {
        dev->ready_low = 1;
        dev->ready_low_since_us = now;
        /* A measurement in flight while READY fell is not trustworthy. */
        atomic_fetch_add(&dev->ready_drops, 1u);
    }
    keel__servo_ready_low(dev, now);
}

int32_t keel__fresh_inputs(keel__device *dev, uint64_t *now)
{
    int32_t rc = keel__sample(dev);
    if (rc == KEEL_OK) {
        rc = keel__clock(dev, now);
    }
    if (rc == KEEL_OK) {
        keel__note_inputs(dev, *now);
    }
    return rc;
}

int32_t keel__latch(keel__device *dev, int32_t status, uint32_t cause, int32_t os_error,
                    uint64_t detail)
{
    keel_fault *record = &dev->record;
    int32_t cleanup_os_error = 0;
    uint64_t now;
    /* A stop call's own section finds the device already DISARMED by its
     * stop epoch; the record names the state that stop ended (SPEC 12.1). */
    int from_stop = dev->in_stop_call && dev->state == KEEL_STATE_DISARMED;
    if (dev->state == KEEL_STATE_FAULTED) {
        /* The first fault since the last clear keeps the record, but a defect
         * found while latched must still outlive keel_clear_fault(). */
        if (keel__cause_sticky(cause)) {
            record->flags |= KEEL_FAULT_STICKY;
        }
        return status;
    }
    now = keel__now(dev);
    /* The record is captured before any cleanup can change the state. */
    memset(record, 0, sizeof *record);
    record->struct_size = sizeof *record;
    record->status = status;
    record->cause = cause;
    record->state = from_stop ? dev->stopped_state : dev->state;
    record->ack_phase = from_stop ? dev->stopped_ack : dev->ack_phase;
    record->inputs = dev->inputs & (KEEL_IN_READY | KEEL_IN_ARMED);
    record->os_error = os_error;
    record->detail = detail;
    record->time_us = now;
    record->fault_count = ++dev->fault_count;
    record->flags = keel__cause_sticky(cause) ? (uint32_t)KEEL_FAULT_STICKY : 0u;
    record->cleanup_status = stop_status(keel__safe_stop(dev, 1, &cleanup_os_error));
    record->cleanup_os_error = cleanup_os_error;
    dev->state = KEEL_STATE_FAULTED;
    dev->ack_phase = KEEL_ACK_NONE;
    dev->hb_strict = 0;
    dev->hb_restarted = 0;
    dev->have_latch = 1;
    dev->latched_at_us = now;
    return status;
}

int32_t keel__hook_failed(keel__device *dev, int32_t rc, uint32_t cause, uint64_t detail)
{
    keel_port_fault fault;
    /* Only FAULT-class statuses pass through a safety path; anything else
     * from a backend is a contract violation, kept raw in the detail. */
    if (!keel__is_fault_status(rc)) {
        return keel__latch(dev, KEEL_E_INTERNAL, KEEL_CAUSE_HAL_CONTRACT, 0, (uint64_t)(int64_t)rc);
    }
    if (backend_fault(dev, &fault)) {
        return keel__latch(dev, rc, fault.cause, fault.os_error, fault.detail);
    }
    return keel__latch(dev, rc, cause, 0, detail);
}

int32_t keel__latch_backend(keel__device *dev)
{
    keel_port_fault fault;
    if (!backend_fault(dev, &fault)) {
        /* A latch bit without a usable record breaks the port contract. */
        return keel__latch(dev, KEEL_E_INTERNAL, KEEL_CAUSE_HAL_CONTRACT, 0,
                           (uint64_t)(int64_t)fault.status);
    }
    return keel__latch(dev, fault.status, fault.cause, fault.os_error, fault.detail);
}

int32_t keel__window(keel__device *dev, uint64_t now)
{
    if (!dev->have_tick || now - dev->last_tick_us > KEEL__MS(KEEL_TICK_WINDOW_MS)) {
        return keel__latch(dev, KEEL_E_DEADLINE, KEEL_CAUSE_TICK_LATE, 0,
                           dev->have_tick ? now - dev->last_tick_us : 0u);
    }
    return KEEL_OK;
}

int32_t keel__enable_low(keel__device *dev)
{
    int32_t rc = dev->be.write_outputs(dev->be.user, KEEL_OUT_ENABLE, 0, dev->handled_epoch);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_ENABLE);
    }
    enable_lowered(dev);
    return KEEL_OK;
}

int32_t keel__emergency_disable(keel__device *dev)
{
    uint32_t epoch = atomic_fetch_add(&dev->stop_epoch, 1u) + 1u;
    return dev->be.emergency_disable(dev->be.user, epoch);
}

int32_t keel__safe_stop(keel__device *dev, int stop_heartbeat, int32_t *os_error)
{
    keel_port_fault fault;
    int32_t first, rc;

    /* 1. ENABLE LOW and coast under the backend lock only. */
    first = dev->be.emergency_disable(dev->be.user, dev->handled_epoch);
    enable_lowered(dev);
    /* 2. COAST through the motor path as well. */
    rc = keel__coast_write(dev);
    first = first != KEEL_OK ? first : rc;
    keel__motion_drop(dev);
    /* 3. Servo pulse off, then request LOW. */
    rc = keel__servo_off(dev);
    first = first != KEEL_OK ? first : rc;
    /* 4. Heartbeat LOW for latching stops; a pulse in progress ends here. */
    if (stop_heartbeat) {
        rc = dev->be.write_outputs(dev->be.user, KEEL_OUT_HEARTBEAT, 0, dev->handled_epoch);
        first = first != KEEL_OK ? first : rc;
        dev->outputs &= ~(uint32_t)KEEL_OUT_HEARTBEAT;
        if (dev->hb_high) {
            dev->hb_high = 0;
            dev->last_fall_us = keel__now(dev);
            dev->have_fall = 1;
        }
    }
    *os_error = (first != KEEL_OK && backend_fault(dev, &fault)) ? fault.os_error : 0;
    return first;
}

/* ---- heartbeat --------------------------------------------------------- */

/* Decides whether this tick pulses (SPEC 9.2). In ARMING, ARMED and SERVO a
 * late tick or a fall predicted beyond 70 ms fails before pulsing; after a
 * tick that ran DISARMED the same gap restarts the heartbeat instead. */
static int32_t schedule_heartbeat(keel__device *dev, uint64_t now, int strict, int *pulse,
                                  int *restart)
{
    if (dev->have_tick && now - dev->last_tick_us > KEEL__MS(KEEL_TICK_WINDOW_MS)) {
        if (strict) {
            return keel__latch(dev, KEEL_E_DEADLINE, KEEL_CAUSE_TICK_LATE, 0,
                               now - dev->last_tick_us);
        }
        *restart = 1;
    }
    if (dev->have_fall) {
        uint64_t interval = now + KEEL_HEARTBEAT_PULSE_US - dev->last_fall_us;
        if (interval > HEARTBEAT_MAX_US) {
            if (strict) {
                return keel__latch(dev, KEEL_E_DEADLINE, KEEL_CAUSE_HEARTBEAT_LATE, 0, interval);
            }
            *restart = 1;
            *pulse = 1;
        } else if (interval >= KEEL__MS(KEEL_HEARTBEAT_MIN_MS)) {
            *pulse = 1;
        }
    } else if (now >= dev->hb_not_before_us) {
        *pulse = 1;
    }
    dev->hb_restarted = *restart;
    if (*restart) {
        /* The MCU latched F_HEARTBEAT and re-qualifies after two valid falls. */
        dev->heartbeat_restarts++;
        dev->hb_falls = 0;
    }
    return KEEL_OK;
}

/* One 1 ms pulse with CONTROL released during the HIGH time, so a stop from
 * another thread never waits for it. Returns with CONTROL held. */
static int32_t pulse_heartbeat(keel__device *dev, int check_late)
{
    uint64_t previous = dev->last_fall_us, deadline, fall_at;
    int had_fall = dev->have_fall;
    int32_t rc, sleep_rc;

    rc = dev->be.write_outputs(dev->be.user, KEEL_OUT_HEARTBEAT, KEEL_OUT_HEARTBEAT,
                               dev->handled_epoch);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_HEARTBEAT);
    }
    dev->outputs |= KEEL_OUT_HEARTBEAT;
    dev->hb_high = 1;
    deadline = keel__now(dev) + KEEL_HEARTBEAT_PULSE_US;
    keel__control_leave(dev);
    sleep_rc = dev->be.sleep_until_us(dev->be.user, deadline);
    /* Re-lock even if close has started: until close's own stop runs, this
     * tick still owns the HIGH level and must lower it. */
    keel__lock(dev, KEEL_LOCK_CONTROL);
    apply_pending_stop(dev);
    if (dev->hb_high) {
        rc = dev->be.write_outputs(dev->be.user, KEEL_OUT_HEARTBEAT, 0, dev->handled_epoch);
        if (rc != KEEL_OK) {
            return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_HEARTBEAT);
        }
        dev->hb_high = 0;
        dev->outputs &= ~(uint32_t)KEEL_OUT_HEARTBEAT;
        rc = keel__clock(dev, &fall_at);
        if (rc != KEEL_OK) {
            return rc;
        }
        dev->last_fall_us = fall_at;
        dev->have_fall = 1;
        dev->hb_falls++;
        if (sleep_rc == KEEL_OK && check_late && strict_state(dev->state) && had_fall &&
            fall_at - previous > KEEL__MS(KEEL_HEARTBEAT_LATE_MS)) {
            return keel__latch(dev, KEEL_E_DEADLINE, KEEL_CAUSE_HEARTBEAT_LATE, 0,
                               fall_at - previous);
        }
    }
    /* Otherwise a stop lowered the line during the sleep and kept its time. */
    if (sleep_rc != KEEL_OK) {
        return keel__hook_failed(dev, sleep_rc, KEEL_CAUSE_CLOCK, 0);
    }
    if (atomic_load(&dev->closing)) {
        return KEEL_E_INVALID_HANDLE;
    }
    return dev->state == KEEL_STATE_FAULTED ? KEEL_E_FAULTED : KEEL_OK;
}

/* ---- arming acknowledgement -------------------------------------------- */

/* Writes one acknowledgement level. Its hold time starts when the write has
 * completed, so a slow backend can only lengthen a level, never shorten it;
 * a write that blocked past the tick window or the heartbeat bound fails. */
static int32_t ack_level(keel__device *dev, int high, uint32_t next_phase)
{
    uint64_t now;
    int32_t rc = dev->be.write_outputs(dev->be.user, KEEL_OUT_ENABLE, high ? KEEL_OUT_ENABLE : 0u,
                                       dev->handled_epoch);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_ENABLE);
    }
    if (high) {
        dev->outputs |= KEEL_OUT_ENABLE;
    } else {
        enable_lowered(dev);
    }
    rc = keel__clock(dev, &now);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = keel__window(dev, now);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (dev->have_fall && now - dev->last_fall_us > HEARTBEAT_MAX_US) {
        return keel__latch(dev, KEEL_E_DEADLINE, KEEL_CAUSE_HEARTBEAT_LATE, 0,
                           now - dev->last_fall_us);
    }
    dev->ack_phase = next_phase;
    dev->phase_at_us = now;
    return KEEL_OK;
}

/* SPEC 9.4: LOW1 until READY and 150 ms, then HIGH1, LOW2 and HIGH2 for
 * 150 ms each; ARMED must stay LOW until HIGH2. */
static int32_t acknowledge(keel__device *dev, uint64_t now)
{
    uint64_t in_phase = now - dev->phase_at_us;
    uint32_t inputs = dev->inputs;
    int level_done = in_phase >= KEEL__MS(KEEL_ACK_LEVEL_MS);

    if (now - dev->ack_started_us > KEEL__MS(KEEL_ARM_TIMEOUT_MS)) {
        return keel__latch(dev, KEEL_E_ACK_TIMEOUT,
                           dev->ack_phase == KEEL_ACK_LOW1 ? KEEL_CAUSE_ACK_NO_READY
                                                           : KEEL_CAUSE_ACK_NO_ARMED,
                           0, 0);
    }
    if (dev->ack_phase == KEEL_ACK_LOW1) {
        if (inputs & KEEL_IN_ARMED) {
            return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_ACK_FEEDBACK, 0, KEEL_ACK_LOW1);
        }
        return (inputs & KEEL_IN_READY) && level_done ? ack_level(dev, 1, KEEL_ACK_HIGH1)
                                                      : KEEL_OK;
    }
    if (!(inputs & KEEL_IN_READY)) {
        return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_READY_LOST, 0, 0);
    }
    switch (dev->ack_phase) {
    case KEEL_ACK_HIGH1:
        if (inputs & KEEL_IN_ARMED) {
            /* An acknowledgement interrupted after its second LOW leaves the
             * MCU waiting for the final HIGH; this HIGH1 then completes it
             * after the MCU's 100 ms. Go back to LOW1 once per arm. */
            if (in_phase >= KEEL__MS(KEEL__STALE_ACK_MS) && !dev->stale_used) {
                dev->stale_used = 1;
                dev->stale_ack_restarts++;
                return ack_level(dev, 0, KEEL_ACK_LOW1);
            }
            return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_ACK_FEEDBACK, 0, KEEL_ACK_HIGH1);
        }
        return level_done ? ack_level(dev, 0, KEEL_ACK_LOW2) : KEEL_OK;
    case KEEL_ACK_LOW2:
        if (inputs & KEEL_IN_ARMED) {
            return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_ACK_FEEDBACK, 0, KEEL_ACK_LOW2);
        }
        return level_done ? ack_level(dev, 1, KEEL_ACK_HIGH2) : KEEL_OK;
    default: /* HIGH2 */
        if (level_done && (inputs & KEEL_IN_ARMED)) {
            dev->state = KEEL_STATE_ARMED;
            dev->ack_phase = KEEL_ACK_NONE;
        }
        return KEEL_OK;
    }
}

/* Feedback rules and state progress of one tick (SPEC 9.5). */
static int32_t evaluate(keel__device *dev, uint64_t now)
{
    uint32_t inputs = dev->inputs;
    switch (dev->state) {
    case KEEL_STATE_DISARMED:
        if ((inputs & KEEL_IN_ARMED) &&
            now - dev->enable_low_since_us > KEEL__MS(KEEL_ARMED_GRACE_MS)) {
            return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_ARMED_UNEXPECTED, 0, 0);
        }
        return KEEL_OK;
    case KEEL_STATE_ARMING:
        return acknowledge(dev, now);
    case KEEL_STATE_ARMED:
        if (!(inputs & KEEL_IN_READY)) {
            return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_READY_LOST, 0, 0);
        }
        if (!(inputs & KEEL_IN_ARMED)) {
            return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_ARMED_LOST, 0, 0);
        }
        return keel__motion_tick(dev, now);
    case KEEL_STATE_SERVO:
        if (!(inputs & KEEL_IN_READY)) {
            return keel__latch(dev, KEEL_E_FEEDBACK, KEEL_CAUSE_READY_LOST, 0, 0);
        }
        return keel__servo_tick(dev, now);
    default:
        return KEEL_OK;
    }
}

/* The motor command of keel_cycle(), checked after the tick (SPEC 13.1). */
static int32_t apply_command(keel__device *dev, const keel_cycle_input *in,
                             const keel_snapshot *out, uint64_t now)
{
    if (in->command > KEEL_CYCLE_BRAKE || (in->flags & ~(uint32_t)KEEL_CYCLE_TAKE_BUTTONS) ||
        (out && !keel__out_ok(out, sizeof *out)) ||
        (in->command == KEEL_CYCLE_DRIVE &&
         (!keel__duty_valid(in->left) || !keel__duty_valid(in->right)))) {
        return keel__motion_refuse(dev, KEEL_E_INVALID_ARGUMENT);
    }
    switch (in->command) {
    case KEEL_CYCLE_DRIVE:
        if (in->left == 0 && in->right == 0) {
            return keel__motion_zero(dev);
        }
        return keel__motion_set(dev, in->left, in->right, &now);
    case KEEL_CYCLE_BRAKE:
        return keel__motion_brake(dev, &now);
    default:
        return KEEL_OK;
    }
}

/* SPEC 9.3, with CONTROL held on entry and on return. */
static int32_t tick_locked(keel__device *dev, uint64_t cycle_id, uint32_t health,
                           const keel_cycle_input *in, const keel_snapshot *out,
                           int32_t *command_rc)
{
    uint64_t now;
    int strict, pulse = 0, restart = 0;
    int32_t rc = keel__sample(dev);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (dev->state == KEEL_STATE_FAULTED) {
        /* Buttons keep working while FAULTED; the heartbeat stays stopped. */
        rc = keel__clock(dev, &now);
        if (rc == KEEL_OK) {
            keel__note_inputs(dev, now);
            keel__buttons_debounce(dev, now);
        }
        return rc != KEEL_OK ? rc : KEEL_E_FAULTED;
    }
    if (!keel__is_owner(dev)) {
        return keel__latch(dev, KEEL_E_HEALTH, KEEL_CAUSE_TICK_THREAD_CHANGED, 0,
                           dev->be.thread_id(dev->be.user));
    }
    if (health != KEEL_HEALTH_ALL) {
        return keel__latch(dev, KEEL_E_HEALTH, KEEL_CAUSE_HEALTH_FLAGS, 0,
                           (uint64_t)(KEEL_HEALTH_ALL & ~health));
    }
    if (dev->have_cycle && cycle_id <= dev->last_cycle_id) {
        return keel__latch(dev, KEEL_E_HEALTH, KEEL_CAUSE_CYCLE_NOT_INCREASING, 0, cycle_id);
    }
    rc = keel__clock(dev, &now);
    if (rc != KEEL_OK) {
        return rc;
    }
    keel__note_inputs(dev, now);
    strict = dev->hb_strict && strict_state(dev->state);
    rc = schedule_heartbeat(dev, now, strict, &pulse, &restart);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev->last_cycle_id = cycle_id;
    dev->have_cycle = 1;
    dev->last_tick_us = now;
    dev->have_tick = 1;
    keel__buttons_debounce(dev, now);
    rc = evaluate(dev, now);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (in) {
        *command_rc = apply_command(dev, in, out, now);
    }
    if (pulse && dev->state != KEEL_STATE_FAULTED) {
        rc = pulse_heartbeat(dev, strict && !restart);
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    if (dev->state != KEEL_STATE_FAULTED) {
        if (dev->be.thread_id && !dev->have_owner) {
            dev->have_owner = 1;
            dev->owner_thread = dev->be.thread_id(dev->be.user);
        }
        dev->hb_strict = strict_state(dev->state);
    }
    return KEEL_OK;
}

int32_t keel__tick(keel__device *dev, uint64_t cycle_id, uint32_t health,
                   const keel_cycle_input *in, keel_snapshot *out)
{
    int32_t command_rc = KEEL_OK, rc, result;
    rc = keel__control_enter(dev);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = tick_locked(dev, cycle_id, health, in, out, &command_rc);
    result = rc != KEEL_OK ? rc : command_rc;
    if (out && result != KEEL_E_INVALID_ARGUMENT && result != KEEL_E_INVALID_HANDLE &&
        keel__out_ok(out, sizeof *out)) {
        keel_snapshot snapshot;
        keel__snapshot(dev, &snapshot);
        keel__out_write(out, &snapshot, sizeof snapshot);
        if (in && (in->flags & KEEL_CYCLE_TAKE_BUTTONS)) {
            dev->buttons.pressed = 0;
            dev->buttons.released = 0;
        }
    }
    keel__control_leave(dev);
    return result;
}

/* ---- arm, stop, clear ---------------------------------------------------- */

int32_t keel__request_arm(keel__device *dev)
{
    uint64_t now;
    int32_t rc;
    if (!keel__is_owner(dev)) {
        return keel__reject(dev, KEEL_E_CONFLICT);
    }
    if (dev->state == KEEL_STATE_FAULTED) {
        return KEEL_E_FAULTED;
    }
    if (dev->have_latch &&
        keel__now(dev) - dev->latched_at_us < KEEL__MS(KEEL_REARM_HOLDOFF_MS)) {
        return keel__reject(dev, KEEL_E_STATE);
    }
    if (dev->state == KEEL_STATE_SERVO) {
        return keel__reject(dev, KEEL_E_CONFLICT);
    }
    if (dev->state != KEEL_STATE_DISARMED) {
        return KEEL_OK; /* already arming or armed */
    }
    rc = keel__fresh_inputs(dev, &now);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = keel__enable_low(dev);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = keel__coast_write(dev);
    keel__motion_drop(dev);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_MOTOR_WRITE, 0);
    }
    /* LOW1 is timed from the completion of these writes. */
    rc = keel__clock(dev, &now);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev->state = KEEL_STATE_ARMING;
    dev->ack_phase = KEEL_ACK_LOW1;
    dev->ack_started_us = now;
    dev->phase_at_us = now;
    dev->stale_used = 0;
    /* The first tick after arming tolerates a gap. */
    dev->hb_strict = 0;
    return KEEL_OK;
}

static void note_cleanup(keel__device *dev, int32_t status, int32_t os_error)
{
    if (dev->record.status != KEEL_OK) {
        dev->record.cleanup_status = stop_status(status);
        dev->record.cleanup_os_error = os_error;
    }
}

int32_t keel__disarm(keel__device *dev, int32_t disable_rc)
{
    uint32_t raw = 0;
    int32_t read_rc, stop_rc, first, os_error = 0;
    int faulted = dev->state == KEEL_STATE_FAULTED;

    read_rc = dev->be.read_inputs(dev->be.user, &raw);
    /* The whole sequence is re-run in every state; in FAULTED the heartbeat
     * is re-asserted LOW as well. */
    stop_rc = keel__safe_stop(dev, faulted, &os_error);
    first = disable_rc != KEEL_OK ? disable_rc : stop_rc;
    note_cleanup(dev, first, os_error);
    if (faulted) {
        if (first != KEEL_OK) {
            (void)keel__hook_failed(dev, first, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_ENABLE);
        }
        return stop_status(first);
    }
    keel__enter_disarmed(dev);
    if (read_rc != KEEL_OK) {
        return keel__hook_failed(dev, read_rc, KEEL_CAUSE_GPIO_READ, 0);
    }
    dev->inputs = raw & (KEEL_IN_READY | KEEL_IN_ARMED | KEEL_IN_BUTTONS);
    if (raw & KEEL_IN_BACKEND_FAULT) {
        return keel__latch_backend(dev); /* the earlier failure wins */
    }
    if (first != KEEL_OK) {
        return keel__hook_failed(dev, first, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_ENABLE);
    }
    return KEEL_OK;
}

int32_t keel__emergency_stop(keel__device *dev, int32_t disable_rc)
{
    uint32_t raw = 0;
    int32_t read_rc, first, os_error = 0;

    read_rc = dev->be.read_inputs(dev->be.user, &raw);
    if (dev->state == KEEL_STATE_FAULTED) {
        /* Re-run the stop; the existing record is kept. */
        int32_t stop_rc = keel__safe_stop(dev, 1, &os_error);
        first = disable_rc != KEEL_OK ? disable_rc : stop_rc;
        note_cleanup(dev, first, os_error);
        if (first != KEEL_OK) {
            (void)keel__hook_failed(dev, first, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_ENABLE);
        }
        return stop_status(first);
    }
    if (read_rc != KEEL_OK) {
        return keel__hook_failed(dev, read_rc, KEEL_CAUSE_GPIO_READ, 0);
    }
    dev->inputs = raw & (KEEL_IN_READY | KEEL_IN_ARMED | KEEL_IN_BUTTONS);
    if (raw & KEEL_IN_BACKEND_FAULT) {
        return keel__latch_backend(dev);
    }
    if (disable_rc != KEEL_OK) {
        /* Lowering ENABLE failed before the stop latched: that failure is
         * the first fault, not the user's stop. */
        return keel__hook_failed(dev, disable_rc, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_ENABLE);
    }
    (void)keel__latch(dev, KEEL_E_FAULTED, KEEL_CAUSE_USER_STOP, 0, 0);
    return dev->record.cleanup_status;
}

int32_t keel__clear_fault(keel__device *dev)
{
    keel_port_fault fault;
    uint32_t raw = 0;
    uint64_t now;
    int32_t rc, os_error = 0;

    if (!keel__is_owner(dev)) {
        return keel__reject(dev, KEEL_E_CONFLICT);
    }
    if (dev->state != KEEL_STATE_FAULTED) {
        return KEEL_OK;
    }
    if (dev->record.flags & KEEL_FAULT_STICKY) {
        return KEEL_E_FAULTED;
    }
    rc = keel__safe_stop(dev, 1, &os_error);
    note_cleanup(dev, rc, os_error);
    if (rc == KEEL_OK) {
        rc = dev->be.reset_fault(dev->be.user);
    }
    if (rc == KEEL_OK) {
        rc = dev->be.read_inputs(dev->be.user, &raw);
    }
    if (rc != KEEL_OK) {
        (void)keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE, 0); /* FAULTED: sticky check only */
        return stop_status(rc);
    }
    if (raw & KEEL_IN_BACKEND_FAULT) {
        /* The condition persists in the backend: stay FAULTED. */
        return backend_fault(dev, &fault) ? fault.status : KEEL_E_INTERNAL;
    }
    rc = keel__clock(dev, &now);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev->inputs = raw & (KEEL_IN_READY | KEEL_IN_ARMED | KEEL_IN_BUTTONS);
    keel__note_inputs(dev, now);
    dev->record.flags |= KEEL_FAULT_CLEARED;
    keel__enter_disarmed(dev);
    dev->have_owner = 0;
    dev->have_tick = 0;
    dev->hb_restarted = 0;
    /* The next tick restarts the heartbeat, keeping the MCU's 20 ms minimum
     * spacing from the last fall before the latch. */
    dev->hb_not_before_us =
        dev->have_fall && dev->last_fall_us + KEEL__MS(KEEL_HEARTBEAT_MIN_MS) > now
            ? dev->last_fall_us + KEEL__MS(KEEL_HEARTBEAT_MIN_MS)
            : now;
    dev->have_fall = 0;
    dev->hb_falls = 0;
    return KEEL_OK;
}

int32_t keel__close_stop(keel__device *dev, int32_t disable_rc)
{
    uint32_t raw = 0;
    int32_t read_rc, stop_rc, first, os_error = 0;
    apply_pending_stop(dev); /* close's own epoch, bumped before it locked */
    /* The final record names a failure found now; close's own status only
     * says whether the stop itself was confirmed. */
    read_rc = dev->be.read_inputs(dev->be.user, &raw);
    if (read_rc != KEEL_OK) {
        (void)keel__hook_failed(dev, read_rc, KEEL_CAUSE_GPIO_READ, 0);
    } else if (raw & KEEL_IN_BACKEND_FAULT) {
        (void)keel__latch_backend(dev);
    }
    stop_rc = keel__safe_stop(dev, 1, &os_error);
    first = disable_rc != KEEL_OK ? disable_rc : stop_rc;
    if (first != KEEL_OK) {
        (void)keel__hook_failed(dev, first, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_ENABLE);
    }
    /* Button LEDs are not a stop action; their failure is not reported. */
    (void)dev->be.write_outputs(dev->be.user, KEEL_OUT_BUTTON_LEDS, 0, dev->handled_epoch);
    dev->button_leds = 0;
    dev->record.cleanup_status = stop_status(first);
    dev->record.cleanup_os_error = os_error;
    dev->state = KEEL_STATE_FAULTED;
    dev->ack_phase = KEEL_ACK_NONE;
    return first == KEEL_OK ? KEEL_OK : KEEL_E_IO;
}
