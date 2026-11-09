/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/* Reference-model fixture (see fixture.h). */
/* SPEC refers to the libkeel specification, software/libkeel/docs/spec.md. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fixture.h"

/* MCU faults that no host behaviour may ever cause. F_ACK and F_HEARTBEAT
 * follow legitimately from a stop during arming or a stopped heartbeat, so
 * the random walk judges those itself (fx_take_trips()). */
#define FX_FORBIDDEN_TRIPS \
    (F_PWM | F_COOLDOWN | F_SERVO_TIMEOUT | F_FEEDBACK | F_INTERNAL | F_LOOP | F_RAIL)

/* Long FX_INVARIANTS runs keep counting after a log fills up; every other
 * user expects the whole history. */
static int log_slot(const fixture *f, size_t used, size_t capacity)
{
    if (used < capacity) {
        return 1;
    }
    CHECK(f->options & FX_INVARIANTS);
    return 0;
}

/* Records the ENABLE edges and heartbeat falls a board operation caused. */
static void observe(fixture *f, uint32_t before)
{
    uint32_t after = f->board.outputs;
    if ((before ^ after) & KEEL_OUT_ENABLE) {
        if (log_slot(f, f->nenable_changes, sizeof f->enable_changes / sizeof f->enable_changes[0])) {
            f->enable_changes[f->nenable_changes++] = f->board.now_us;
        }
        if (after & KEEL_OUT_ENABLE) {
            f->enable_rises++;
        }
    }
    if ((before & KEEL_OUT_HEARTBEAT) && !(after & KEEL_OUT_HEARTBEAT)) {
        if (log_slot(f, f->nfalls, sizeof f->falls / sizeof f->falls[0])) {
            f->falls[f->nfalls++] = f->board.now_us;
        }
        f->fall_count++;
        f->last_fall_us = f->board.now_us;
    }
}

/* An injected failure. A FAULT-class status latches the backend record
 * first, as port.h requires; any other status models a backend that breaks
 * the contract, so it comes back bare. */
static int32_t injected(fixture *f, uint32_t cause, uint64_t detail)
{
    int32_t status = f->fail_status ? f->fail_status : KEEL_E_IO;
    uint32_t before = f->board.outputs;
    if (keel_status_class(status) != KEEL_CLASS_FAULT) {
        return status;
    }
    (void)keel__board_latch(&f->board, status, cause, EIO, detail);
    observe(f, before);
    return status;
}

static void delay(fixture *f, uint32_t *pending)
{
    if (*pending) {
        uint32_t amount = *pending;
        *pending = 0;
        fx_advance(f, f->board.now_us + amount);
    }
}

static uint64_t hook_now(void *user)
{
    return ((fixture *)user)->board.now_us;
}

static int32_t hook_sleep(void *user, uint64_t deadline_us)
{
    fixture *f = user;
    uint64_t now = f->board.now_us;
    if (++f->sleep_calls == f->fail_sleep) {
        /* A sleep has no backend record; the core reports cause CLOCK. */
        return f->fail_status ? f->fail_status : KEEL_E_IO;
    }
    fx_advance(f, (deadline_us > now ? deadline_us : now) + f->extra_sleep);
    return KEEL_OK;
}

static int32_t hook_read(void *user, uint32_t *inputs)
{
    fixture *f = user;
    int32_t rc;
    if (++f->read_calls == f->fail_read) {
        return injected(f, KEEL_CAUSE_GPIO_READ, 0);
    }
    delay(f, &f->delay_read);
    rc = keel__board_read(&f->board, inputs);
    if (rc == KEEL_OK) {
        *inputs = (*inputs | f->force_high) & ~f->force_low;
    }
    if (f->read_calls == f->latch_after_read) {
        /* A PWM worker failure racing a read that already succeeded. */
        uint32_t before = f->board.outputs;
        (void)keel__board_latch(&f->board, KEEL_E_PWM, KEEL_CAUSE_PWM_OVERRUN, 0, 31000);
        observe(f, before);
    }
    return rc;
}

static int32_t hook_outputs(void *user, uint32_t mask, uint32_t values, uint32_t stop_epoch)
{
    fixture *f = user;
    uint32_t before;
    int32_t rc;
    if (++f->output_calls == f->fail_output) {
        return injected(f, KEEL_CAUSE_GPIO_WRITE, mask);
    }
    CHECK((mask & ~(uint32_t)KEEL_OUT_ALL) == 0);
    if (mask & KEEL_OUT_ENABLE) {
        delay(f, &f->delay_enable);
    }
    before = f->board.outputs;
    rc = keel__board_write_outputs(&f->board, mask, values, stop_epoch);
    observe(f, before);
    return rc;
}

static int32_t hook_motors(void *user, const keel_port_motor_command *command)
{
    fixture *f = user;
    uint32_t before;
    int32_t rc;
    if (++f->motor_calls == f->fail_motor) {
        return injected(f, KEEL_CAUSE_MOTOR_WRITE, 0);
    }
    if (command->mode == KEEL_MOTOR_COAST) {
        delay(f, &f->delay_motor_zero);
    } else if (f->board.fault.status == KEEL_OK) {
        /* The core never asks for motion the MCU has not permitted. */
        CHECK(f->board.permit.motor_permit && f->board.permit.ready && !f->board.fault_low);
        CHECK(f->board.outputs & KEEL_OUT_ENABLE);
        CHECK(command->valid_until_us >= f->board.now_us);
    }
    before = f->board.outputs;
    rc = keel__board_write_motors(&f->board, command);
    observe(f, before);
    delay(f, &f->delay_motor_return);
    return rc;
}

static int32_t hook_disable(void *user, uint32_t stop_epoch)
{
    fixture *f = user;
    uint32_t before = f->board.outputs;
    int32_t rc;
    if (++f->disable_calls == f->fail_disable) {
        return injected(f, KEEL_CAUSE_GPIO_WRITE, KEEL_OUT_ENABLE);
    }
    rc = keel__board_emergency_disable(&f->board, stop_epoch);
    observe(f, before);
    return rc;
}

static int32_t hook_get_fault(void *user, keel_port_fault *out)
{
    fixture *f = user;
    if (++f->get_fault_calls == f->fail_get_fault) {
        return f->fail_status ? f->fail_status : KEEL_E_IO;
    }
    return keel__board_get_fault(&f->board, out);
}

static int32_t hook_reset_fault(void *user)
{
    fixture *f = user;
    if (++f->reset_calls == f->fail_reset) {
        return f->fail_status ? f->fail_status : KEEL_E_IO; /* the latch persists */
    }
    return keel__board_reset_fault(&f->board);
}

static int32_t hook_i2c(void *user, uint8_t address, uint8_t reg, const uint8_t *data,
                        size_t count)
{
    fixture *f = user;
    size_t index = f->nwrites++;
    if (f->fail_i2c == f->nwrites) {
        return f->fail_status ? f->fail_status : KEEL_E_IO;
    }
    CHECK(count <= sizeof f->writes[0].data);
    if (log_slot(f, index, sizeof f->writes / sizeof f->writes[0])) {
        transaction *t = &f->writes[index];
        t->address = address;
        t->reg = reg;
        t->count = count;
        memcpy(t->data, data, count);
    }
    return keel__board_i2c(&f->board, address, reg, data, count);
}

static int32_t hook_servo(void *user, const keel_port_servo_command *command)
{
    fixture *f = user;
    if (++f->servo_calls == f->fail_servo) {
        return injected(f, KEEL_CAUSE_GPIO_WRITE, 0);
    }
    return keel__board_servo(&f->board, command);
}

static int32_t hook_trigger(void *user, uint32_t *out_echo_high)
{
    fixture *f = user;
    if (++f->trigger_calls == f->fail_trigger) {
        return f->fail_status ? f->fail_status : KEEL_E_IO;
    }
    return keel__board_range_trigger(&f->board, out_echo_high);
}

static int32_t hook_edges(void *user, keel_port_edge *edges, uint32_t capacity,
                          uint32_t *out_count)
{
    fixture *f = user;
    if (++f->edges_calls == f->fail_edges) {
        return f->fail_status ? f->fail_status : KEEL_E_IO;
    }
    return keel__board_range_edges(&f->board, edges, capacity, out_count);
}

/* The domain locks of a single real thread: FX_THREADS only plays threads
 * through the thread_id hook, so the locks have nothing to exclude. */
static void hook_lock(void *user, uint32_t domain)
{
    (void)user;
    CHECK(domain <= KEEL_LOCK_RANGE);
}

static uint64_t hook_thread(void *user)
{
    return ((fixture *)user)->thread;
}

void fx_prepare(fixture *f, uint32_t held)
{
    size_t align = keel_port_storage_align();
    size_t size = (keel_port_storage_size() + align - 1) / align * align;
    memset(f, 0, sizeof *f);
    keel__board_init(&f->board, 0);
    f->board.buttons = held;
    f->thread = 1;
    f->storage = aligned_alloc(align, size);
    CHECK(f->storage);
}

int32_t fx_attach(fixture *f, keel_open_report *report)
{
    keel_port_backend be;
    memset(&be, 0, sizeof be);
    be.struct_size = sizeof be;
    be.port_version = KEEL_PORT_VERSION;
    be.capabilities =
        KEEL_CAP_MOTORS | KEEL_CAP_BRAKE | KEEL_CAP_RGB | KEEL_CAP_BUTTONS | KEEL_CAP_BUTTON_LEDS;
    be.user = f;
    be.now_us = hook_now;
    be.sleep_until_us = hook_sleep;
    be.read_inputs = hook_read;
    be.write_outputs = hook_outputs;
    be.write_motors = hook_motors;
    be.emergency_disable = hook_disable;
    be.get_fault = hook_get_fault;
    be.reset_fault = hook_reset_fault;
    be.i2c_write = hook_i2c;
    if (f->options & FX_THREADS) {
        be.lock = hook_lock;
        be.unlock = hook_lock;
        be.thread_id = hook_thread;
    } else {
        be.flags = KEEL_PORT_SINGLE_THREADED;
    }
    if (f->options & FX_SERVO) {
        be.capabilities |= KEEL_CAP_SERVO;
        be.servo = hook_servo;
    }
    if (f->options & FX_RANGE) {
        be.capabilities |= KEEL_CAP_RANGE;
        be.range_trigger = hook_trigger;
        be.range_edges = hook_edges;
    }
    return keel_port_attach(f->storage, keel_port_storage_size(), &be, NULL, &f->device, report);
}

void fx_init_options(fixture *f, unsigned options)
{
    fx_prepare(f, 0);
    f->options = options;
    CHECK_RC(fx_attach(f, NULL), KEEL_OK);
    CHECK(fx_state(f) == KEEL_STATE_DISARMED);
    CHECK(f->board.outputs == 0 && fx_left(f) == 0 && fx_right(f) == 0);
}

void fx_init_held(fixture *f, uint32_t held)
{
    fx_prepare(f, held);
    CHECK_RC(fx_attach(f, NULL), KEEL_OK);
    CHECK(fx_state(f) == KEEL_STATE_DISARMED);
    CHECK(f->board.outputs == 0 && fx_left(f) == 0 && fx_right(f) == 0);
}

void fx_init(fixture *f)
{
    fx_init_held(f, 0);
}

void fx_destroy(fixture *f)
{
    if (f->device) {
        CHECK_RC(keel_close(f->device, NULL), KEEL_OK);
        f->device = NULL;
    }
    free(f->storage);
    f->storage = NULL;
}

/* SPEC 23.1 random-walk invariants, true on every simulated millisecond. */
static void check_invariants(const fixture *f)
{
    const keel__sim_board *b = &f->board;
    uint32_t left_both = KEEL__PWM_LP | KEEL__PWM_LN, right_both = KEEL__PWM_RP | KEEL__PWM_RN;
    /* No bridge input HIGH while the MCU does not permit the motors. */
    if (b->levels) {
        CHECK(b->permit.motor_permit && (b->outputs & KEEL_OUT_ENABLE));
    }
    /* Both inputs of a bridge HIGH only as a brake. */
    if ((b->levels & left_both) == left_both || (b->levels & right_both) == right_both) {
        CHECK(b->motor.mode == KEEL_MOTOR_BRAKE);
    }
    /* The servo request only with the motors idle. */
    if (b->servo_request) {
        CHECK(b->levels == 0 && b->motor.mode == KEEL_MOTOR_COAST);
    }
    CHECK(!(b->mcu.last_faults & FX_FORBIDDEN_TRIPS));
}

void fx_advance(fixture *f, uint64_t target_us)
{
    CHECK(target_us >= f->board.now_us);
    while (f->board.now_us < target_us) {
        uint64_t next = f->board.next_step_us < target_us ? f->board.next_step_us : target_us;
        enum safety_stage stage = f->board.mcu.stage;
        keel__board_advance(&f->board, next);
        CHECK(!(f->board.permit.motor_permit && f->board.permit.servo_permit));
        if (!(f->options & FX_SERVO)) {
            CHECK(!f->board.permit.servo_permit); /* nothing requests the servo */
        }
        if (stage != S_LATCHED && f->board.mcu.stage == S_LATCHED) {
            /* F_BOOT is not a trip reason: it stays set until the MCU first
             * reaches its acknowledgement wait. */
            f->mcu_trips |= f->board.mcu.last_faults & ~(uint32_t)F_BOOT;
            f->mcu_trip_us = f->board.now_us;
        }
        if (f->options & FX_INVARIANTS) {
            check_invariants(f);
        }
    }
}

uint32_t fx_take_trips(fixture *f)
{
    uint32_t trips = f->mcu_trips;
    f->mcu_trips = 0;
    return trips;
}

uint64_t fx_now(const fixture *f)
{
    return f->board.now_us;
}

int32_t fx_tick(fixture *f)
{
    return keel_tick(f->device, ++f->cycle, KEEL_HEALTH_ALL);
}

void fx_tick_after(fixture *f, uint64_t gap_us)
{
    fx_advance(f, f->board.now_us + gap_us);
    CHECK_RC(fx_tick(f), KEEL_OK);
}

uint32_t fx_state(fixture *f)
{
    uint32_t state = UINT32_MAX;
    CHECK_RC(keel_get_state(f->device, &state), KEEL_OK);
    return state;
}

keel_snapshot fx_snapshot(fixture *f)
{
    keel_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.struct_size = sizeof snapshot;
    CHECK_RC(keel_get_snapshot(f->device, &snapshot), KEEL_OK);
    return snapshot;
}

keel_fault fx_fault(fixture *f)
{
    keel_fault fault;
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    CHECK_RC(keel_get_fault(f->device, &fault), KEEL_OK);
    return fault;
}

void fx_arm(fixture *f)
{
    unsigned n;
    CHECK_RC(keel_request_arm(f->device), KEEL_OK);
    for (n = 0; n < 160 && fx_state(f) != KEEL_STATE_ARMED; ++n) {
        fx_tick_after(f, 10000);
    }
    CHECK(fx_state(f) == KEEL_STATE_ARMED);
    CHECK(f->board.permit.motor_permit && f->board.permit.ready);
    CHECK(fx_left(f) == 0 && fx_right(f) == 0);
}

void fx_recover(fixture *f)
{
    unsigned n;
    CHECK_RC(keel_clear_fault(f->device), KEEL_OK);
    CHECK(fx_state(f) == KEEL_STATE_DISARMED);
    /* Ticks keep the heartbeat running through the 1 s re-arm hold-off. */
    for (n = 0; n < 100; ++n) {
        fx_tick_after(f, 10000);
    }
    fx_arm(f);
}

void fx_safe(const fixture *f)
{
    CHECK(!(f->board.outputs & (KEEL_OUT_ENABLE | KEEL_OUT_HEARTBEAT)));
    CHECK(fx_left(f) == 0 && fx_right(f) == 0 && f->board.levels == 0);
    CHECK(!f->board.servo_request && f->board.servo_pulse_us == 0);
}

void fx_no_more_heartbeat(fixture *f)
{
    size_t falls = f->fall_count;
    unsigned i;
    for (i = 0; i < 10; ++i) {
        fx_advance(f, f->board.now_us + 10000);
        CHECK_RC(fx_tick(f), KEEL_E_FAULTED);
    }
    CHECK(f->fall_count == falls && !f->board.permit.motor_permit);
    CHECK_RC(keel_disarm(f->device), KEEL_OK);
    CHECK_RC(fx_tick(f), KEEL_E_FAULTED);
}

int32_t fx_left(const fixture *f)
{
    return f->board.motor.mode == KEEL_MOTOR_DRIVE ? f->board.motor.left : 0;
}

int32_t fx_right(const fixture *f)
{
    return f->board.motor.mode == KEEL_MOTOR_DRIVE ? f->board.motor.right : 0;
}
