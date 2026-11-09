/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Reference-model suite: the core against the production MCU policy
 * (hash-guarded copy in src/backend/sim/mcu), stepped every simulated
 * millisecond. This is the safety regression net for the heartbeat and
 * acknowledgement timing, the motion rules and the fault handling.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "check.h"
#include "fixture.h"

static void check_cause(fixture *f, int32_t status, uint32_t cause)
{
    keel_fault fault = fx_fault(f);
    CHECK_RC(fault.status, status);
    CHECK_EQ(fault.cause, cause);
}

/* Heartbeat and acknowledgement against the real MCU for every tick period
 * from 1 to 70 ms: arming completes with exactly two ENABLE rises, no MCU
 * fault, and falls 30-70 ms apart. */
static void schedule_sweep(void)
{
    unsigned period;
    for (period = 1; period <= 70; ++period) {
        fixture f;
        uint64_t deadline = 0;
        size_t i;
        fx_init(&f);
        CHECK_RC(keel_request_arm(f.device), KEEL_OK);
        while (deadline < 1500000) {
            if (fx_now(&f) < deadline) {
                fx_advance(&f, deadline);
            }
            CHECK_RC(fx_tick(&f), KEEL_OK);
            deadline += period * 1000u;
        }
        CHECK(fx_state(&f) == KEEL_STATE_ARMED);
        CHECK(f.board.mcu.stage == S_ARMED && f.board.mcu.last_faults == 0);
        CHECK_EQ(f.enable_rises, 2);
        CHECK(f.nfalls > 10);
        for (i = 1; i < f.nfalls; ++i) {
            CHECK(f.falls[i] - f.falls[i - 1] >= 30000);
            CHECK(f.falls[i] - f.falls[i - 1] <= 70000);
        }
        CHECK_RC(keel_set_motors(f.device, 500, 750), KEEL_OK);
        CHECK_RC(keel_disarm(f.device), KEEL_OK);
        fx_safe(&f);
        fx_destroy(&f);
    }
}

/* An acknowledgement interrupted after its second LOW leaves the MCU in its
 * final-HIGH wait. The next arm's HIGH1 completes that stale wait; the host
 * returns to LOW1 and runs a complete acknowledgement. */
static void stale_ack(void)
{
    fixture f;
    uint64_t low_at;
    unsigned rises;
    fx_init(&f);
    CHECK_RC(keel_request_arm(f.device), KEEL_OK);
    while (fx_snapshot(&f).ack_phase != KEEL_ACK_LOW2) {
        fx_tick_after(&f, 10000);
    }
    low_at = fx_now(&f);
    while (fx_now(&f) - low_at < 120000) {
        fx_tick_after(&f, 10000);
    }
    CHECK(f.board.mcu.stage == S_WAIT_H2);
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    rises = f.enable_rises;
    fx_arm(&f);
    CHECK_EQ(f.enable_rises, rises + 3); /* stale completion, then the full two-HIGH ACK */
    CHECK(f.board.mcu.last_faults == 0);
    CHECK_EQ(fx_snapshot(&f).stale_ack_restarts, 1);
    fx_destroy(&f);
}

/* Tick periods of 71-100 ms never let the MCU arm and fail the second tick. */
static void invalid_schedules(void)
{
    unsigned period;
    for (period = 71; period <= 100; ++period) {
        fixture f;
        unsigned n;
        int32_t rc = KEEL_OK;
        fx_init(&f);
        CHECK_RC(keel_request_arm(f.device), KEEL_OK);
        for (n = 0; n < 5 && rc == KEEL_OK; ++n) {
            fx_advance(&f, fx_now(&f) + period * 1000u);
            rc = fx_tick(&f);
            CHECK(!f.board.permit.motor_permit);
        }
        CHECK_RC(rc, KEEL_E_DEADLINE);
        check_cause(&f, KEEL_E_DEADLINE, KEEL_CAUSE_TICK_LATE);
        fx_safe(&f);
        fx_destroy(&f);
    }
}

static void clock_and_cycle_id(void)
{
    fixture f;
    uint64_t prior;
    fx_init(&f);
    fx_arm(&f);
    prior = f.board.now_us;
    f.board.now_us -= 1; /* a broken monotonic clock */
    /* E_INTERNAL with the sticky cause CLOCK. */
    CHECK_RC(fx_tick(&f), KEEL_E_INTERNAL);
    check_cause(&f, KEEL_E_INTERNAL, KEEL_CAUSE_CLOCK);
    CHECK(fx_fault(&f).flags & KEEL_FAULT_STICKY);
    fx_safe(&f);
    f.board.now_us = prior;
    CHECK_RC(keel_clear_fault(f.device), KEEL_E_FAULTED);
    CHECK_RC(keel_request_arm(f.device), KEEL_E_FAULTED);
    fx_destroy(&f); /* sticky: only close and reopen recover */

    fx_init(&f);
    fx_arm(&f);
    CHECK_RC(keel_tick(f.device, UINT64_MAX, KEEL_HEALTH_ALL), KEEL_OK);
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    CHECK_RC(keel_request_arm(f.device), KEEL_OK);
    /* Re-arming does not reset cycle-id freshness, and uint64 wrap is refused. */
    CHECK_RC(keel_tick(f.device, 0, KEEL_HEALTH_ALL), KEEL_E_HEALTH);
    check_cause(&f, KEEL_E_HEALTH, KEEL_CAUSE_CYCLE_NOT_INCREASING);
    fx_safe(&f);
    fx_destroy(&f);
}

/* An early tick followed by a long gap misses the fall window although no
 * single loop interval exceeds 70 ms. */
static void early_call_long_gap(void)
{
    fixture f;
    size_t falls;
    fx_init(&f);
    fx_arm(&f);
    while (!f.nfalls || fx_now(&f) != f.falls[f.nfalls - 1]) {
        fx_tick_after(&f, 10000);
    }
    fx_tick_after(&f, 20000);
    falls = f.nfalls;
    fx_advance(&f, fx_now(&f) + 60000);
    CHECK_RC(fx_tick(&f), KEEL_E_DEADLINE);
    check_cause(&f, KEEL_E_DEADLINE, KEEL_CAUSE_HEARTBEAT_LATE);
    CHECK(f.nfalls == falls);
    fx_safe(&f);
    fx_destroy(&f);
}

static void health_and_deadlines(void)
{
    fixture f;
    size_t falls;
    fx_init(&f);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, 800, 800), KEEL_OK);
    CHECK_RC(keel_tick(f.device, f.cycle, KEEL_HEALTH_ALL), KEEL_E_HEALTH);
    check_cause(&f, KEEL_E_HEALTH, KEEL_CAUSE_CYCLE_NOT_INCREASING);
    fx_safe(&f);
    fx_no_more_heartbeat(&f);
    fx_recover(&f); /* clear_fault and the hold-off first */
    CHECK_RC(keel_tick(f.device, ++f.cycle, KEEL_HEALTH_CONTROL_OK), KEEL_E_HEALTH);
    check_cause(&f, KEEL_E_HEALTH, KEEL_CAUSE_HEALTH_FLAGS);
    CHECK_EQ(fx_fault(&f).detail, KEEL_HEALTH_IO_OK | KEEL_HEALTH_NO_EXCEPTION);
    fx_safe(&f);
    fx_no_more_heartbeat(&f);
    fx_recover(&f);
    CHECK_RC(keel_set_motors(f.device, 800, 800), KEEL_OK);
    falls = f.nfalls;
    fx_advance(&f, fx_now(&f) + 81000);
    CHECK(fx_left(&f) == 0 && fx_right(&f) == 0); /* the 70 ms lease ran out */
    /* The MCU window is timed from the last fall, which it samples 1 ms
     * later. SPEC 9.3 pulses after evaluating, so the arming tick ends with a
     * fall at the very moment of this command: wait until 82 ms after that
     * fall. */
    if (f.falls[f.nfalls - 1] + 82000 > fx_now(&f)) {
        fx_advance(&f, f.falls[f.nfalls - 1] + 82000);
    }
    CHECK(!f.board.permit.motor_permit);
    CHECK_RC(fx_tick(&f), KEEL_E_DEADLINE);
    CHECK(f.nfalls == falls);
    fx_safe(&f);
    fx_no_more_heartbeat(&f);
    fx_recover(&f);
    CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_OK);
    fx_advance(&f, fx_now(&f) + 71000);
    CHECK_RC(keel_set_motors(f.device, 600, 600), KEEL_E_DEADLINE);
    check_cause(&f, KEEL_E_DEADLINE, KEEL_CAUSE_TICK_LATE);
    fx_safe(&f);
    fx_destroy(&f);
}

static void feedback_and_fault(void)
{
    static const uint32_t missing_inputs[2] = {KEEL_IN_READY, KEEL_IN_ARMED};
    static const uint32_t causes[2] = {KEEL_CAUSE_READY_LOST, KEEL_CAUSE_ARMED_LOST};
    unsigned i;
    for (i = 0; i < 2; ++i) {
        fixture f;
        fx_init(&f);
        fx_arm(&f);
        CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_OK);
        f.force_low = missing_inputs[i];
        CHECK_RC(fx_tick(&f), KEEL_E_FEEDBACK);
        check_cause(&f, KEEL_E_FEEDBACK, causes[i]);
        fx_safe(&f);
        fx_no_more_heartbeat(&f);
        f.force_low = 0;
        fx_recover(&f);
        f.board.fault_low = 1;
        f.board.fault_edge = 1;
        fx_advance(&f, fx_now(&f) + 1000);
        CHECK(!f.board.permit.motor_permit);
        CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_E_FEEDBACK);
        fx_safe(&f);
        f.board.fault_low = 0;
        fx_recover(&f);
        fx_destroy(&f);
    }
}

static void motion_and_reversal(void)
{
    fixture f;
    uint64_t stopped;
    fx_init(&f);
    /* Refused without side effect. */
    CHECK_RC(keel_set_motors(f.device, 1, 1), KEEL_E_STATE);
    CHECK(fx_state(&f) == KEEL_STATE_DISARMED);
    fx_safe(&f);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, 1000, 500), KEEL_OK);
    CHECK(fx_left(&f) == 1000 && fx_right(&f) == 500);
    /* A reversal is accepted and deferred. */
    CHECK_RC(keel_set_motors(f.device, -1000, 500), KEEL_OK);
    CHECK(fx_left(&f) == 0 && fx_right(&f) == 0);
    CHECK(fx_snapshot(&f).flags & KEEL_SNAP_REVERSAL_PENDING);
    stopped = fx_now(&f);
    fx_advance(&f, stopped + 19000);
    CHECK_RC(keel_set_motors(f.device, -1000, 500), KEEL_OK);
    CHECK(fx_left(&f) == 0 && fx_right(&f) == 0);
    fx_advance(&f, stopped + 20000);
    CHECK_RC(keel_set_motors(f.device, -1000, 500), KEEL_OK);
    CHECK(fx_left(&f) == -1000 && fx_right(&f) == 500);
    CHECK(!(fx_snapshot(&f).flags & KEEL_SNAP_REVERSAL_PENDING));
    CHECK_RC(keel_set_motors(f.device, 0, 0), KEEL_OK);
    CHECK(!f.board.levels && fx_left(&f) == 0 && fx_right(&f) == 0);
    stopped = fx_now(&f);
    CHECK_RC(keel_set_motors(f.device, 1000, 500), KEEL_OK); /* deferred by the dwell */
    CHECK(fx_left(&f) == 0);
    fx_advance(&f, stopped + 20000);
    CHECK_RC(keel_set_motors(f.device, 1000, 500), KEEL_OK);
    CHECK(fx_left(&f) == 1000);
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    CHECK_RC(keel_set_motors(f.device, -1000, -500), KEEL_E_STATE);
    fx_safe(&f);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, -1000, -500), KEEL_OK);
    /* An out-of-range duty coasts and stays ARMED. */
    CHECK_RC(keel_set_motors(f.device, 1001, 0), KEEL_E_INVALID_ARGUMENT);
    CHECK(fx_state(&f) == KEEL_STATE_ARMED && (f.board.outputs & KEEL_OUT_ENABLE));
    CHECK(fx_left(&f) == 0 && fx_right(&f) == 0 && !f.board.levels);
    CHECK_RC(fx_snapshot(&f).last_rejection, KEEL_E_INVALID_ARGUMENT);
    /* The first tick after the dwell applies a pending reversal. */
    CHECK_RC(keel_set_motors(f.device, -500, -500), KEEL_OK);
    CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_OK);
    fx_tick_after(&f, 10000);
    CHECK(fx_left(&f) == 0 && (fx_snapshot(&f).flags & KEEL_SNAP_REVERSAL_PENDING));
    fx_tick_after(&f, 10000);
    CHECK(fx_left(&f) == 500 && fx_right(&f) == 500);
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    fx_safe(&f);
    fx_destroy(&f);
}

/* Backend latch priority (SPEC 12.3): a worker failure keeps its own status
 * and cause. */
static void backend_latch_priority(void)
{
    fixture f;
    keel_buttons buttons;
    keel_fault fault;
    fx_init(&f);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_OK);
    /* Named regression: a PWM worker overrun between two ticks with no pulse
     * due makes keel_take_buttons() report it, not ARMED_LOST or E_IO. */
    (void)keel__board_latch(&f.board, KEEL_E_PWM, KEEL_CAUSE_PWM_OVERRUN, 0, 31000);
    CHECK(!(f.board.outputs & KEEL_OUT_ENABLE) && fx_left(&f) == 0);
    memset(&buttons, 0, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    CHECK_RC(keel_take_buttons(f.device, &buttons), KEEL_E_PWM);
    fault = fx_fault(&f);
    CHECK_RC(fault.status, KEEL_E_PWM);
    CHECK_EQ(fault.cause, KEEL_CAUSE_PWM_OVERRUN);
    CHECK_EQ(fault.detail, 31000);
    CHECK(fx_state(&f) == KEEL_STATE_FAULTED);
    fx_safe(&f);
    CHECK_RC(fx_tick(&f), KEEL_E_FAULTED);
    /* Stops never reset the backend latch; only clear_fault does. */
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    CHECK_RC(f.board.fault.status, KEEL_E_PWM);
    fx_recover(&f);
    CHECK_RC(f.board.fault.status, KEEL_OK);
    CHECK(fx_fault(&f).flags & KEEL_FAULT_CLEARED);

    /* The latch racing a successful read is caught by the motor write. */
    f.latch_after_read = f.read_calls + 1;
    CHECK_RC(keel_set_motors(f.device, 600, 600), KEEL_E_PWM);
    check_cause(&f, KEEL_E_PWM, KEEL_CAUSE_PWM_OVERRUN);
    fx_safe(&f);
    fx_recover(&f);

    /* A reversal only coasts, which is always applied; the next call that
     * reads the inputs records the worker's fault. */
    CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_OK);
    f.latch_after_read = f.read_calls + 1;
    CHECK_RC(keel_set_motors(f.device, -500, 500), KEEL_OK);
    CHECK(fx_left(&f) == 0 && fx_right(&f) == 0);
    CHECK_RC(fx_tick(&f), KEEL_E_PWM);
    check_cause(&f, KEEL_E_PWM, KEEL_CAUSE_PWM_OVERRUN);
    fx_safe(&f);
    fx_destroy(&f);
}

static void take(fixture *f, uint32_t held, uint32_t pressed, uint32_t released)
{
    keel_buttons buttons;
    memset(&buttons, 0, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    CHECK_RC(keel_take_buttons(f->device, &buttons), KEEL_OK);
    CHECK_EQ(buttons.held, held);
    CHECK_EQ(buttons.pressed, pressed);
    CHECK_EQ(buttons.released, released);
}

static void buttons(void)
{
    fixture f;
    fx_init_held(&f, KEEL_BUTTON_A);
    take(&f, 0, 0, 0);
    fx_advance(&f, 19000);
    take(&f, 0, 0, 0);
    fx_advance(&f, 20000);
    take(&f, KEEL_BUTTON_A, KEEL_BUTTON_A, 0);
    take(&f, KEEL_BUTTON_A, 0, 0);
    f.board.buttons = KEEL_BUTTON_A | KEEL_BUTTON_B;
    take(&f, KEEL_BUTTON_A, 0, 0);
    fx_advance(&f, 30000);
    f.board.buttons = KEEL_BUTTON_A;
    take(&f, KEEL_BUTTON_A, 0, 0);
    fx_advance(&f, 40000);
    f.board.buttons = KEEL_BUTTON_A | KEEL_BUTTON_B;
    take(&f, KEEL_BUTTON_A, 0, 0);
    fx_advance(&f, 59000);
    take(&f, KEEL_BUTTON_A, 0, 0);
    fx_advance(&f, 60000);
    take(&f, KEEL_BUTTON_A | KEEL_BUTTON_B, KEEL_BUTTON_B, 0);
    f.board.buttons = KEEL_BUTTON_B;
    take(&f, KEEL_BUTTON_A | KEEL_BUTTON_B, 0, 0);
    fx_advance(&f, 80000);
    take(&f, KEEL_BUTTON_B, 0, KEEL_BUTTON_A);
    CHECK_RC(keel_set_button_leds(f.device, KEEL_BUTTON_ALL, KEEL_BUTTON_X | KEEL_BUTTON_Y),
             KEEL_OK);
    CHECK_EQ(f.board.outputs & KEEL_OUT_BUTTON_LEDS,
             (KEEL_BUTTON_X | KEEL_BUTTON_Y) << KEEL_OUT_BUTTON_LED_SHIFT);
    /* Masked update; only the bits in the mask change. */
    CHECK_RC(keel_set_button_leds(f.device, KEEL_BUTTON_X | KEEL_BUTTON_A, KEEL_BUTTON_A), KEEL_OK);
    CHECK_EQ(f.board.outputs & KEEL_OUT_BUTTON_LEDS,
             (KEEL_BUTTON_A | KEEL_BUTTON_Y) << KEEL_OUT_BUTTON_LED_SHIFT);
    CHECK_RC(keel_set_button_leds(f.device, 16, 16), KEEL_E_INVALID_ARGUMENT);
    fx_destroy(&f);
}

/* Slow backend operations may lengthen acknowledgement levels and the
 * reversal dwell, never shorten them; one blocked past the window fails. */
static void blocking_callbacks(void)
{
    uint32_t phase;
    unsigned zero_first;
    for (phase = KEEL_ACK_LOW1; phase <= KEEL_ACK_LOW2; ++phase) {
        fixture f;
        fx_init(&f);
        CHECK_RC(keel_request_arm(f.device), KEEL_OK);
        while (fx_snapshot(&f).ack_phase != phase) {
            fx_tick_after(&f, 10000);
        }
        f.delay_enable = 30000;
        while (fx_state(&f) != KEEL_STATE_ARMED) {
            fx_tick_after(&f, 10000);
        }
        CHECK_EQ(f.nenable_changes, 3);
        CHECK(f.enable_changes[1] - f.enable_changes[0] >= 150000);
        CHECK(f.enable_changes[2] - f.enable_changes[1] >= 150000);
        CHECK(fx_now(&f) - f.enable_changes[2] >= 150000);
        CHECK(f.board.mcu.stage == S_ARMED && f.board.mcu.last_faults == 0);
        fx_destroy(&f);
    }
    for (zero_first = 0; zero_first <= 1; ++zero_first) {
        fixture f;
        uint64_t coast_completed;
        fx_init(&f);
        fx_arm(&f);
        CHECK_RC(keel_set_motors(f.device, 1000, 1000), KEEL_OK);
        f.delay_motor_zero = 15000;
        /* OK in both cases. */
        CHECK_RC(keel_set_motors(f.device, zero_first ? 0 : -1000, zero_first ? 0 : -1000), KEEL_OK);
        coast_completed = fx_now(&f);
        CHECK(fx_left(&f) == 0 && fx_right(&f) == 0);
        fx_advance(&f, coast_completed + 19000);
        CHECK_RC(keel_set_motors(f.device, -1000, -1000), KEEL_OK); /* deferred by the dwell */
        CHECK(fx_left(&f) == 0);
        fx_advance(&f, coast_completed + 20000);
        CHECK_RC(keel_set_motors(f.device, -1000, -1000), KEEL_OK);
        CHECK(fx_left(&f) == -1000 && fx_right(&f) == -1000);
        fx_destroy(&f);
    }
    {
        fixture f;
        int32_t rc = KEEL_OK;
        fx_init(&f);
        CHECK_RC(keel_request_arm(f.device), KEEL_OK);
        f.delay_enable = 71000;
        while (rc == KEEL_OK) {
            fx_advance(&f, fx_now(&f) + 10000);
            rc = fx_tick(&f);
        }
        CHECK_RC(rc, KEEL_E_DEADLINE);
        fx_safe(&f);
        fx_no_more_heartbeat(&f);
        fx_destroy(&f);
    }
    {
        fixture f;
        fx_init(&f);
        fx_arm(&f);
        CHECK_RC(keel_set_motors(f.device, 1000, 1000), KEEL_OK);
        f.delay_motor_return = 71000;
        CHECK_RC(fx_tick(&f), KEEL_E_DEADLINE); /* delayed PWM lease renewal */
        fx_safe(&f);
        fx_no_more_heartbeat(&f);
        fx_recover(&f);
        f.delay_motor_return = 71000;
        CHECK_RC(keel_set_motors(f.device, 1000, 1000), KEEL_E_DEADLINE);
        fx_safe(&f);
        fx_destroy(&f);
    }
    {
        fixture f;
        fx_prepare(&f, KEEL_BUTTON_A);
        f.delay_read = 30000;
        CHECK_RC(fx_attach(&f, NULL), KEEL_OK);
        take(&f, 0, 0, 0);
        fx_advance(&f, 49000);
        take(&f, 0, 0, 0);
        fx_advance(&f, 50000);
        take(&f, KEEL_BUTTON_A, KEEL_BUTTON_A, 0);
        fx_destroy(&f);
    }
}

static void leds(void)
{
    static const uint8_t expected_regs[] = {0x00, 0x17, 0x01, 0x13, 0x16, 0x00};
    fixture f;
    size_t i, start;
    uint32_t rgb = 0;
    fx_init(&f);
    CHECK_EQ(f.nwrites, sizeof expected_regs);
    for (i = 0; i < sizeof expected_regs; ++i) {
        CHECK_EQ(f.writes[i].address, 0x54);
        CHECK_EQ(f.writes[i].reg, expected_regs[i]);
    }
    CHECK(f.writes[0].data[0] == 0 && f.writes[5].data[0] == 1);
    CHECK(f.writes[2].count == 18 && f.writes[3].count == 3);
    for (i = 0; i < 18; ++i) {
        CHECK_EQ(f.writes[2].data[i], 0);
    }
    for (i = 0; i < 3; ++i) {
        CHECK_EQ(f.writes[3].data[i], 0x3f);
    }
    start = f.nwrites;
    CHECK_RC(keel_set_rgb(f.device, KEEL_LED_RGB3, 0x0C2238), KEEL_OK);
    CHECK_EQ(f.nwrites, start + 2);
    CHECK(f.writes[start].reg == 1 && f.writes[start].count == 18);
    for (i = 0; i < 18; ++i) {
        uint8_t expected = i == 6 ? 12 : i == 7 ? 34 : i == 8 ? 56 : 0;
        CHECK_EQ(f.writes[start].data[i], expected);
    }
    CHECK_EQ(f.writes[start + 1].reg, 0x16);
    /* The driver shows the frame only after the latch write. */
    CHECK(f.board.led_output[6] == 12 && f.board.led_output[7] == 34 &&
          f.board.led_output[8] == 56);
    start = f.nwrites;
    CHECK_RC(keel_set_rgb_all(f.device, 0x070809), KEEL_OK);
    for (i = 0; i < 18; ++i) {
        CHECK_EQ(f.writes[start].data[i], 7 + i % 3);
    }
    CHECK_RC(keel_set_rgb(f.device, 6, 0x010203), KEEL_E_INVALID_ARGUMENT);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, 400, 400), KEEL_OK);
    f.fail_i2c = (unsigned)f.nwrites + 2; /* successful frame, failing latch */
    /* A peripheral failure that never disarms. */
    CHECK_RC(keel_set_rgb_all(f.device, 0x010203), KEEL_E_PERIPHERAL);
    CHECK(fx_state(&f) == KEEL_STATE_ARMED && fx_left(&f) == 400);
    CHECK_RC(keel_get_rgb(f.device, 0, &rgb), KEEL_OK);
    CHECK_EQ(rgb, 0x070809); /* the cache keeps the last good frame */
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    fx_destroy(&f);
    /* close puts the driver into shutdown */
    CHECK(f.writes[f.nwrites - 1].reg == 0x00 && f.writes[f.nwrites - 1].data[0] == 0);
    CHECK_EQ(f.board.led_registers[0], 0);
}

static void failures(void)
{
    unsigned fault;
    for (fault = 0; fault < 4; ++fault) {
        fixture f;
        fx_init(&f);
        fx_arm(&f);
        CHECK_RC(keel_set_motors(f.device, 400, 400), KEEL_OK);
        if (fault == 0) {
            f.fail_motor = f.motor_calls + 1;
            CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_E_IO);
            check_cause(&f, KEEL_E_IO, KEEL_CAUSE_MOTOR_WRITE);
        } else if (fault == 1) {
            f.fail_read = f.read_calls + 1;
            CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_E_IO);
            check_cause(&f, KEEL_E_IO, KEEL_CAUSE_GPIO_READ);
        } else {
            fx_advance(&f, fx_now(&f) + 30000); /* the next tick must pulse */
            if (fault == 2) {
                f.fail_sleep = f.sleep_calls + 1;
                CHECK_RC(fx_tick(&f), KEEL_E_IO);
                check_cause(&f, KEEL_E_IO, KEEL_CAUSE_CLOCK);
            } else {
                f.extra_sleep = 50000;
                CHECK_RC(fx_tick(&f), KEEL_E_DEADLINE);
                check_cause(&f, KEEL_E_DEADLINE, KEEL_CAUSE_HEARTBEAT_LATE);
            }
        }
        fx_safe(&f);
        fx_no_more_heartbeat(&f);
        fx_destroy(&f);
    }
    for (fault = 1; fault <= 6; ++fault) {
        fixture f;
        keel_open_report report;
        uint32_t caps = 0;
        fx_prepare(&f, 0);
        f.fail_i2c = fault;
        memset(&report, 0, sizeof report);
        report.struct_size = sizeof report;
        /* A missing LED driver degrades the device. */
        CHECK_RC(fx_attach(&f, &report), KEEL_OK);
        CHECK(report.warnings & KEEL_OPEN_WARN_NO_RGB);
        CHECK_EQ(report.resource, KEEL_RESOURCE_LED_DRIVER);
        CHECK_RC(keel_get_capabilities(f.device, &caps), KEEL_OK);
        CHECK(!(caps & KEEL_CAP_RGB) && caps == report.capabilities);
        CHECK_RC(keel_set_rgb(f.device, 0, 0xFFFFFF), KEEL_E_UNSUPPORTED);
        fx_safe(&f);
        fx_destroy(&f);
    }
    {
        fixture f;
        unsigned motors;
        fx_init(&f);
        fx_arm(&f);
        CHECK_RC(keel_set_motors(f.device, 400, 400), KEEL_OK);
        motors = f.motor_calls;
        f.fail_disable = f.disable_calls + 1; /* lowering ENABLE fails */
        CHECK_RC(keel_disarm(f.device), KEEL_E_IO);
        CHECK(fx_state(&f) == KEEL_STATE_FAULTED);
        check_cause(&f, KEEL_E_IO, KEEL_CAUSE_GPIO_WRITE);
        /* Later stop actions ran despite the first failure. */
        CHECK(f.motor_calls > motors);
        CHECK(fx_left(&f) == 0 && fx_right(&f) == 0 && !(f.board.outputs & KEEL_OUT_HEARTBEAT));
        fx_advance(&f, fx_now(&f) + 81000);
        CHECK(!f.board.permit.motor_permit);
        CHECK_RC(fx_tick(&f), KEEL_E_FAULTED);
        fx_destroy(&f);
    }
}

static void fresh_safety_diagnostics(void)
{
    fixture f;
    uint32_t inputs = 9;
    size_t falls;
    unsigned writes;
    fx_init(&f);
    fx_arm(&f);
    falls = f.nfalls;
    writes = f.output_calls;
    CHECK_RC(keel_read_safety_inputs(f.device, &inputs), KEEL_OK);
    CHECK_EQ(inputs, KEEL_INPUT_READY | KEEL_INPUT_ARMED);
    CHECK(f.nfalls == falls && f.output_calls == writes);
    fx_advance(&f, fx_now(&f) + 120000);
    CHECK_RC(keel_read_safety_inputs(f.device, &inputs), KEEL_OK);
    CHECK_EQ(inputs, 0);
    CHECK(f.nfalls == falls && f.output_calls == writes);
    CHECK(fx_state(&f) == KEEL_STATE_ARMED); /* diagnostics do not alter cached state */
    /* (0, 0) is a stop, accepted in every state. */
    CHECK_RC(keel_set_motors(f.device, 0, 0), KEEL_OK);
    f.fail_read = f.read_calls + 1;
    CHECK_RC(keel_read_safety_inputs(f.device, &inputs), KEEL_E_IO);
    fx_safe(&f);
    fx_destroy(&f);
}

/* ---- further cases on the same reference model ------------------------- */

/* A command lives for the command timeout and then coasts without a fault. */
static void command_expiry(void)
{
    fixture f;
    unsigned n;
    fx_init(&f);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, 300, -300), KEEL_OK);
    /* 45 ticks plus the 1 ms pulses stay inside the default 500 ms. */
    for (n = 0; n < 45; ++n) {
        fx_tick_after(&f, 10000);
    }
    CHECK(fx_left(&f) == 300 && fx_right(&f) == -300);
    for (n = 0; n < 6; ++n) {
        fx_tick_after(&f, 10000);
    }
    CHECK(fx_left(&f) == 0 && fx_right(&f) == 0);
    CHECK(fx_state(&f) == KEEL_STATE_ARMED);
    CHECK(fx_snapshot(&f).flags & KEEL_SNAP_COMMAND_EXPIRED);
    CHECK(f.board.mcu.last_faults == 0);
    fx_destroy(&f);
}

/* A late tick while DISARMED restarts the heartbeat instead of faulting. */
static void disarmed_restart(void)
{
    fixture f;
    keel_snapshot snapshot;
    fx_init(&f);
    fx_tick_after(&f, 40000);
    fx_tick_after(&f, 200000); /* late while DISARMED: restarts the heartbeat */
    snapshot = fx_snapshot(&f);
    CHECK(snapshot.flags & KEEL_SNAP_HEARTBEAT_RESTARTED);
    CHECK_EQ(snapshot.heartbeat_restarts, 1);
    CHECK(fx_state(&f) == KEEL_STATE_DISARMED);
    fx_tick_after(&f, 10000);
    CHECK(!(fx_snapshot(&f).flags & KEEL_SNAP_HEARTBEAT_RESTARTED));
    fx_arm(&f); /* the MCU re-qualifies the heartbeat by itself */
    fx_destroy(&f);
}

static void emergency_stop_and_holdoff(void)
{
    fixture f;
    keel_fault fault;
    unsigned n;
    fx_init(&f);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_OK);
    CHECK_RC(keel_emergency_stop(f.device), KEEL_OK);
    fault = fx_fault(&f);
    CHECK_RC(fault.status, KEEL_E_FAULTED);
    CHECK_EQ(fault.cause, KEEL_CAUSE_USER_STOP);
    fx_safe(&f);
    CHECK_RC(keel_request_arm(f.device), KEEL_E_FAULTED); /* refused in FAULTED */
    CHECK_RC(keel_clear_fault(f.device), KEEL_OK);
    CHECK(fx_fault(&f).flags & KEEL_FAULT_CLEARED);
    CHECK_RC(keel_request_arm(f.device), KEEL_E_STATE);
    CHECK(fx_snapshot(&f).flags & KEEL_SNAP_REARM_HOLDOFF);
    for (n = 0; n < 101; ++n) {
        fx_tick_after(&f, 10000);
    }
    fx_arm(&f);
    CHECK_RC(keel_request_arm(f.device), KEEL_OK); /* idempotent while ARMED */
    CHECK(fx_state(&f) == KEEL_STATE_ARMED);
    fx_destroy(&f);
}

/* Brake while ARMED drives all four inputs HIGH and never trips the MCU. */
static void brake(void)
{
    fixture f;
    fx_init(&f);
    CHECK_RC(keel_brake(f.device), KEEL_E_STATE);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, 600, 600), KEEL_OK);
    CHECK_RC(keel_brake(f.device), KEEL_OK);
    CHECK_EQ(f.board.levels, KEEL__PWM_ALL);
    CHECK(fx_snapshot(&f).flags & KEEL_SNAP_BRAKING);
    fx_tick_after(&f, 10000);
    fx_tick_after(&f, 10000);
    CHECK_EQ(f.board.levels, KEEL__PWM_ALL);
    /* Braking counts as idle time, so the reversal applies at once. */
    CHECK_RC(keel_set_motors(f.device, -600, -600), KEEL_OK);
    CHECK(fx_left(&f) == -600);
    CHECK(f.board.mcu.stage == S_ARMED && f.board.mcu.last_faults == 0);
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    fx_safe(&f);
    fx_destroy(&f);
}

/* ARMED HIGH while ENABLE is LOW means the gate no longer removes enable. */
static void armed_unexpected(void)
{
    fixture f;
    fx_init(&f);
    fx_tick_after(&f, 40000);
    f.board.armed_stuck = 1;
    CHECK_RC(fx_tick(&f), KEEL_E_FEEDBACK);
    check_cause(&f, KEEL_E_FEEDBACK, KEEL_CAUSE_ARMED_UNEXPECTED);
    CHECK(fx_fault(&f).flags & KEEL_FAULT_STICKY);
    CHECK(fx_snapshot(&f).flags & KEEL_SNAP_FAULT_STICKY);
    CHECK_RC(keel_clear_fault(f.device), KEEL_E_FAULTED);
    f.board.armed_stuck = 0;
    fx_destroy(&f);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"schedule_sweep", schedule_sweep},
        {"stale_ack", stale_ack},
        {"invalid_schedules", invalid_schedules},
        {"clock_and_cycle_id", clock_and_cycle_id},
        {"early_call_long_gap", early_call_long_gap},
        {"health_and_deadlines", health_and_deadlines},
        {"feedback_and_fault", feedback_and_fault},
        {"motion_and_reversal", motion_and_reversal},
        {"backend_latch_priority", backend_latch_priority},
        {"buttons", buttons},
        {"blocking_callbacks", blocking_callbacks},
        {"leds", leds},
        {"failures", failures},
        {"fresh_safety_diagnostics", fresh_safety_diagnostics},
        {"command_expiry", command_expiry},
        {"disarmed_restart", disarmed_restart},
        {"emergency_stop_and_holdoff", emergency_stop_and_holdoff},
        {"brake", brake},
        {"armed_unexpected", armed_unexpected},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
