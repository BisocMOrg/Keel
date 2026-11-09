/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Seeded random walk (SPEC 23.1): 10 000 random public calls against the
 * production MCU policy on the virtual clock. Each case uses its own seed;
 * KEEL_TEST_SEED replaces it to replay a failure.
 *
 * The sequences are legal: ticks come on time except for deliberate late
 * ticks, injected failures are I/O errors a real backend reports. Every
 * simulated millisecond (fixture FX_INVARIANTS): no bridge input HIGH while
 * the MCU does not permit the motors, both inputs of a bridge HIGH only as a
 * brake, the servo request only with idle motors, never both MCU permits,
 * and no MCU fault the host could have caused (F_PWM, F_COOLDOWN,
 * F_SERVO_TIMEOUT, F_FEEDBACK, ...). After every call: FAULTED means every
 * output LOW, heartbeat falls come only from ticks and 20-80 ms apart unless
 * the heartbeat was stopped or restarted on purpose, F_ACK only follows a
 * stop during arming and F_HEARTBEAT only a stopped or restarted heartbeat.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fixture.h"

enum { MS = 1000, STEPS = 10000 };

typedef struct walk {
    fixture f;
    uint64_t rng;
    uint64_t hb_excused_until;  /* F_HEARTBEAT is explained until then */
    uint64_t ack_excused_until; /* F_ACK is explained until then */
    int hb_break;               /* the heartbeat stopped or restarted since the last fall */
    size_t falls;               /* fall_count at the last check */
    uint64_t last_fall_us;      /* time of the fall before those */
    uint32_t restarts;          /* heartbeat_restarts at the last check */
    unsigned visited;           /* bit per state seen */
    int ticked;                 /* the current call is a tick or cycle */
} walk;

/* xorshift64*: small, fast and identical on every platform. */
static uint32_t random32(walk *w)
{
    w->rng ^= w->rng >> 12;
    w->rng ^= w->rng << 25;
    w->rng ^= w->rng >> 27;
    return (uint32_t)((w->rng * UINT64_C(2685821657736338717)) >> 32);
}

static uint32_t below(walk *w, uint32_t n)
{
    return random32(w) % n;
}

static int32_t duty(walk *w)
{
    /* Mostly valid duties, sometimes one out of range. */
    return below(w, 40) == 0 ? 1001 + (int32_t)below(w, 100) : (int32_t)below(w, 2001) - 1000;
}

static int32_t do_tick(walk *w, int allow_late)
{
    fixture *f = &w->f;
    keel_cycle_input in;
    keel_snapshot out;
    uint32_t gap = 1 + below(w, 15);
    if (allow_late && below(w, 150) == 0) {
        keel_snapshot s = fx_snapshot(f);
        /* A late tick: legal, and it stops (or restarts) the heartbeat. With
         * the motors active it is left to the reference tests: the backend
         * lease (70 ms from the last tick) may outlast the MCU's window (80 ms
         * from the last fall) by a few ms, which the per-millisecond
         * "no PWM without permit" invariant would report although the AND
         * gate already removes bridge enable then. */
        if (s.output_left == 0 && s.output_right == 0 &&
            !(s.flags & (KEEL_SNAP_BRAKING | KEEL_SNAP_REVERSAL_PENDING))) {
            gap = 71 + below(w, 40);
            w->hb_excused_until = fx_now(f) + (gap + 300u) * MS;
            w->hb_break = 1;
        }
    }
    fx_advance(f, fx_now(f) + gap * MS);
    w->ticked = 1;
    if (below(w, 2)) {
        return fx_tick(f);
    }
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.struct_size = sizeof in;
    in.health = KEEL_HEALTH_ALL;
    in.cycle_id = ++f->cycle;
    in.command = below(w, 8) == 0 ? below(w, 4) : below(w, 3);
    in.left = duty(w);
    in.right = duty(w);
    in.flags = below(w, 2);
    out.struct_size = sizeof out;
    return keel_cycle(f->device, &in, &out);
}

static void inject_failure(walk *w)
{
    fixture *f = &w->f;
    switch (below(w, 8)) {
    case 0:
        f->fail_read = f->read_calls + 1u;
        break;
    case 1:
        f->fail_output = f->output_calls + 1u;
        break;
    case 2:
        f->fail_motor = f->motor_calls + 1u;
        break;
    case 3:
        f->fail_disable = f->disable_calls + 1u;
        break;
    case 4:
        f->fail_servo = f->servo_calls + 1u;
        break;
    case 5:
        f->fail_i2c = (unsigned)f->nwrites + 1u;
        break;
    case 6:
        f->fail_trigger = f->trigger_calls + 1u;
        break;
    default:
        f->fail_edges = f->edges_calls + 1u;
        break;
    }
}

static void stimulus(walk *w)
{
    fixture *f = &w->f;
    switch (below(w, 3)) {
    case 0:
        CHECK_RC(keel__board_set(&f->board, KEEL_SIM_BUTTONS, (int32_t)below(w, 16)), KEEL_OK);
        break;
    case 1:
        CHECK_RC(keel__board_set(&f->board, KEEL_SIM_RANGE_MM, (int32_t)below(w, 5000) - 1),
                 KEEL_OK);
        break;
    default:
        /* Late PWM wakes below every latch threshold. */
        CHECK_RC(keel__board_set(&f->board, KEEL_SIM_PWM_LATENESS_US, (int32_t)below(w, 2500)),
                 KEEL_OK);
        break;
    }
}

/* The calls the walk makes. */
enum action {
    A_TICK, A_MOTORS, A_ZERO, A_BRAKE, A_ARM, A_DISARM, A_ESTOP, A_CLEAR, A_SERVO_START,
    A_SERVO_PULSE, A_SERVO_END, A_RANGE_START, A_RANGE_POLL, A_RANGE_MEASURE, A_RGB,
    A_BUTTONS, A_INPUTS, A_BUTTON_LEDS, A_STIMULUS, A_FAILURE, A_SNAPSHOT, A_COUNT
};

/* Phases of 1000 calls each. Chaos mixes everything, including stops, late
 * ticks and injected failures; the calm phases leave the device alone long
 * enough to arm, outlast the 2.2 s servo cooldown and drive or burst. */
enum phase { P_CHAOS, P_SERVO, P_DRIVE, P_COUNT, PHASE_STEPS = 1000 };

/* Relative weights per phase, in ::action order. */
static const uint16_t weights[P_COUNT][A_COUNT] = {
    /* chaos */ {520, 80, 20, 20, 40, 8, 4, 38, 40, 15, 5, 30, 30, 5, 15, 15, 15, 15, 75, 4, 6},
    /* servo */ {600, 0, 10, 0, 50, 0, 0, 40, 80, 30, 5, 30, 30, 5, 20, 20, 20, 10, 20, 0, 10},
    /* drive */ {550, 150, 30, 30, 40, 0, 0, 40, 0, 0, 0, 30, 30, 5, 20, 20, 20, 10, 25, 0, 0},
};

static enum action pick(walk *w, enum phase phase)
{
    uint32_t total = 0, r;
    unsigned a;
    for (a = 0; a < A_COUNT; ++a) {
        total += weights[phase][a];
    }
    r = below(w, total);
    for (a = 0; a < A_COUNT; ++a) {
        if (r < weights[phase][a]) {
            break;
        }
        r -= weights[phase][a];
    }
    return (enum action)a;
}

/* One random call; the status is judged by the invariants, not predicted. */
static void step(walk *w, enum phase phase)
{
    fixture *f = &w->f;
    keel_device *d = f->device;
    keel_range range;
    keel_buttons buttons;
    uint32_t value = 0;
    memset(&range, 0, sizeof range);
    range.struct_size = sizeof range;
    memset(&buttons, 0, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    w->ticked = 0;
    switch (pick(w, phase)) {
    case A_TICK:
        (void)do_tick(w, phase == P_CHAOS);
        break;
    case A_MOTORS:
        (void)keel_set_motors(d, duty(w), duty(w));
        break;
    case A_ZERO:
        (void)keel_set_motors(d, 0, 0);
        break;
    case A_BRAKE:
        (void)keel_brake(d);
        break;
    case A_ARM:
        (void)keel_request_arm(d);
        break;
    case A_DISARM:
        (void)keel_disarm(d);
        break;
    case A_ESTOP:
        (void)keel_emergency_stop(d);
        break;
    case A_CLEAR:
        (void)keel_clear_fault(d);
        break;
    case A_SERVO_START:
        (void)keel_servo_start(d, 500 + below(w, 2001), 20 + below(w, 381));
        break;
    case A_SERVO_PULSE:
        (void)keel_servo_set_pulse(d, 500 + below(w, 2001));
        break;
    case A_SERVO_END:
        (void)keel_servo_end(d);
        break;
    case A_RANGE_START:
        (void)keel_range_start(d);
        break;
    case A_RANGE_POLL:
        (void)keel_range_poll(d, &range);
        break;
    case A_RANGE_MEASURE:
        (void)keel_range_measure(d, &range);
        break;
    case A_RGB:
        (void)keel_set_rgb(d, below(w, KEEL_LED_COUNT), random32(w));
        break;
    case A_BUTTONS:
        (void)keel_take_buttons(d, &buttons);
        break;
    case A_INPUTS:
        (void)keel_read_safety_inputs(d, &value);
        break;
    case A_BUTTON_LEDS:
        (void)keel_set_button_leds(d, below(w, 16), below(w, 16));
        break;
    case A_STIMULUS:
        stimulus(w);
        break;
    case A_FAILURE:
        inject_failure(w);
        break;
    default:
        (void)fx_snapshot(f);
        break;
    }
}

static void check_after(walk *w, uint32_t state_before)
{
    fixture *f = &w->f;
    uint32_t state = fx_state(f), trips;
    keel_snapshot s = fx_snapshot(f);
    w->visited |= 1u << state;
    if (state == KEEL_STATE_FAULTED) {
        fx_safe(f); /* every output LOW, no servo request or pulse */
        /* Legal sequences never reach a sticky (defect) cause. */
        CHECK(!(fx_fault(f).flags & KEEL_FAULT_STICKY));
        /* The heartbeat is stopped: the MCU may time it out, and the first
         * fall after a clear comes late on purpose. */
        w->hb_excused_until = fx_now(f) + 300u * MS;
        w->hb_break = 1;
    }
    if (state_before == KEEL_STATE_ARMING &&
        (state == KEEL_STATE_DISARMED || state == KEEL_STATE_FAULTED)) {
        /* Lowering ENABLE inside the MCU's acknowledgement window trips F_ACK. */
        w->ack_excused_until = fx_now(f) + 5u * MS;
    }
    if (s.heartbeat_restarts != w->restarts) {
        /* A restart follows a gap the MCU may already have timed out. */
        w->restarts = s.heartbeat_restarts;
        w->hb_break = 1;
        w->hb_excused_until = fx_now(f) + 300u * MS;
    }
    if (f->fall_count != w->falls) {
        CHECK(w->ticked);                     /* falls only from ticks */
        CHECK_EQ(f->fall_count, w->falls + 1u); /* one pulse per tick */
        if (!w->hb_break && w->falls > 0) {
            uint64_t interval = f->last_fall_us - w->last_fall_us;
            CHECK(interval >= 20u * MS && interval <= 80u * MS);
        }
        w->falls = f->fall_count;
        w->last_fall_us = f->last_fall_us;
        w->hb_break = state == KEEL_STATE_FAULTED;
    }
    trips = fx_take_trips(f);
    if (trips & F_ACK) {
        CHECK(f->mcu_trip_us <= w->ack_excused_until);
    }
    if (trips & F_HEARTBEAT) {
        CHECK(f->mcu_trip_us <= w->hb_excused_until);
    }
    CHECK_EQ(trips & ~(uint32_t)(F_ACK | F_HEARTBEAT), 0);
}

static void run_walk(uint64_t seed)
{
    const char *replay = getenv("KEEL_TEST_SEED");
    walk w;
    unsigned i;
    if (replay && *replay) {
        seed = strtoull(replay, NULL, 0);
    }
    fprintf(stderr, "walk seed 0x%016" PRIx64 " (replay with KEEL_TEST_SEED)\n", seed);
    memset(&w, 0, sizeof w);
    w.rng = seed ? seed : 1u;
    fx_init_options(&w.f, FX_SERVO | FX_RANGE | FX_INVARIANTS);
    for (i = 0; i < STEPS; ++i) {
        uint32_t before = fx_state(&w.f);
        step(&w, (enum phase)((i / PHASE_STEPS) % P_COUNT));
        check_after(&w, before);
    }
    fprintf(stderr, "walk: %u calls, %.1f s simulated, %zu heartbeat falls, %u motor writes\n",
            STEPS, (double)fx_now(&w.f) / 1e6, w.f.fall_count, w.f.motor_calls);
    /* The walk is only meaningful if it reached every state. */
    CHECK_EQ(w.visited, (1u << (KEEL_STATE_FAULTED + 1)) - 1u);
    /* An injection still armed would fail the close. */
    w.f.fail_read = w.f.fail_output = w.f.fail_motor = w.f.fail_disable = w.f.fail_servo = 0;
    fx_destroy(&w.f);
}

static void seed_a(void)
{
    run_walk(UINT64_C(0x6b65656c00000001));
}

static void seed_b(void)
{
    run_walk(UINT64_C(0x9e3779b97f4a7c15));
}

static void seed_c(void)
{
    run_walk(UINT64_C(0x0123456789abcdef));
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"seed_a", seed_a},
        {"seed_b", seed_b},
        {"seed_c", seed_c},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
