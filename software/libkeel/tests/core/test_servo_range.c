/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Servo bursts (SPEC 14) and ranging (SPEC 15) against the production MCU
 * policy, stepped every simulated millisecond by the fixture.
 *
 * The servo cases replay the burst contract the MCU enforces: a request only
 * from ARMED with quiet motors and outside the 2 s cooldown, a lease under
 * 500 ms, mutual exclusion with the motors. The sweep asserts that no host
 * schedule ever trips F_COOLDOWN, F_PWM or F_SERVO_TIMEOUT or grants both
 * permits. The ranging cases cover every result status of the HC-SR04
 * classification and the rules that keep TRIG away from an unpowered sensor.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "check.h"
#include "fixture.h"

enum { MS = 1000 };

static void ticks(fixture *f, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        fx_tick_after(f, 10 * MS);
    }
}

static void check_cause(fixture *f, int32_t status, uint32_t cause)
{
    keel_fault fault = fx_fault(f);
    CHECK_RC(fault.status, status);
    CHECK_EQ(fault.cause, cause);
}

/* ARMED with the servo cooldown over: 2.4 s of DISARMED ticks first (the
 * cooldown runs 2.2 s from the last READY-LOW sample after open). */
static void armed_cooled(fixture *f, unsigned options)
{
    fx_init_options(f, options | FX_SERVO | FX_RANGE);
    ticks(f, 240);
    fx_arm(f);
    ticks(f, 3); /* coasting for 20 ms and more */
    CHECK_EQ(fx_snapshot(f).servo_cooldown_remaining_ms, 0);
}

/* No MCU fault at all during a run that never stopped on purpose. */
static void mcu_clean(fixture *f)
{
    CHECK_EQ(f->board.mcu.last_faults, 0);
    CHECK_EQ(fx_take_trips(f), 0);
}

static void servo_burst(void)
{
    fixture f;
    keel_snapshot s;
    unsigned n;
    armed_cooled(&f, 0);
    CHECK_RC(keel_servo_start(f.device, 1500, 200), KEEL_OK);
    CHECK_EQ(fx_state(&f), KEEL_STATE_SERVO);
    /* The request is up; the pulse waits for the grant. */
    CHECK(f.board.servo_request && f.board.servo_pulse_us == 0);
    s = fx_snapshot(&f);
    CHECK(s.servo_pulse_us == 1500 && s.servo_lease_remaining_ms == 200);
    CHECK(!(s.flags & KEEL_SNAP_SERVO_GRANTED));
    fx_tick_after(&f, 10 * MS);
    CHECK(f.board.servo_pulse_us == 1500 && f.board.permit.servo_permit);
    CHECK(f.board.mcu.stage == S_SERVO && !f.board.permit.motor_permit);
    CHECK(fx_snapshot(&f).flags & KEEL_SNAP_SERVO_GRANTED);
    /* A new width, never a longer lease. */
    CHECK_RC(keel_servo_set_pulse(f.device, 1000), KEEL_OK);
    CHECK(f.board.servo_pulse_us == 1000);
    CHECK(fx_snapshot(&f).servo_lease_remaining_ms <= 190);
    /* Motion, braking and arming conflict with the burst. */
    CHECK_RC(keel_set_motors(f.device, 300, 300), KEEL_E_CONFLICT);
    CHECK_RC(keel_brake(f.device), KEEL_E_CONFLICT);
    CHECK_RC(keel_request_arm(f.device), KEEL_E_CONFLICT);
    CHECK_RC(keel_servo_start(f.device, 1500, 200), KEEL_E_CONFLICT);
    CHECK_RC(keel_set_motors(f.device, 0, 0), KEEL_OK); /* a stop is always fine */
    for (n = 0; n < 30 && fx_state(&f) == KEEL_STATE_SERVO; ++n) {
        ticks(&f, 1);
    }
    /* The lease ended the burst disarmed: pulse, request and ENABLE LOW. */
    CHECK_EQ(fx_state(&f), KEEL_STATE_DISARMED);
    CHECK(!f.board.servo_request && f.board.servo_pulse_us == 0);
    CHECK(!(f.board.outputs & KEEL_OUT_ENABLE));
    s = fx_snapshot(&f);
    CHECK(s.servo_cooldown_remaining_ms > 2150 && s.servo_pulse_us == 0);
    /* Driving again needs a new arm; another burst also the cooldown. */
    CHECK_RC(keel_set_motors(f.device, 300, 300), KEEL_E_STATE);
    fx_arm(&f);
    ticks(&f, 3);
    CHECK_RC(keel_servo_start(f.device, 1500, 200), KEEL_E_AGAIN);
    ticks(&f, 220);
    CHECK_RC(keel_servo_start(f.device, 1500, 200), KEEL_OK);
    ticks(&f, 25);
    CHECK_EQ(fx_state(&f), KEEL_STATE_DISARMED);
    mcu_clean(&f);
    fx_destroy(&f);
}

/* Every lease against every tick period, the loop keeping one period from
 * open to the end of the burst (a loop cannot stretch its period at will:
 * the heartbeat bound counts from the previous fall). The MCU never sees a
 * cooldown, PWM or lease violation and never grants both permits
 * (fx_advance() checks every millisecond). */
static void servo_sweep(void)
{
    static const unsigned leases[] = {20, 150, 400};
    static const unsigned periods[] = {1, 5, 10, 25, 50, 70};
    size_t l, p;
    for (l = 0; l < sizeof leases / sizeof leases[0]; ++l) {
        for (p = 0; p < sizeof periods / sizeof periods[0]; ++p) {
            uint64_t period = periods[p] * MS, deadline = 0;
            int32_t rc = KEEL_E_STATE;
            fixture f;
            unsigned n;
            fx_init_options(&f, FX_SERVO | FX_RANGE);
            CHECK_RC(keel_request_arm(f.device), KEEL_OK);
            /* Ticks on an absolute schedule, as a loop on keel_sleep_until();
             * the burst is retried until the cooldown and the 20 ms coast
             * allow it. */
            for (n = 0; n < 4000 && rc != KEEL_OK; ++n) {
                deadline += period;
                fx_advance(&f, deadline > fx_now(&f) ? deadline : fx_now(&f));
                CHECK_RC(fx_tick(&f), KEEL_OK);
                rc = keel_servo_start(f.device, 1500, leases[l]);
                CHECK(rc == KEEL_OK || rc == KEEL_E_AGAIN || rc == KEEL_E_STATE);
            }
            CHECK_RC(rc, KEEL_OK);
            for (n = 0; n < 1000 && fx_state(&f) == KEEL_STATE_SERVO; ++n) {
                deadline += period;
                fx_advance(&f, deadline > fx_now(&f) ? deadline : fx_now(&f));
                CHECK_RC(fx_tick(&f), KEEL_OK);
            }
            CHECK_EQ(fx_state(&f), KEEL_STATE_DISARMED);
            /* The MCU ended its burst on the request LOW, before its own lease. */
            CHECK(f.board.mcu.stage != S_SERVO && f.board.mcu.stage != S_LATCHED);
            mcu_clean(&f);
            fx_destroy(&f);
        }
    }
}

static void servo_preconditions(void)
{
    fixture f;
    fx_init_options(&f, FX_SERVO | FX_RANGE);
    /* Arguments, then the state. */
    CHECK_RC(keel_servo_start(f.device, 499, 100), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_servo_start(f.device, 1500, 401), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_servo_start(f.device, 1500, 19), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_servo_set_pulse(f.device, 2501), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_E_STATE);
    CHECK_RC(keel_servo_set_pulse(f.device, 1500), KEEL_E_STATE);
    CHECK_RC(keel_servo_end(f.device), KEEL_OK); /* nothing to end */
    fx_destroy(&f);

    armed_cooled(&f, 0);
    /* Driving, braking and a pending reversal conflict; a short coast waits. */
    CHECK_RC(keel_set_motors(f.device, 300, 300), KEEL_OK);
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_E_CONFLICT);
    CHECK_RC(keel_set_motors(f.device, -300, -300), KEEL_OK); /* deferred reversal */
    CHECK(fx_snapshot(&f).flags & KEEL_SNAP_REVERSAL_PENDING);
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_E_CONFLICT);
    CHECK_RC(keel_brake(f.device), KEEL_OK);
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_E_CONFLICT);
    CHECK_RC(keel_set_motors(f.device, 0, 0), KEEL_OK);
    fx_advance(&f, fx_now(&f) + 10 * MS);
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_E_AGAIN);
    CHECK_RC(fx_snapshot(&f).last_rejection, KEEL_E_AGAIN);
    fx_tick_after(&f, 11 * MS);
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_OK);
    CHECK(fx_snapshot(&f).command_left == 0); /* the stored command is gone */
    mcu_clean(&f);
    fx_destroy(&f);

    /* Without the capability: UNSUPPORTED before the state, end is a no-op. */
    fx_init_options(&f, FX_RANGE);
    fx_arm(&f);
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_E_UNSUPPORTED);
    CHECK_RC(keel_servo_set_pulse(f.device, 1500), KEEL_E_UNSUPPORTED);
    CHECK_RC(keel_servo_end(f.device), KEEL_OK);
    CHECK_EQ(fx_snapshot(&f).servo_cooldown_remaining_ms, 0);
    fx_destroy(&f);
}

static void servo_feedback(void)
{
    fixture f;
    /* ARMED does not fall after the request: the MCU refused the grant. */
    armed_cooled(&f, 0);
    f.force_high = KEEL_IN_ARMED;
    CHECK_RC(keel_servo_start(f.device, 1500, 200), KEEL_OK);
    fx_tick_after(&f, 5 * MS); /* still inside the 10 ms grant window */
    fx_advance(&f, fx_now(&f) + 7 * MS);
    CHECK_RC(fx_tick(&f), KEEL_E_FEEDBACK);
    check_cause(&f, KEEL_E_FEEDBACK, KEEL_CAUSE_SERVO_NOT_GRANTED);
    fx_safe(&f);
    fx_destroy(&f);

    /* ARMED HIGH after the grant: both permits at once. */
    armed_cooled(&f, 0);
    CHECK_RC(keel_servo_start(f.device, 1500, 200), KEEL_OK);
    fx_tick_after(&f, 10 * MS);
    CHECK(f.board.servo_pulse_us == 1500);
    f.force_high = KEEL_IN_ARMED;
    fx_advance(&f, fx_now(&f) + 10 * MS);
    CHECK_RC(fx_tick(&f), KEEL_E_FEEDBACK);
    check_cause(&f, KEEL_E_FEEDBACK, KEEL_CAUSE_DUAL_PERMIT);
    fx_safe(&f);
    CHECK_EQ(fx_snapshot(&f).flags & KEEL_SNAP_FAULT_STICKY, 0); /* recoverable */
    fx_destroy(&f);

    /* READY LOW during a burst. */
    armed_cooled(&f, 0);
    CHECK_RC(keel_servo_start(f.device, 1500, 200), KEEL_OK);
    fx_tick_after(&f, 10 * MS);
    f.force_low = KEEL_IN_READY;
    fx_advance(&f, fx_now(&f) + 10 * MS);
    CHECK_RC(fx_tick(&f), KEEL_E_FEEDBACK);
    check_cause(&f, KEEL_E_FEEDBACK, KEEL_CAUSE_READY_LOST);
    fx_safe(&f);
    fx_destroy(&f);
}

/* An MCU reset restarts its 2 s cooldown; the host sees only READY LOW and
 * times 2.2 s from the last such sample. */
static void servo_cooldown_after_reset(void)
{
    fixture f;
    unsigned n;
    fx_init_options(&f, FX_SERVO | FX_RANGE);
    ticks(&f, 240);
    CHECK_RC(keel__board_set(&f.board, KEEL_SIM_MCU_RESET, 1), KEEL_OK);
    for (n = 0; n < 100 && (f.board.permit.ready == 0 || n < 5); ++n) {
        ticks(&f, 1);
    }
    CHECK(f.board.permit.ready);
    CHECK(fx_snapshot(&f).servo_cooldown_remaining_ms > 2000);
    fx_arm(&f);
    ticks(&f, 3);
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_E_AGAIN);
    while (fx_snapshot(&f).servo_cooldown_remaining_ms > 0) {
        ticks(&f, 1);
    }
    CHECK_RC(keel_servo_start(f.device, 1500, 100), KEEL_OK);
    ticks(&f, 15);
    CHECK_EQ(fx_state(&f), KEEL_STATE_DISARMED);
    CHECK(!(f.board.mcu.last_faults & (F_COOLDOWN | F_PWM | F_SERVO_TIMEOUT)));
    fx_destroy(&f);
}

/* Every stop ends a burst with the pulse off and the request LOW. */
static void servo_stops(void)
{
    fixture f;
    armed_cooled(&f, 0);
    CHECK_RC(keel_servo_start(f.device, 1500, 300), KEEL_OK);
    fx_tick_after(&f, 10 * MS);
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    CHECK_EQ(fx_state(&f), KEEL_STATE_DISARMED);
    CHECK(!f.board.servo_request && f.board.servo_pulse_us == 0);
    CHECK(fx_snapshot(&f).servo_cooldown_remaining_ms > 2150);
    mcu_clean(&f);
    fx_destroy(&f);

    armed_cooled(&f, 0);
    CHECK_RC(keel_servo_start(f.device, 1500, 300), KEEL_OK);
    fx_tick_after(&f, 10 * MS);
    CHECK_RC(keel_emergency_stop(f.device), KEEL_OK);
    check_cause(&f, KEEL_E_FAULTED, KEEL_CAUSE_USER_STOP);
    fx_safe(&f);
    fx_destroy(&f);

    armed_cooled(&f, 0);
    CHECK_RC(keel_servo_start(f.device, 1500, 300), KEEL_OK);
    fx_tick_after(&f, 10 * MS);
    CHECK_RC(keel_servo_end(f.device), KEEL_OK);
    CHECK_EQ(fx_state(&f), KEEL_STATE_DISARMED);
    CHECK(!f.board.servo_request && !(f.board.outputs & KEEL_OUT_ENABLE));
    mcu_clean(&f);
    CHECK_RC(keel_close(f.device, NULL), KEEL_OK);
    f.device = NULL;
    fx_safe(&f);
    fx_destroy(&f);
}

/* ---- ranging -------------------------------------------------------------------- */

static keel_range measure(fixture *f, int32_t expected)
{
    keel_range r;
    memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    CHECK_RC(keel_range_measure(f->device, &r), expected);
    return r;
}

static keel_range poll(fixture *f)
{
    keel_range r;
    memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    CHECK_RC(keel_range_poll(f->device, &r), KEEL_OK);
    return r;
}

/* Measures an object at @p mm from a DISARMED, powered sensor. */
static keel_range measure_at(fixture *f, int32_t mm)
{
    uint64_t start;
    keel_range r;
    ticks(f, 7); /* the 60 ms trigger spacing */
    CHECK_RC(keel__board_set(&f->board, KEEL_SIM_RANGE_MM, mm), KEEL_OK);
    start = fx_now(f);
    r = measure(f, KEEL_OK);
    CHECK(fx_now(f) - start <= KEEL_RANGE_MEASURE_MAX_MS * MS);
    CHECK(r.status != KEEL_RANGE_PENDING && r.status != KEEL_RANGE_IDLE);
    return r;
}

static void range_statuses(void)
{
    fixture f;
    keel_range r;
    uint32_t sequence;
    fx_init_options(&f, FX_SERVO | FX_RANGE);
    ticks(&f, 30);
    CHECK_EQ(fx_snapshot(&f).sensor_power, KEEL_SENSOR_PERMITTED);

    r = measure_at(&f, 1000);
    CHECK(r.status == KEEL_RANGE_VALID && r.distance_mm == 1000);
    CHECK(r.echo_us > 5800 && r.echo_us < 5850);
    CHECK_EQ(fx_snapshot(&f).sensor_power, KEEL_SENSOR_CONFIRMED);
    sequence = r.sequence;
    r = measure_at(&f, 21);
    CHECK(r.status == KEEL_RANGE_VALID && r.distance_mm == 21 && r.sequence == sequence + 1u);
    r = measure_at(&f, 3990);
    CHECK(r.status == KEEL_RANGE_VALID && r.distance_mm == 3990);
    /* Too close to time: a pulse under 117 us. */
    r = measure_at(&f, 10);
    CHECK(r.status == KEEL_RANGE_INVALID && r.distance_mm == 0);
    /* Beyond 4 m, and nothing in range (still HIGH at 30 ms). */
    r = measure_at(&f, 5000);
    CHECK(r.status == KEEL_RANGE_NO_ECHO && r.distance_mm == 0);
    r = measure_at(&f, -1);
    CHECK(r.status == KEEL_RANGE_NO_ECHO && r.distance_mm == 0);
    /* A locked-out sensor never answers: UNAVAILABLE after 5 ms. */
    CHECK_RC(keel__board_set(&f.board, KEEL_SIM_SENSOR_LOCKOUT, 1), KEEL_OK);
    r = measure_at(&f, 1000);
    CHECK(r.status == KEEL_RANGE_UNAVAILABLE);
    CHECK_RC(keel__board_set(&f.board, KEEL_SIM_SENSOR_LOCKOUT, 0), KEEL_OK);
    /* Confirmation fades 500 ms after the last echo. */
    ticks(&f, 60);
    CHECK_EQ(fx_snapshot(&f).sensor_power, KEEL_SENSOR_PERMITTED);
    fx_destroy(&f);
}

static void range_rules(void)
{
    fixture f;
    keel_range r;
    unsigned triggers;
    fx_init_options(&f, FX_SERVO | FX_RANGE);
    /* Before the heartbeat qualifies, nothing drives TRIG. */
    triggers = f.trigger_calls;
    r = measure(&f, KEEL_OK);
    CHECK(r.status == KEEL_RANGE_UNAVAILABLE && r.sequence == 1 && r.measured_at_us == fx_now(&f));
    CHECK_RC(keel_range_start(f.device), KEEL_OK);
    CHECK(poll(&f).status == KEEL_RANGE_UNAVAILABLE && poll(&f).sequence == 2);
    CHECK_EQ(f.trigger_calls, triggers);
    ticks(&f, 30);

    /* Start and poll; the spacing is never waited for. */
    CHECK_RC(keel__board_set(&f.board, KEEL_SIM_RANGE_MM, 500), KEEL_OK);
    CHECK_RC(keel_range_start(f.device), KEEL_OK);
    CHECK_EQ(poll(&f).status, KEEL_RANGE_PENDING);
    CHECK_RC(keel_range_start(f.device), KEEL_E_AGAIN);
    fx_advance(&f, fx_now(&f) + 5 * MS);
    r = poll(&f);
    CHECK(r.status == KEEL_RANGE_VALID && r.distance_mm == 500);
    CHECK_RC(keel_range_start(f.device), KEEL_E_AGAIN); /* under 60 ms since the trigger */
    r = measure(&f, KEEL_E_AGAIN);
    CHECK_EQ(r.status, KEEL_RANGE_VALID); /* the output still holds the latest result */
    CHECK_RC(fx_snapshot(&f).last_rejection, KEEL_E_AGAIN);

    /* READY falling during a measurement invalidates it. */
    ticks(&f, 7);
    CHECK_RC(keel_range_start(f.device), KEEL_OK);
    CHECK_RC(keel__board_set(&f.board, KEEL_SIM_MCU_RESET, 1), KEEL_OK);
    fx_tick_after(&f, 1 * MS);
    CHECK_EQ(poll(&f).status, KEEL_RANGE_INVALID);
    ticks(&f, 30);

    /* FAULTED: no heartbeat, so no sensor power and no trigger. */
    CHECK_RC(keel_emergency_stop(f.device), KEEL_OK);
    triggers = f.trigger_calls;
    r = measure(&f, KEEL_OK);
    CHECK_EQ(r.status, KEEL_RANGE_UNAVAILABLE);
    CHECK_EQ(f.trigger_calls, triggers);
    fx_destroy(&f);

    /* Without the capability: UNSUPPORTED, and the output reads IDLE. */
    fx_init_options(&f, FX_SERVO);
    memset(&r, 0xA5, sizeof r);
    r.struct_size = sizeof r;
    CHECK_RC(keel_range_poll(f.device, &r), KEEL_E_UNSUPPORTED);
    CHECK(r.status == KEEL_RANGE_IDLE && r.sequence == 0 && r.reserved0 == 0);
    CHECK_RC(keel_range_start(f.device), KEEL_E_UNSUPPORTED);
    fx_destroy(&f);
}

/* The control owner may not block while a heartbeat deadline is armed;
 * another thread may. */
static void range_owner_rule(void)
{
    fixture f;
    keel_range r;
    fx_init_options(&f, FX_THREADS | FX_SERVO | FX_RANGE);
    ticks(&f, 30);
    CHECK_EQ(fx_snapshot(&f).owner_thread_id, 1);
    r = measure(&f, KEEL_OK); /* DISARMED: the owner may wait */
    CHECK_EQ(r.status, KEEL_RANGE_NO_ECHO);
    CHECK_RC(keel_request_arm(f.device), KEEL_OK);
    ticks(&f, 7);
    r = measure(&f, KEEL_E_CONFLICT);
    CHECK_RC(fx_snapshot(&f).last_rejection, KEEL_E_CONFLICT);
    f.thread = 2;
    CHECK_RC(keel__board_set(&f.board, KEEL_SIM_RANGE_MM, 800), KEEL_OK);
    r = measure(&f, KEEL_OK);
    CHECK(r.status == KEEL_RANGE_VALID && r.distance_mm == 800);
    f.thread = 1;
    CHECK_EQ(fx_state(&f), KEEL_STATE_ARMING);
    fx_tick_after(&f, 5 * MS); /* the measurement took under 35 ms of the window */
    fx_destroy(&f);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"servo_burst", servo_burst},
        {"servo_sweep", servo_sweep},
        {"servo_preconditions", servo_preconditions},
        {"servo_feedback", servo_feedback},
        {"servo_cooldown_after_reset", servo_cooldown_after_reset},
        {"servo_stops", servo_stops},
        {"range_statuses", range_statuses},
        {"range_rules", range_rules},
        {"range_owner_rule", range_owner_rule},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
