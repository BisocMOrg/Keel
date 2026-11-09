/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Arbitration replay (SPEC 13.2 and 23.1): every reachable cell of
 * arbitration_table.h, from both thread columns, against the simulator on
 * its virtual clock, through the public API only.
 *
 * For each cell a fresh device is brought into the cell's state with the
 * row's precondition, the control owner is recorded by the main thread,
 * and the call runs either on the main thread (OWNER) or on a new thread
 * (OTHER). The replay then checks the status, the recorded cause, the state
 * after the call, the observable side effects, that a refusal changed
 * nothing, and heartbeat continuity: eight more on-time ticks from the owner
 * must keep the heartbeat running exactly when the cell says so.
 *
 * One CTest case per row: test_arbitration <row number>.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arbitration_table.h"
#include "check.h"

/* BCM pins in keel_sim_state::pins. */
enum {
    PIN_LP = 1u << 8, PIN_RN = 1u << 9, PIN_RP = 1u << 10, PIN_LN = 1u << 11,
    PIN_HEARTBEAT = 1u << 18, PIN_SERVO_REQUEST = 1u << 19, PIN_LED_B = 1u << 22,
    PIN_ENABLE = 1u << 26,
    PIN_MOTORS = PIN_LP | PIN_RN | PIN_RP | PIN_LN,
    /* Pins a refusal must never change. */
    PIN_GUARDED = PIN_MOTORS | PIN_SERVO_REQUEST | PIN_ENABLE
};

enum { MS = 1000, TICK_US = 10 * MS, OBJECT_MM = 500 };

/* Row preconditions (row_spec::setup). */
enum setup_bit {
    SET_LATE_TICK = 1 << 0,     /* the call comes 75 ms after the previous tick */
    SET_LATE_FALL = 1 << 1,     /* ticks arranged so the next fall is predicted > 70 ms */
    SET_HOLDOFF = 1 << 2,       /* a fault latched just before (cleared again in DISARMED) */
    SET_STICKY = 1 << 3,        /* FAULTED with a sticky cause */
    SET_DRIVING = 1 << 4,       /* ARMED: both motors driven forward */
    SET_COOLED = 1 << 5,        /* ARMED after the 2.2 s servo cooldown */
    SET_NO_SERVO = 1 << 6,      /* opened with KEEL_OPEN_NO_SERVO */
    SET_POWER_UNKNOWN = 1 << 7, /* heartbeat not yet qualified; ARMED/SERVO: loop stalled */
    SET_RANGE_BUSY = 1 << 8,    /* a measurement was just triggered */
    SET_TICK_DUE = 1 << 9       /* 10 ms pass before the call, as in a loop */
};

/* Effects with no visible change to the guarded pins or the motion state. */
#define PASSIVE_EFFECTS                                                                       \
    (KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_READ | KEEL_ARB_FX_RESULT | KEEL_ARB_FX_SIM |           \
     KEEL_ARB_FX_HOLDOFF | KEEL_ARB_FX_TAKE | KEEL_ARB_FX_SLEEP | KEEL_ARB_FX_WAIT |           \
     KEEL_ARB_FX_TRIGGER | KEEL_ARB_FX_UNAVAILABLE | KEEL_ARB_FX_I2C | KEEL_ARB_FX_BUTTON_LEDS)

typedef struct bench {
    keel_device *device;
    uint64_t cycle;
} bench;

typedef int32_t (*action)(bench *b);

typedef struct row_spec {
    const char *call; /* must equal keel_arb_table[i].call */
    unsigned setup;   /* ::setup_bit */
    action act;
} row_spec;

/* ---- device helpers ------------------------------------------------------ */

static uint64_t now_of(bench *b)
{
    uint64_t now = 0;
    CHECK_RC(keel_now_us(b->device, &now), KEEL_OK);
    return now;
}

static void advance(bench *b, uint64_t us)
{
    CHECK_RC(keel_sleep_until(b->device, now_of(b) + us), KEEL_OK);
}

static int32_t tick(bench *b)
{
    return keel_tick(b->device, ++b->cycle, KEEL_HEALTH_ALL);
}

static void ticks(bench *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        advance(b, TICK_US);
        CHECK_RC(tick(b), KEEL_OK);
    }
}

static uint32_t state_of(bench *b)
{
    uint32_t state = UINT32_MAX;
    CHECK_RC(keel_get_state(b->device, &state), KEEL_OK);
    return state;
}

static keel_snapshot snapshot_of(bench *b)
{
    keel_snapshot s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    CHECK_RC(keel_get_snapshot(b->device, &s), KEEL_OK);
    return s;
}

static keel_fault fault_of(bench *b)
{
    keel_fault f;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    CHECK_RC(keel_get_fault(b->device, &f), KEEL_OK);
    return f;
}

static keel_sim_state sim_of(bench *b)
{
    keel_sim_state s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    CHECK_RC(keel_sim_get_state(b->device, &s), KEEL_OK);
    return s;
}

static keel_range poll_of(bench *b)
{
    keel_range r;
    memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    CHECK_RC(keel_range_poll(b->device, &r), KEEL_OK);
    return r;
}

static void open_bench(bench *b, unsigned setup)
{
    keel_open_options options;
    memset(b, 0, sizeof *b);
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    options.flags = KEEL_OPEN_VIRTUAL_CLOCK | ((setup & SET_NO_SERVO) ? KEEL_OPEN_NO_SERVO : 0u);
    CHECK_RC(keel_open(&options, &b->device, NULL), KEEL_OK);
    CHECK_RC(keel_sim_set(b->device, KEEL_SIM_RANGE_MM, OBJECT_MM), KEEL_OK);
}

static void arm(bench *b)
{
    unsigned n;
    CHECK_RC(keel_request_arm(b->device), KEEL_OK);
    for (n = 0; n < 150 && state_of(b) != KEEL_STATE_ARMED; ++n) {
        ticks(b, 1);
    }
    CHECK(state_of(b) == KEEL_STATE_ARMED);
}

/* The main thread latches a non-sticky fault by a latching stop. */
static void user_stop(bench *b)
{
    CHECK_RC(keel_emergency_stop(b->device), KEEL_OK);
    CHECK(state_of(b) == KEEL_STATE_FAULTED);
}

/* ARMED_UNEXPECTED: the enable gate no longer removes ARMED (sticky). */
static void sticky_fault(bench *b)
{
    CHECK_RC(keel_sim_set(b->device, KEEL_SIM_ARMED_STUCK_HIGH, 1), KEEL_OK);
    advance(b, TICK_US); /* ENABLE has been LOW far longer than the 10 ms grace */
    CHECK_RC(tick(b), KEEL_E_FEEDBACK);
    CHECK_RC(keel_sim_set(b->device, KEEL_SIM_ARMED_STUCK_HIGH, 0), KEEL_OK);
    CHECK(fault_of(b).cause == KEEL_CAUSE_ARMED_UNEXPECTED);
}

/* Ticks until one emits a fall, so the last fall is "now". */
static void tick_until_fall(bench *b)
{
    uint32_t falls = sim_of(b).heartbeat_falls;
    unsigned n;
    for (n = 0; n < 10 && sim_of(b).heartbeat_falls == falls; ++n) {
        ticks(b, 1);
    }
    CHECK(sim_of(b).heartbeat_falls > falls);
}

/* Brings a fresh device into @p state with the row's precondition. The main
 * thread is the recorded control owner afterwards (a fresh open before any
 * tick has none, which SET_POWER_UNKNOWN avoids by one early tick). */
static void reach(bench *b, uint32_t state, unsigned setup)
{
    int stall = (setup & SET_POWER_UNKNOWN) &&
                (state == KEEL_STATE_ARMED || state == KEEL_STATE_SERVO);
    open_bench(b, setup);
    if ((setup & SET_POWER_UNKNOWN) && !stall) {
        /* One early tick: one fall, so the heartbeat is not qualified yet. */
        advance(b, 31 * MS);
        CHECK_RC(tick(b), KEEL_OK);
        CHECK(snapshot_of(b).sensor_power == KEEL_SENSOR_UNKNOWN);
        if (state == KEEL_STATE_ARMING) {
            CHECK_RC(keel_request_arm(b->device), KEEL_OK);
            ticks(b, 1);
        } else if (state == KEEL_STATE_FAULTED) {
            user_stop(b);
        }
        CHECK(state_of(b) == state);
        return;
    }
    /* The heartbeat qualifies and READY rises within the first 300 ms. */
    ticks(b, 30);
    if ((setup & SET_COOLED) || state == KEEL_STATE_SERVO) {
        ticks(b, 240); /* the servo cooldown runs 2.2 s from the last READY LOW */
    }
    switch (state) {
    case KEEL_STATE_DISARMED:
        if (setup & SET_HOLDOFF) {
            user_stop(b);
            CHECK_RC(keel_clear_fault(b->device), KEEL_OK);
            ticks(b, 1); /* records the owner again */
        }
        break;
    case KEEL_STATE_ARMING:
        CHECK_RC(keel_request_arm(b->device), KEEL_OK);
        ticks(b, 1); /* the previous tick ran in ARMING */
        break;
    case KEEL_STATE_ARMED:
        arm(b);
        if (setup & SET_DRIVING) {
            CHECK_RC(keel_set_motors(b->device, 400, 400), KEEL_OK);
            ticks(b, 1);
            CHECK(sim_of(b).bridge_left == 400);
        }
        break;
    case KEEL_STATE_SERVO:
        arm(b);
        ticks(b, 3);
        CHECK_RC(keel_servo_start(b->device, 1500, 300), KEEL_OK);
        ticks(b, 1); /* confirms the grant */
        CHECK(snapshot_of(b).flags & KEEL_SNAP_SERVO_GRANTED);
        break;
    default: /* FAULTED */
        if (setup & SET_STICKY) {
            sticky_fault(b);
        } else {
            user_stop(b);
        }
        break;
    }
    CHECK(state_of(b) == state);
    if ((setup & SET_LATE_FALL) && state != KEEL_STATE_FAULTED) {
        /* Fall at t, a tick at t + 28 ms (no pulse due), then the call's tick
         * at t + 71 ms: the gap is legal, the predicted fall is 72 ms. */
        tick_until_fall(b);
        advance(b, 28 * MS);
        CHECK_RC(tick(b), KEEL_OK);
        advance(b, 43 * MS);
    }
    if (setup & SET_LATE_TICK) {
        advance(b, 75 * MS);
    }
    if (setup & SET_TICK_DUE) {
        advance(b, TICK_US);
    }
    if (setup & SET_RANGE_BUSY) {
        CHECK_RC(keel_range_start(b->device), KEEL_OK);
    }
    if (stall) {
        /* No tick for 100 ms: the state stays cached while the heartbeat,
         * and with it the MCU's sensor power, is gone. */
        advance(b, 100 * MS);
        CHECK(state_of(b) == state);
        CHECK(snapshot_of(b).sensor_power == KEEL_SENSOR_OFF);
    }
}

/* ---- the calls, one per row ----------------------------------------------- */

static int32_t act_tick(bench *b)
{
    return tick(b);
}

static int32_t act_tick_bad_health(bench *b)
{
    return keel_tick(b->device, ++b->cycle, KEEL_HEALTH_CONTROL_OK | KEEL_HEALTH_IO_OK);
}

static int32_t act_tick_same_cycle(bench *b)
{
    return keel_tick(b->device, b->cycle, KEEL_HEALTH_ALL);
}

static int32_t cycle_with(bench *b, uint32_t command, int32_t left, int32_t right)
{
    keel_cycle_input in;
    keel_snapshot out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.struct_size = sizeof in;
    in.health = KEEL_HEALTH_ALL;
    in.cycle_id = ++b->cycle;
    in.command = command;
    in.left = left;
    in.right = right;
    out.struct_size = sizeof out;
    return keel_cycle(b->device, &in, &out);
}

static int32_t act_cycle_null(bench *b)
{
    return keel_cycle(b->device, NULL, NULL);
}

static int32_t act_cycle_tail(bench *b)
{
    unsigned char buffer[sizeof(keel_cycle_input) + 8];
    keel_cycle_input in;
    memset(buffer, 0, sizeof buffer);
    memset(&in, 0, sizeof in);
    in.struct_size = sizeof buffer;
    in.health = KEEL_HEALTH_ALL;
    in.cycle_id = b->cycle + 1;
    memcpy(buffer, &in, sizeof in);
    buffer[sizeof in + 3] = 1; /* a field this library does not know */
    return keel_cycle(b->device, (const keel_cycle_input *)(const void *)buffer, NULL);
}

static int32_t act_cycle_none(bench *b)
{
    return cycle_with(b, KEEL_CYCLE_NONE, 0, 0);
}

static int32_t act_cycle_drive(bench *b)
{
    return cycle_with(b, KEEL_CYCLE_DRIVE, 300, 300);
}

static int32_t act_cycle_zero(bench *b)
{
    return cycle_with(b, KEEL_CYCLE_DRIVE, 0, 0);
}

static int32_t act_cycle_brake(bench *b)
{
    return cycle_with(b, KEEL_CYCLE_BRAKE, 0, 0);
}

static int32_t act_cycle_unknown(bench *b)
{
    return cycle_with(b, 7, 0, 0);
}

static int32_t act_request_arm(bench *b)
{
    return keel_request_arm(b->device);
}

static int32_t act_disarm(bench *b)
{
    return keel_disarm(b->device);
}

static int32_t act_emergency_stop(bench *b)
{
    return keel_emergency_stop(b->device);
}

static int32_t act_clear_fault(bench *b)
{
    return keel_clear_fault(b->device);
}

static int32_t act_motors_zero(bench *b)
{
    return keel_set_motors(b->device, 0, 0);
}

static int32_t act_motors(bench *b)
{
    return keel_set_motors(b->device, 300, 300);
}

static int32_t act_motors_reverse(bench *b)
{
    return keel_set_motors(b->device, -300, -300);
}

static int32_t act_motors_range(bench *b)
{
    return keel_set_motors(b->device, 1001, 0);
}

static int32_t act_brake(bench *b)
{
    return keel_brake(b->device);
}

static int32_t act_servo_start(bench *b)
{
    return keel_servo_start(b->device, 1500, 300);
}

static int32_t act_servo_both(bench *b)
{
    int32_t rc = keel_servo_start(b->device, 1500, 300);
    CHECK_RC(keel_servo_set_pulse(b->device, 1500), rc);
    return rc;
}

static int32_t act_servo_pulse(bench *b)
{
    return keel_servo_set_pulse(b->device, 1200);
}

static int32_t act_servo_end(bench *b)
{
    return keel_servo_end(b->device);
}

static int32_t act_range_start(bench *b)
{
    return keel_range_start(b->device);
}

static int32_t act_range_poll(bench *b)
{
    keel_range r;
    memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    return keel_range_poll(b->device, &r);
}

static int32_t act_range_measure(bench *b)
{
    keel_range r;
    int32_t rc;
    memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    rc = keel_range_measure(b->device, &r);
    if (rc == KEEL_OK) {
        CHECK(r.status != KEEL_RANGE_PENDING && r.status != KEEL_RANGE_IDLE);
    }
    return rc;
}

static int32_t act_rgb(bench *b)
{
    static const uint32_t frame[2] = {0x123456u, 0x654321u};
    int32_t rc = keel_set_rgb_all(b->device, 0x010101u);
    CHECK_RC(keel_set_rgb_frame(b->device, frame, 2), rc);
    CHECK_RC(keel_set_rgb(b->device, 0, 0x123456u), rc);
    return rc;
}

static int32_t act_button_leds(bench *b)
{
    return keel_set_button_leds(b->device, KEEL_BUTTON_ALL, KEEL_BUTTON_B);
}

static int32_t act_take_buttons(bench *b)
{
    keel_buttons buttons;
    memset(&buttons, 0, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    return keel_take_buttons(b->device, &buttons);
}

static int32_t act_read_inputs(bench *b)
{
    uint32_t inputs = 0;
    return keel_read_safety_inputs(b->device, &inputs);
}

static int32_t act_queries(bench *b)
{
    keel_snapshot s;
    keel_fault f;
    uint32_t value = 0;
    uint64_t now = 0;
    memset(&s, 0, sizeof s);
    memset(&f, 0, sizeof f);
    s.struct_size = sizeof s;
    f.struct_size = sizeof f;
    CHECK_RC(keel_get_snapshot(b->device, &s), KEEL_OK);
    CHECK_RC(keel_get_fault(b->device, &f), KEEL_OK);
    CHECK_RC(keel_get_capabilities(b->device, &value), KEEL_OK);
    CHECK_RC(keel_get_rgb(b->device, 0, &value), KEEL_OK);
    CHECK_RC(keel_now_us(b->device, &now), KEEL_OK);
    return keel_get_state(b->device, &value);
}

static int32_t act_sleep(bench *b)
{
    uint64_t now = 0;
    CHECK_RC(keel_now_us(b->device, &now), KEEL_OK);
    return keel_sleep_until(b->device, now + 5 * MS);
}

static int32_t act_sim(bench *b)
{
    keel_sim_state s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    CHECK_RC(keel_sim_get_state(b->device, &s), KEEL_OK);
    return keel_sim_set(b->device, KEEL_SIM_BUTTONS, 0);
}

static int32_t act_close(bench *b)
{
    keel_fault f;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    return keel_close(b->device, &f);
}

/* A forked child gets NOT_OWNER from every call and touches nothing. */
static int32_t act_forked(bench *b)
{
    int status = 0;
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        keel_fault f;
        uint32_t state;
        int ok;
        memset(&f, 0, sizeof f);
        f.struct_size = sizeof f;
        ok = keel_get_state(b->device, &state) == KEEL_E_NOT_OWNER &&
             keel_tick(b->device, b->cycle + 1, KEEL_HEALTH_ALL) == KEEL_E_NOT_OWNER &&
             keel_set_motors(b->device, 0, 0) == KEEL_E_NOT_OWNER &&
             keel_disarm(b->device) == KEEL_E_NOT_OWNER &&
             keel_emergency_stop(b->device) == KEEL_E_NOT_OWNER &&
             keel_close(b->device, &f) == KEEL_E_NOT_OWNER;
        _exit(ok ? 0 : 1);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return KEEL_E_NOT_OWNER;
}

/* The table, row by row; the row's call text is checked against the table. */
static const row_spec specs[] = {
    {"keel_tick", SET_TICK_DUE, act_tick},
    {"keel_tick", SET_LATE_TICK, act_tick},
    {"keel_tick", SET_LATE_FALL, act_tick},
    {"keel_tick", SET_TICK_DUE, act_tick_bad_health},
    {"keel_tick", SET_TICK_DUE, act_tick_same_cycle},
    {"keel_cycle", 0, act_cycle_null},
    {"keel_cycle", 0, act_cycle_tail},
    {"keel_cycle", SET_TICK_DUE, act_cycle_none},
    {"keel_cycle", SET_TICK_DUE, act_cycle_drive},
    {"keel_cycle", SET_TICK_DUE | SET_DRIVING, act_cycle_zero},
    {"keel_cycle", SET_TICK_DUE, act_cycle_brake},
    {"keel_cycle", SET_TICK_DUE | SET_DRIVING, act_cycle_unknown},
    {"keel_request_arm", 0, act_request_arm},
    {"keel_request_arm", SET_HOLDOFF, act_request_arm},
    {"keel_disarm", 0, act_disarm},
    {"keel_emergency_stop", 0, act_emergency_stop},
    {"keel_clear_fault", 0, act_clear_fault},
    {"keel_clear_fault", SET_STICKY, act_clear_fault},
    {"keel_set_motors", SET_DRIVING, act_motors_zero},
    {"keel_set_motors", 0, act_motors},
    {"keel_set_motors", SET_DRIVING, act_motors_reverse},
    {"keel_set_motors", SET_DRIVING, act_motors_range},
    {"keel_brake", 0, act_brake},
    {"keel_servo_start", SET_COOLED, act_servo_start},
    {"keel_servo_start", SET_COOLED | SET_DRIVING, act_servo_start},
    {"keel_servo_start", 0, act_servo_start},
    {"keel_servo_start, keel_servo_set_pulse", SET_NO_SERVO, act_servo_both},
    {"keel_servo_set_pulse", 0, act_servo_pulse},
    {"keel_servo_end", 0, act_servo_end},
    {"keel_range_start", 0, act_range_start},
    {"keel_range_start", SET_POWER_UNKNOWN, act_range_start},
    {"keel_range_start", SET_RANGE_BUSY, act_range_start},
    {"keel_range_poll", 0, act_range_poll},
    {"keel_range_measure", 0, act_range_measure},
    {"keel_set_rgb, keel_set_rgb_all, keel_set_rgb_frame", 0, act_rgb},
    {"keel_set_button_leds", 0, act_button_leds},
    {"keel_take_buttons", 0, act_take_buttons},
    {"keel_read_safety_inputs", 0, act_read_inputs},
    {"keel_get_state, keel_get_snapshot, keel_get_fault, keel_get_capabilities, keel_get_rgb, "
     "keel_now_us",
     0, act_queries},
    {"keel_sleep_until", 0, act_sleep},
    {"keel_sim_set, keel_sim_get_state", 0, act_sim},
    {"keel_close", 0, act_close},
    {"any call", 0, act_forked},
};

_Static_assert(sizeof specs / sizeof specs[0] == (size_t)KEEL_ARB_ROW_COUNT,
               "every arbitration row needs a replay spec");

/* ---- running a call on another thread ------------------------------------- */

typedef struct foreign_call {
    bench *b;
    action act;
    int32_t rc;
} foreign_call;

static void *foreign_main(void *argument)
{
    foreign_call *call = argument;
    call->rc = call->act(call->b);
    return NULL;
}

static int32_t run_on(uint32_t thread, bench *b, action act)
{
    foreign_call call;
    pthread_t worker;
    if (thread == KEEL_ARB_OWNER) {
        return act(b);
    }
    call.b = b;
    call.act = act;
    call.rc = KEEL_OK;
    CHECK(pthread_create(&worker, NULL, foreign_main, &call) == 0);
    CHECK(pthread_join(worker, NULL) == 0);
    return call.rc;
}

/* ---- checking one cell ------------------------------------------------------ */

/* What a refusal must leave unchanged. */
typedef struct fingerprint {
    uint32_t state, ack_phase, flags;
    int32_t command[2], output[2];
    uint32_t pins;
    uint32_t fault_count;
} fingerprint;

static fingerprint fingerprint_of(bench *b)
{
    keel_snapshot s = snapshot_of(b);
    fingerprint p;
    memset(&p, 0, sizeof p);
    p.state = s.state;
    p.ack_phase = s.ack_phase;
    p.flags = s.flags & (KEEL_SNAP_REVERSAL_PENDING | KEEL_SNAP_BRAKING | KEEL_SNAP_SERVO_GRANTED);
    p.command[0] = s.command_left;
    p.command[1] = s.command_right;
    p.output[0] = s.output_left;
    p.output[1] = s.output_right;
    p.pins = sim_of(b).pins & PIN_GUARDED;
    p.fault_count = s.fault_count;
    return p;
}

static void check_effects(bench *b, const keel_arb_cell *cell, const keel_snapshot *before,
                          uint64_t time_before)
{
    keel_snapshot s = snapshot_of(b);
    keel_sim_state sim = sim_of(b);
    uint32_t fx = cell->effects;
    if (fx & KEEL_ARB_FX_STOP) {
        CHECK(!(sim.pins & (PIN_ENABLE | PIN_MOTORS | PIN_SERVO_REQUEST)));
        CHECK(sim.bridge_left == 0 && sim.bridge_right == 0);
    }
    if (fx & KEEL_ARB_FX_HB_STOP) {
        CHECK(!(sim.pins & PIN_HEARTBEAT) && !(s.flags & KEEL_SNAP_HEARTBEAT_RUNNING));
    }
    if (fx & KEEL_ARB_FX_LATCH) {
        CHECK_EQ(s.fault_count, before->fault_count + 1u);
    }
    if (fx & KEEL_ARB_FX_CLEAR) {
        CHECK(fault_of(b).flags & KEEL_FAULT_CLEARED);
        CHECK_EQ(s.owner_thread_id, 0);
    }
    if (fx & KEEL_ARB_FX_ACK) {
        CHECK(s.state != KEEL_STATE_ARMING || s.ack_phase != KEEL_ACK_NONE);
    }
    if (fx & KEEL_ARB_FX_COAST) {
        CHECK(s.command_left == 0 && s.command_right == 0 && !(sim.pins & PIN_MOTORS));
        CHECK(!(s.flags & (KEEL_SNAP_BRAKING | KEEL_SNAP_REVERSAL_PENDING)));
    }
    if (fx & KEEL_ARB_FX_DRIVE) {
        CHECK(sim.bridge_left == 300 && sim.bridge_right == 300);
        CHECK(s.command_remaining_ms > 400);
    }
    if (fx & KEEL_ARB_FX_REVERSAL) {
        CHECK(s.flags & KEEL_SNAP_REVERSAL_PENDING);
        CHECK(sim.bridge_left == 0 && s.command_left == -300);
    }
    if (fx & KEEL_ARB_FX_BRAKE) {
        CHECK((sim.flags & (KEEL_SIM_LEFT_BRAKE | KEEL_SIM_RIGHT_BRAKE)) ==
              (KEEL_SIM_LEFT_BRAKE | KEEL_SIM_RIGHT_BRAKE));
        CHECK(s.flags & KEEL_SNAP_BRAKING);
    }
    if (fx & KEEL_ARB_FX_SERVO_REQUEST) {
        CHECK(sim.pins & PIN_SERVO_REQUEST);
        CHECK(s.servo_pulse_us == 1500 && sim.servo_pulse_us == 0); /* no pulse before the grant */
    }
    if (fx & KEEL_ARB_FX_SERVO_END) {
        CHECK(!(sim.pins & PIN_SERVO_REQUEST) && sim.servo_pulse_us == 0);
        CHECK(s.servo_cooldown_remaining_ms > 2000);
    }
    if (fx & KEEL_ARB_FX_SERVO_PULSE) {
        CHECK(s.servo_pulse_us == 1200 && sim.servo_pulse_us == 1200);
    }
    if (fx & KEEL_ARB_FX_GRANT) {
        CHECK(s.state != KEEL_STATE_SERVO || (s.flags & KEEL_SNAP_SERVO_GRANTED));
    }
    if (fx & KEEL_ARB_FX_TRIGGER) {
        CHECK(poll_of(b).sequence > 0);
    }
    if ((fx & KEEL_ARB_FX_TRIGGER) && !(fx & KEEL_ARB_FX_WAIT)) {
        CHECK(poll_of(b).status == KEEL_RANGE_PENDING);
    }
    if (fx & KEEL_ARB_FX_WAIT) {
        keel_range r = poll_of(b);
        CHECK(r.status == KEEL_RANGE_VALID && r.distance_mm == OBJECT_MM);
    }
    if (fx & KEEL_ARB_FX_UNAVAILABLE) {
        CHECK(poll_of(b).status == KEEL_RANGE_UNAVAILABLE);
    }
    if (fx & KEEL_ARB_FX_I2C) {
        uint32_t rgb = 0;
        CHECK_RC(keel_get_rgb(b->device, 0, &rgb), KEEL_OK);
        CHECK_EQ(rgb, 0x123456u);
    }
    if (fx & KEEL_ARB_FX_BUTTON_LEDS) {
        CHECK(sim.pins & PIN_LED_B);
        CHECK_EQ(s.button_leds, KEEL_BUTTON_B);
    }
    if (fx & KEEL_ARB_FX_HOLDOFF) {
        CHECK(s.flags & KEEL_SNAP_REARM_HOLDOFF);
    }
    if (fx & KEEL_ARB_FX_HB_RESTART) {
        CHECK_EQ(s.heartbeat_restarts, before->heartbeat_restarts + 1u);
        CHECK(s.flags & KEEL_SNAP_HEARTBEAT_RESTARTED);
    }
    if (fx & KEEL_ARB_FX_SLEEP) {
        CHECK(sim.now_us >= time_before + 5 * MS);
    }
}

/* Eight on-time owner ticks: the heartbeat runs on exactly when promised.
 * After a stalled loop the first of them latches the late tick itself. */
static void check_heartbeat(bench *b, const keel_arb_cell *cell)
{
    uint32_t falls = sim_of(b).heartbeat_falls;
    unsigned i;
    for (i = 0; i < 8; ++i) {
        int32_t rc;
        advance(b, TICK_US);
        rc = tick(b);
        if (cell->heartbeat) {
            CHECK(keel_status_class(rc) != KEEL_CLASS_FAULT);
        } else if (i == 0 && (cell->flags & KEEL_ARB_LOOP_STALLED)) {
            CHECK_RC(rc, KEEL_E_DEADLINE);
            CHECK_EQ(fault_of(b).cause, KEEL_CAUSE_TICK_LATE);
        } else {
            CHECK_RC(rc, KEEL_E_FAULTED);
        }
    }
    if (cell->heartbeat) {
        CHECK(sim_of(b).heartbeat_falls >= falls + 2u);
    } else {
        CHECK_EQ(sim_of(b).heartbeat_falls, falls);
    }
}

static void replay_cell(size_t row, uint32_t state, uint32_t thread)
{
    const keel_arb_row *r = &keel_arb_table[row];
    const keel_arb_cell *cell = &r->cells[state][thread];
    const row_spec *spec = &specs[row];
    keel_snapshot before;
    fingerprint print;
    uint64_t time_before;
    bench b;
    int32_t rc;

    fprintf(stderr, "row %zu (%s | %s) state %u thread %s\n", row, r->call, r->condition, state,
            thread == KEEL_ARB_OWNER ? "owner" : "other");
    reach(&b, state, spec->setup);
    before = snapshot_of(&b);
    print = fingerprint_of(&b);
    time_before = sim_of(&b).now_us;
    if (thread == KEEL_ARB_OTHER) {
        CHECK(before.owner_thread_id != 0); /* "other" needs a recorded owner */
    }

    rc = run_on(thread, &b, spec->act);
    CHECK_RC(rc, cell->status);
    if (cell->state_after == KEEL_ARB_CLOSED) {
        uint32_t closed_state;
        CHECK_RC(keel_get_state(b.device, &closed_state), KEEL_E_INVALID_HANDLE);
        return;
    }
    CHECK_EQ(state_of(&b), cell->state_after);
    if (keel_status_class(rc) == KEEL_CLASS_REJECTED) {
        CHECK_RC(snapshot_of(&b).last_rejection, rc);
    }
    if (cell->cause != KEEL_CAUSE_NONE) {
        keel_fault f = fault_of(&b);
        CHECK_EQ(f.cause, cell->cause);
        CHECK_RC(f.status, cell->cause == KEEL_CAUSE_USER_STOP ? KEEL_E_FAULTED : cell->status);
        CHECK_EQ(f.fault_count, before.fault_count + 1u);
    } else if (rc != KEEL_E_NOT_OWNER) {
        CHECK_EQ(fault_of(&b).fault_count, before.fault_count); /* nothing new latched */
    }
    if ((cell->effects & ~(uint32_t)PASSIVE_EFFECTS) == 0) {
        fingerprint after = fingerprint_of(&b);
        CHECK(memcmp(&after, &print, sizeof after) == 0);
    }
    check_effects(&b, cell, &before, time_before);
    check_heartbeat(&b, cell);
    CHECK_RC(keel_close(b.device, NULL), KEEL_OK);
}

static void replay_row(size_t row)
{
    uint32_t state, thread;
    unsigned replayed = 0;
    CHECK(strcmp(specs[row].call, keel_arb_table[row].call) == 0);
    for (state = 0; state < KEEL_ARB_STATE_COUNT; ++state) {
        for (thread = 0; thread < KEEL_ARB_THREAD_COUNT; ++thread) {
            if (!(keel_arb_table[row].cells[state][thread].flags & KEEL_ARB_UNREACHABLE)) {
                replay_cell(row, state, thread);
                replayed++;
            }
        }
    }
    CHECK(replayed > 0);
}

int main(int argc, char **argv)
{
    size_t row;
    if (argc != 2) {
        for (row = 0; row < (size_t)KEEL_ARB_ROW_COUNT; ++row) {
            replay_row(row);
        }
        return 0;
    }
    row = (size_t)strtoul(argv[1], NULL, 10);
    if (row >= (size_t)KEEL_ARB_ROW_COUNT) {
        fprintf(stderr, "no row %s\n", argv[1]);
        return 2;
    }
    replay_row(row);
    printf("PASS row %zu\n", row);
    return 0;
}
