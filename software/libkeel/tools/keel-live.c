/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel-live: hardware qualification runner (SPEC 23.1, layer "hardware";
 * SPEC 24.2). Each mode exercises one part of the robot through the public
 * API and writes JSON Lines to stdout:
 *
 *   {"type":"result","test":T,"status":S,"detail":D,"code":N,"code_name":C}
 *   {"type":"event","event":E,"detail":D}
 *   {"type":"summary","mode":M,"passed":N,"failed":N,"needs_observation":N,"exit_code":N}
 *
 * S is PASS, FAIL, NEEDS_OBSERVATION (a person must confirm what no sensor
 * can, such as an LED's colour) or SKIP. Hardware access needs --programmed,
 * the operator's confirmation that the MCU runs the verified safety
 * firmware; --motors and --kill-test also need --wheels-raised.
 * --simulator runs the same sequences against the simulator, without
 * hardware. Exit status: 0 passed, 1 failed, 2 usage, 77 preconditions not
 * met or no hardware, 128+N after signal N, 130 after button B.
 *
 * Every heartbeat follows a completed foreground iteration (a fresh button
 * read, then the tick) on the device clock; no helper thread invents
 * progress. A signal or button B ends the run with an emergency stop.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tool_common.h"

enum mode {
    MODE_IO,
    MODE_LEDS,
    MODE_BUTTONS,
    MODE_SAFETY,
    MODE_MOTORS,
    MODE_RANGE,
    MODE_PWM_JITTER,
    MODE_KILL_TEST,
    MODE_COUNT
};

static const char *const mode_names[MODE_COUNT] = {"io",     "leds",  "buttons",    "safety",
                                                   "motors", "range", "pwm-jitter", "kill-test"};

/* Not a keel status: the operator stopped the run (signal or button B). */
enum { LIVE_STOPPED = 1 };

enum {
    US_PER_MS = 1000,
    TICK_US = KEEL_TICK_PERIOD_MS * US_PER_MS,
    LED_LEVEL = 64,            /* one colour channel at a quarter of full scale */
    MOTOR_DUTY = 200,          /* permille: one wheel, gently */
    MOTOR_RUN_MS = 250,
    STOP_WINDOW_MS = 20,       /* ARMED must fall this soon after a stop */
    ARM_WAIT_MS = 2500,
    HEARTBEAT_GAP_MS = 120,    /* beyond the MCU's 80 ms heartbeat window */
    RANGE_READINGS = 20,
    RANGE_DISTANCE_MM = 500,   /* the simulator's object */
    SIM_PRESS_MS = 150         /* the simulated operator's press length */
};

typedef struct live {
    keel_device *dev;
    int simulator;
    uint64_t cycle;
    uint64_t next_us;
    uint64_t started_us;
    uint32_t hold_ms;
    keel_buttons buttons;
    unsigned passed, failed, observations;
    const char *operation;
    int button_abort;
} live;

/* ---- output --------------------------------------------------------------- */

static void result(live *l, const char *test, const char *status, const char *detail,
                   int32_t code)
{
    if (strcmp(status, "PASS") == 0) {
        l->passed++;
    } else if (strcmp(status, "FAIL") == 0) {
        l->failed++;
    } else if (strcmp(status, "NEEDS_OBSERVATION") == 0) {
        l->observations++;
    }
    fputs("{\"type\":\"result\",\"test\":", stdout);
    tool_json_string(stdout, test);
    fputs(",\"status\":", stdout);
    tool_json_string(stdout, status);
    fputs(",\"detail\":", stdout);
    tool_json_string(stdout, detail);
    printf(",\"code\":%" PRId32 ",\"code_name\":", code);
    tool_json_string(stdout, keel_status_name(code));
    fputs("}\n", stdout);
    fflush(stdout);
}

static void event(const char *name, const char *detail)
{
    fputs("{\"type\":\"event\",\"event\":", stdout);
    tool_json_string(stdout, name);
    fputs(",\"detail\":", stdout);
    tool_json_string(stdout, detail);
    fputs("}\n", stdout);
    fflush(stdout);
}

/* The record behind a FAULT-class result, for the archive. */
static void fault_event(const live *l)
{
    keel_fault fault;
    tool_get_fault(l->dev, &fault);
    if (fault.status == KEEL_OK) {
        return;
    }
    fputs("{\"type\":\"event\",\"event\":\"fault\",\"status\":", stdout);
    tool_json_string(stdout, keel_status_name(fault.status));
    fputs(",\"cause\":", stdout);
    tool_json_string(stdout, keel_fault_cause_name(fault.cause));
    printf(",\"detail\":%" PRIu64 ",\"os_error\":%" PRId32 ",\"state\":", fault.detail,
           fault.os_error);
    tool_json_string(stdout, tool_state_name(fault.state));
    fputs(",\"cleanup_status\":", stdout);
    tool_json_string(stdout, keel_status_name(fault.cleanup_status));
    fputs("}\n", stdout);
    fflush(stdout);
}

/* ---- the loop --------------------------------------------------------------- */

static uint64_t now_us(const live *l)
{
    uint64_t now = 0;
    (void)keel_now_us(l->dev, &now);
    return now;
}

static uint32_t state_of(const live *l)
{
    uint32_t state = KEEL_STATE_FAULTED;
    (void)keel_get_state(l->dev, &state);
    return state;
}

/* The next 10 ms slot on the device clock; a loop that fell behind starts
 * a new schedule instead of bursting to catch up. */
static int32_t wait_slot(live *l)
{
    uint64_t now = now_us(l);
    l->next_us += TICK_US;
    if (l->next_us < now) {
        l->next_us = now;
    }
    return keel_sleep_until(l->dev, l->next_us);
}

/* One iteration: a fresh button read, then the tick. */
static int32_t service(live *l, int abort_on_b)
{
    int32_t rc;
    if (tool_signal) {
        return LIVE_STOPPED;
    }
    l->operation = "button_read";
    l->buttons.struct_size = sizeof l->buttons;
    rc = keel_take_buttons(l->dev, &l->buttons);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (abort_on_b && (l->buttons.held & KEEL_BUTTON_B)) {
        l->button_abort = 1;
        l->operation = "operator_abort";
        return LIVE_STOPPED;
    }
    l->operation = "health_tick";
    return keel_tick(l->dev, ++l->cycle, KEEL_HEALTH_ALL);
}

static int32_t pump(live *l, uint32_t duration_ms, int abort_on_b)
{
    uint64_t end = now_us(l) + (uint64_t)duration_ms * US_PER_MS;
    do {
        int32_t rc = service(l, abort_on_b);
        if (rc == KEEL_OK) {
            rc = wait_slot(l);
        }
        if (rc != KEEL_OK) {
            return rc;
        }
    } while (now_us(l) < end);
    return KEEL_OK;
}

/* Sleeps without ticking (the heartbeat deliberately stops). */
static int32_t pause_ms(live *l, uint32_t duration_ms)
{
    int32_t rc = keel_sleep_until(l->dev, now_us(l) + (uint64_t)duration_ms * US_PER_MS);
    l->next_us = now_us(l);
    return tool_signal ? LIVE_STOPPED : rc;
}

static int32_t read_feedback(live *l, uint32_t *ready, uint32_t *armed)
{
    uint32_t inputs = 0;
    int32_t rc;
    l->operation = "safety_feedback_read";
    rc = keel_read_safety_inputs(l->dev, &inputs);
    *ready = (inputs & KEEL_INPUT_READY) ? 1u : 0u;
    *armed = (inputs & KEEL_INPUT_ARMED) ? 1u : 0u;
    return rc;
}

/* A fresh READY/ARMED read compared with what the MCU must show;
 * @p expected_ready -1 accepts either READY level. */
static int32_t feedback(live *l, int expected_ready, uint32_t expected_armed, const char *test)
{
    uint32_t ready, armed;
    char detail[160];
    int32_t rc = read_feedback(l, &ready, &armed);
    if (rc != KEEL_OK) {
        return rc;
    }
    snprintf(detail, sizeof detail, "Fresh GPIO feedback: READY=%" PRIu32 ", ARMED=%" PRIu32,
             ready, armed);
    if (armed != expected_armed || (expected_ready >= 0 && ready != (uint32_t)expected_ready)) {
        result(l, test, "FAIL", detail, KEEL_E_FEEDBACK);
        l->operation = "unexpected_safety_feedback";
        return KEEL_E_FEEDBACK;
    }
    result(l, test, "PASS", detail, KEEL_OK);
    return KEEL_OK;
}

/* ARMED must fall within 20 ms of a stop. With @p ticking the loop keeps
 * its health contract meanwhile, so the pass cannot come from a starved
 * heartbeat; after an emergency stop the heartbeat is stopped on purpose. */
static int32_t wait_stopped(live *l, const char *test, int ticking)
{
    uint64_t started = now_us(l);
    uint32_t ready = 0, armed = 1;
    char detail[192];
    do {
        int32_t rc = read_feedback(l, &ready, &armed);
        if (rc != KEEL_OK) {
            return rc;
        }
        if (!armed) {
            snprintf(detail, sizeof detail,
                     "Fresh ARMED feedback LOW after the stop; READY=%" PRIu32
                     "; observed after %" PRIu64 " us of polling",
                     ready, now_us(l) - started);
            result(l, test, "PASS", detail, KEEL_OK);
            return KEEL_OK;
        }
        rc = ticking ? service(l, 1) : KEEL_OK;
        if (rc == KEEL_OK) {
            rc = keel_sleep_until(l->dev, now_us(l) + 2 * US_PER_MS);
        }
        if (rc != KEEL_OK) {
            return rc;
        }
    } while (now_us(l) - started < STOP_WINDOW_MS * US_PER_MS);
    result(l, test, "FAIL", "ARMED feedback stayed HIGH through the 20 ms observation window",
           KEEL_E_FEEDBACK);
    l->operation = "stop_feedback_timeout";
    return KEEL_E_FEEDBACK;
}

/* Arms with both duties zero; ARMED is entered only after the library saw
 * READY and ARMED HIGH, so this verifies the feedback path, not motion. */
static int32_t arm_zero(live *l, const char *test)
{
    uint64_t started;
    int32_t rc;
    l->operation = "request_arm";
    rc = keel_request_arm(l->dev);
    if (rc != KEEL_OK) {
        return rc;
    }
    started = now_us(l);
    while (now_us(l) - started < ARM_WAIT_MS * US_PER_MS) {
        rc = service(l, 1);
        if (rc != KEEL_OK) {
            return rc;
        }
        if (state_of(l) == KEEL_STATE_ARMED) {
            result(l, test, "PASS", "Acknowledgement completed with READY and ARMED feedback; duty zero",
                   KEEL_OK);
            return KEEL_OK;
        }
        rc = wait_slot(l);
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    l->operation = "arm_timeout";
    return KEEL_E_ACK_TIMEOUT;
}

/* ---- modes ---------------------------------------------------------------- */

static int32_t run_io(live *l)
{
    char detail[96];
    int32_t rc = pump(l, 1500, 0);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (state_of(l) != KEEL_STATE_DISARMED) {
        l->operation = "io_state";
        return KEEL_E_STATE;
    }
    result(l, "io_health_loop", "PASS",
           "1500 ms of button reads and foreground ticks; no arming requested", KEEL_OK);
    snprintf(detail, sizeof detail, "buttons held: 0x%" PRIx32, l->buttons.held);
    event("button_snapshot", detail);
    return feedback(l, 1, 0, "io_ready_disarmed");
}

static int32_t run_leds(live *l)
{
    static const char *const colours[] = {"red", "green", "blue"};
    static const char *const names[] = {"A", "B", "X", "Y"};
    uint32_t caps = 0, led, colour, button;
    int32_t rc;
    (void)keel_get_capabilities(l->dev, &caps);
    if (!(caps & KEEL_CAP_RGB)) {
        result(l, "rgb_available", "FAIL",
               "The RGB LED driver is unavailable; the open report says why", KEEL_E_UNSUPPORTED);
        return KEEL_E_UNSUPPORTED;
    }
    l->operation = "rgb_all_off";
    rc = keel_set_rgb_all(l->dev, 0);
    for (led = 0; rc == KEEL_OK && led < KEEL_LED_COUNT; ++led) {
        for (colour = 0; rc == KEEL_OK && colour < 3; ++colour) {
            char name[48], detail[128];
            uint32_t rgb = (uint32_t)LED_LEVEL << (16u - 8u * colour);
            snprintf(name, sizeof name, "RGB%" PRIu32 "_%s", led + 1u, colours[colour]);
            snprintf(detail, sizeof detail, "Observe RGB%" PRIu32 " %s at intensity %d for %" PRIu32
                     " ms", led + 1u, colours[colour], LED_LEVEL, l->hold_ms);
            event("led_observation", detail);
            l->operation = "rgb_write";
            rc = keel_set_rgb(l->dev, led, rgb);
            if (rc == KEEL_OK) {
                rc = pump(l, l->hold_ms, 1);
            }
            if (rc == KEEL_OK) {
                l->operation = "rgb_off";
                rc = keel_set_rgb(l->dev, led, 0);
            }
            if (rc == KEEL_OK) {
                result(l, name, "PASS", "I2C colour, on and off commands completed", KEEL_OK);
                result(l, name, "NEEDS_OBSERVATION",
                       "Confirm the LED's position, colour and brightness", KEEL_OK);
            }
        }
    }
    for (button = 0; rc == KEEL_OK && button < KEEL_BUTTON_COUNT; ++button) {
        char name[48], detail[128];
        snprintf(name, sizeof name, "indicator_%s", names[button]);
        snprintf(detail, sizeof detail, "Observe the button %s indicator for %" PRIu32 " ms",
                 names[button], l->hold_ms);
        event("led_observation", detail);
        l->operation = "indicator_write";
        rc = keel_set_button_leds(l->dev, KEEL_BUTTON_ALL, 1u << button);
        if (rc == KEEL_OK) {
            rc = pump(l, l->hold_ms, 1);
        }
        if (rc == KEEL_OK) {
            result(l, name, "PASS", "Indicator GPIO command completed", KEEL_OK);
            result(l, name, "NEEDS_OBSERVATION", "Confirm the labelled indicator lit", KEEL_OK);
        }
    }
    if (rc != KEEL_OK) {
        return rc;
    }
    l->operation = "indicators_off";
    return keel_set_button_leds(l->dev, KEEL_BUTTON_ALL, 0);
}

/* The simulator's stand-in operator: after a 200 ms baseline, the k-th
 * selected button is held from 300k ms for 150 ms. */
static int32_t simulated_presses(live *l, uint32_t selected, uint64_t elapsed_us)
{
    uint32_t held = 0, bit, k = 0;
    for (bit = 1; bit <= KEEL_BUTTON_Y; bit <<= 1) {
        if (selected & bit) {
            uint64_t from = (200u + 300u * k) * (uint64_t)US_PER_MS;
            if (elapsed_us >= from && elapsed_us < from + SIM_PRESS_MS * US_PER_MS) {
                held |= bit;
            }
            k++;
        }
    }
    return keel_sim_set(l->dev, KEEL_SIM_BUTTONS, (int32_t)held);
}

static int32_t run_buttons(live *l, uint32_t timeout_seconds, uint32_t selected)
{
    static const char *const names[] = {"A", "B", "X", "Y"};
    uint64_t started = now_us(l), released_since[KEEL_BUTTON_COUNT] = {0, 0, 0, 0};
    uint32_t completed = 0, previous_held = UINT32_MAX, index;
    uint8_t phase[KEEL_BUTTON_COUNT] = {0, 0, 0, 0}; /* baseline, armed, pressed, done */
    char wanted[16] = "", detail[160];
    size_t used = 0;
    for (index = 0; index < KEEL_BUTTON_COUNT; ++index) {
        if (selected & (1u << index)) {
            if (used) {
                wanted[used++] = ' ';
            }
            wanted[used++] = names[index][0];
        }
    }
    wanted[used] = '\0';
    snprintf(detail, sizeof detail,
             "Release %s, then press and release each selected button in any order", wanted);
    event("input_required", detail);
    fprintf(stderr, "%s\n", detail);
    while (now_us(l) - started < (uint64_t)timeout_seconds * 1000000u) {
        uint64_t time = now_us(l);
        int32_t rc = l->simulator ? simulated_presses(l, selected, time - started) : KEEL_OK;
        if (rc == KEEL_OK) {
            rc = service(l, 0); /* B is an input here, never an abort */
        }
        if (rc != KEEL_OK) {
            return rc;
        }
        if (l->buttons.pressed || l->buttons.released || l->buttons.held != previous_held) {
            printf("{\"type\":\"event\",\"event\":\"button_edges\",\"elapsed_ms\":%" PRIu64
                   ",\"held\":%" PRIu32 ",\"pressed\":%" PRIu32 ",\"released\":%" PRIu32
                   ",\"selected_mask\":%" PRIu32 ",\"completed_mask\":%" PRIu32 "}\n",
                   (time - l->started_us) / US_PER_MS, l->buttons.held, l->buttons.pressed,
                   l->buttons.released, selected, completed);
            fflush(stdout);
            previous_held = l->buttons.held;
        }
        for (index = 0; index < KEEL_BUTTON_COUNT; ++index) {
            uint32_t bit = 1u << index;
            if (!(selected & bit) || (completed & bit)) {
                continue;
            }
            if (phase[index] == 0) {
                /* Every selected button must be seen released and settled
                 * past the debounce before its press counts. */
                if (l->buttons.held & bit) {
                    released_since[index] = 0;
                } else if (!released_since[index]) {
                    released_since[index] = time;
                } else if (time - released_since[index] >= 30u * US_PER_MS) {
                    phase[index] = 1;
                }
            } else if (phase[index] == 1 && (l->buttons.pressed & bit) && (l->buttons.held & bit)) {
                phase[index] = 2;
                event("button_press_observed", names[index]);
            } else if (phase[index] == 2 && (l->buttons.released & bit) &&
                       !(l->buttons.held & bit)) {
                char test[48];
                phase[index] = 3;
                completed |= bit;
                snprintf(test, sizeof test, "button_%s_press_release", names[index]);
                result(l, test, "PASS",
                       "Observed a debounced press followed by a release on the GPIO input",
                       KEEL_OK);
            }
        }
        if (completed == selected) {
            return KEEL_OK;
        }
        rc = wait_slot(l);
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    for (index = 0; index < KEEL_BUTTON_COUNT; ++index) {
        uint32_t bit = 1u << index;
        if ((selected & bit) && !(completed & bit)) {
            char test[48];
            snprintf(test, sizeof test, "button_%s_press_release", names[index]);
            result(l, test, "FAIL", "Timed out before both debounced transitions were observed",
                   KEEL_E_STATE);
        }
    }
    l->operation = "button_timeout";
    return KEEL_E_STATE;
}

static int32_t run_motors(live *l)
{
    static const struct {
        const char *name;
        int32_t left, right;
    } motions[] = {{"left_forward", MOTOR_DUTY, 0},
                   {"left_reverse", -MOTOR_DUTY, 0},
                   {"right_forward", 0, MOTOR_DUTY},
                   {"right_reverse", 0, -MOTOR_DUTY}};
    size_t i;
    for (i = 0; i < sizeof motions / sizeof motions[0]; ++i) {
        char test[64], detail[160];
        uint64_t end;
        int32_t rc, stop_rc;
        snprintf(test, sizeof test, "arm_%s", motions[i].name);
        rc = arm_zero(l, test);
        if (rc != KEEL_OK) {
            return rc;
        }
        snprintf(detail, sizeof detail, "%s: one wheel, %d permille, %d ms; B or Ctrl-C stops",
                 motions[i].name, MOTOR_DUTY, MOTOR_RUN_MS);
        event("motor_observation", detail);
        /* A loop re-sends its command every cycle. */
        end = now_us(l) + MOTOR_RUN_MS * US_PER_MS;
        do {
            rc = service(l, 1);
            if (rc == KEEL_OK) {
                l->operation = "motor_command";
                rc = keel_set_motors(l->dev, motions[i].left, motions[i].right);
            }
            if (rc == KEEL_OK) {
                rc = wait_slot(l);
            }
        } while (rc == KEEL_OK && now_us(l) < end);
        /* Always remove permission right after the bounded phase. */
        stop_rc = keel_disarm(l->dev);
        if (rc != KEEL_OK) {
            return rc;
        }
        if (stop_rc != KEEL_OK) {
            l->operation = "motor_stop";
            return stop_rc;
        }
        snprintf(test, sizeof test, "motor_%s_stop_feedback", motions[i].name);
        rc = wait_stopped(l, test, 1);
        if (rc != KEEL_OK) {
            return rc;
        }
        snprintf(test, sizeof test, "motor_%s_command", motions[i].name);
        result(l, test, "PASS",
               "Drive interval and stop completed with a healthy loop; no encoder feedback",
               KEEL_OK);
        result(l, motions[i].name, "NEEDS_OBSERVATION",
               "Confirm only the intended wheel moved, in the right direction, and stopped; 20% "
               "duty may not overcome friction",
               KEEL_OK);
        rc = pump(l, 300, 1);
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    return KEEL_OK;
}

/* Records PASS when a call returned @p expected and left the device in
 * @p state, FAIL otherwise. */
static int32_t expect(live *l, const char *test, int32_t rc, int32_t expected, uint32_t state,
                      const char *detail)
{
    if (rc != expected || state_of(l) != state) {
        char text[192];
        snprintf(text, sizeof text, "%s: returned %s in state %s", detail, keel_status_name(rc),
                 tool_state_name(state_of(l)));
        result(l, test, "FAIL", text, rc);
        l->operation = test;
        return rc != KEEL_OK ? rc : KEEL_E_STATE;
    }
    result(l, test, "PASS", detail, rc);
    return KEEL_OK;
}

static int32_t run_safety(live *l)
{
    keel_fault fault;
    keel_snapshot snapshot;
    int32_t rc = arm_zero(l, "zero_duty_ack");
    if (rc == KEEL_OK) {
        rc = feedback(l, 1, 1, "armed_feedback");
    }
    if (rc == KEEL_OK) {
        l->operation = "zero_duty_stop";
        rc = expect(l, "stop_host_state", keel_disarm(l->dev), KEEL_OK, KEEL_STATE_DISARMED,
                    "keel_disarm returned the device to DISARMED; no drive was requested");
    }
    if (rc == KEEL_OK) {
        rc = wait_stopped(l, "stop_hardware_feedback", 1);
    }
    if (rc == KEEL_OK) {
        rc = pump(l, 300, 1);
    }
    if (rc == KEEL_OK) {
        rc = arm_zero(l, "zero_duty_rearm");
    }
    if (rc != KEEL_OK) {
        return rc;
    }
    event("heartbeat_gap", "Withholding the tick for 120 ms while both duties stay zero");
    rc = pause_ms(l, HEARTBEAT_GAP_MS);
    /* Read the pins before the library judges the gap, so that its own
     * safe stop cannot pass for the MCU's. */
    if (rc == KEEL_OK) {
        rc = feedback(l, 0, 0, "mcu_timeout_feedback_after_120ms");
    }
    if (rc != KEEL_OK) {
        return rc;
    }
    l->operation = "expired_health_rejection";
    rc = keel_tick(l->dev, ++l->cycle, KEEL_HEALTH_ALL);
    tool_get_fault(l->dev, &fault);
    if (rc != KEEL_E_DEADLINE || fault.cause != KEEL_CAUSE_TICK_LATE ||
        state_of(l) != KEEL_STATE_FAULTED) {
        result(l, "expired_health_rejection", "FAIL",
               "The late tick was not refused with KEEL_E_DEADLINE / TICK_LATE", rc);
        return rc != KEEL_OK ? rc : KEEL_E_STATE;
    }
    result(l, "expired_health_rejection", "PASS",
           "The tick after 120 ms without one was refused (TICK_LATE) and the device FAULTED", rc);
    event("timing_limit", "Feedback was sampled after the gap; this does not measure the cut-off "
                          "waveform or certify a timeout bound");

    /* Recovery: never implicit, with a hold-off, then a deliberate stop. */
    rc = expect(l, "rearm_refused_while_faulted", keel_request_arm(l->dev), KEEL_E_FAULTED,
                KEEL_STATE_FAULTED, "keel_request_arm in FAULTED changed nothing");
    if (rc == KEEL_OK) {
        rc = expect(l, "clear_fault", keel_clear_fault(l->dev), KEEL_OK, KEEL_STATE_DISARMED,
                    "keel_clear_fault returned the device to DISARMED");
    }
    if (rc == KEEL_OK) {
        memset(&snapshot, 0, sizeof snapshot);
        snapshot.struct_size = sizeof snapshot;
        rc = expect(l, "rearm_holdoff", keel_request_arm(l->dev), KEEL_E_STATE,
                    KEEL_STATE_DISARMED, "keel_request_arm within 1 s of the latch was held off");
        if (rc == KEEL_OK && (keel_get_snapshot(l->dev, &snapshot) != KEEL_OK ||
                              !(snapshot.flags & KEEL_SNAP_REARM_HOLDOFF))) {
            result(l, "rearm_holdoff_flag", "FAIL", "KEEL_SNAP_REARM_HOLDOFF was not set",
                   KEEL_E_STATE);
            rc = KEEL_E_STATE;
        }
    }
    /* The MCU latched the heartbeat loss; two falls and quiet re-qualify it. */
    if (rc == KEEL_OK) {
        rc = pump(l, 1100, 1);
    }
    if (rc == KEEL_OK) {
        rc = arm_zero(l, "rearm_after_clear");
    }
    if (rc == KEEL_OK) {
        l->operation = "emergency_stop";
        rc = expect(l, "emergency_stop_latch", keel_emergency_stop(l->dev), KEEL_OK,
                    KEEL_STATE_FAULTED, "keel_emergency_stop latched FAULTED");
    }
    if (rc == KEEL_OK) {
        tool_get_fault(l->dev, &fault);
        if (fault.cause != KEEL_CAUSE_USER_STOP) {
            result(l, "emergency_stop_cause", "FAIL", "The record does not say USER_STOP",
                   fault.status);
            return KEEL_E_STATE;
        }
        rc = wait_stopped(l, "emergency_stop_feedback", 0);
    }
    if (rc == KEEL_OK) {
        rc = expect(l, "clear_after_emergency_stop", keel_clear_fault(l->dev), KEEL_OK,
                    KEEL_STATE_DISARMED,
                    "keel_clear_fault returned the device to DISARMED after the user stop");
    }
    return rc == KEEL_OK ? pump(l, 300, 1) : rc;
}

static const char *range_status_name(uint32_t status)
{
    static const char *const names[] = {"IDLE", "PENDING", "VALID", "NO_ECHO", "UNAVAILABLE",
                                        "INVALID"};
    return status < sizeof names / sizeof names[0] ? names[status] : "UNKNOWN";
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Sensor power needs a qualified heartbeat and READY, not arming. */
static int32_t wait_sensor_power(live *l)
{
    keel_snapshot snapshot;
    uint64_t started = now_us(l);
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.struct_size = sizeof snapshot;
    while (now_us(l) - started < 1500u * US_PER_MS) {
        int32_t rc = service(l, 1);
        if (rc == KEEL_OK) {
            rc = keel_get_snapshot(l->dev, &snapshot);
        }
        if (rc != KEEL_OK) {
            return rc;
        }
        if (snapshot.sensor_power == KEEL_SENSOR_PERMITTED ||
            snapshot.sensor_power == KEEL_SENSOR_CONFIRMED) {
            result(l, "range_sensor_power", "PASS",
                   "Heartbeat qualified and READY HIGH: the MCU powers the sensor", KEEL_OK);
            return KEEL_OK;
        }
        rc = wait_slot(l);
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    result(l, "range_sensor_power", "FAIL",
           "Sensor power was not permitted within 1.5 s (READY LOW or heartbeat not qualified)",
           KEEL_E_PERIPHERAL);
    return KEEL_E_PERIPHERAL;
}

static int32_t run_range(live *l)
{
    uint32_t caps = 0, counts[6] = {0, 0, 0, 0, 0, 0}, distances[RANGE_READINGS];
    uint32_t taken = 0, valid = 0;
    uint64_t started;
    char detail[192];
    int pending = 0;
    int32_t rc;
    (void)keel_get_capabilities(l->dev, &caps);
    if (!(caps & KEEL_CAP_RANGE)) {
        result(l, "range_available", "FAIL", "Ranging is unavailable on this device",
               KEEL_E_UNSUPPORTED);
        return KEEL_E_UNSUPPORTED;
    }
    if (l->simulator) {
        (void)keel_sim_set(l->dev, KEEL_SIM_RANGE_MM, RANGE_DISTANCE_MM);
    }
    rc = wait_sensor_power(l);
    if (rc != KEEL_OK) {
        return rc;
    }
    event("range_observation", "Hold a flat object 20-200 cm in front of the sensor");
    started = now_us(l);
    while (taken < RANGE_READINGS && now_us(l) - started < 5000u * US_PER_MS) {
        rc = service(l, 1);
        if (rc == KEEL_OK && !pending) {
            l->operation = "range_start";
            rc = keel_range_start(l->dev);
            pending = rc == KEEL_OK;
            if (rc == KEEL_E_AGAIN) {
                rc = KEEL_OK; /* trigger spacing: try on a later cycle */
            }
        } else if (rc == KEEL_OK) {
            keel_range r;
            memset(&r, 0, sizeof r);
            r.struct_size = sizeof r;
            l->operation = "range_poll";
            rc = keel_range_poll(l->dev, &r);
            if (rc == KEEL_OK && r.status != KEEL_RANGE_PENDING) {
                pending = 0;
                counts[r.status < 6 ? r.status : 0]++;
                if (r.status == KEEL_RANGE_VALID) {
                    distances[valid++] = r.distance_mm;
                }
                taken++;
                printf("{\"type\":\"event\",\"event\":\"range\",\"status\":\"%s\",\"distance_mm\":"
                       "%" PRIu32 ",\"echo_us\":%" PRIu32 ",\"sequence\":%" PRIu32 "}\n",
                       range_status_name(r.status), r.distance_mm, r.echo_us, r.sequence);
                fflush(stdout);
            }
        }
        if (rc == KEEL_OK) {
            rc = wait_slot(l);
        }
        if (rc != KEEL_OK) {
            return rc;
        }
    }
    snprintf(detail, sizeof detail,
             "%" PRIu32 " readings: %" PRIu32 " VALID, %" PRIu32 " NO_ECHO, %" PRIu32
             " UNAVAILABLE, %" PRIu32 " INVALID",
             taken, counts[KEEL_RANGE_VALID], counts[KEEL_RANGE_NO_ECHO],
             counts[KEEL_RANGE_UNAVAILABLE], counts[KEEL_RANGE_INVALID]);
    if (taken < RANGE_READINGS || counts[KEEL_RANGE_UNAVAILABLE] || counts[KEEL_RANGE_INVALID]) {
        result(l, "range_measurements", "FAIL", detail, KEEL_E_PERIPHERAL);
        return KEEL_E_PERIPHERAL;
    }
    result(l, "range_measurements", "PASS", detail, KEEL_OK);
    if (valid) {
        qsort(distances, valid, sizeof distances[0], compare_u32);
        snprintf(detail, sizeof detail,
                 "Median %" PRIu32 " mm over %" PRIu32 " valid readings: compare with a tape "
                 "measure",
                 distances[valid / 2], valid);
        result(l, "range_distance", "NEEDS_OBSERVATION", detail, KEEL_OK);
    }
    return KEEL_OK;
}

/* ---- PWM timing probe ----------------------------------------------------- */

/* The worker's wake pattern at 30 % duty: an edge at the period start and
 * one 3 ms later, absolute CLOCK_MONOTONIC deadlines on a condition
 * variable, SCHED_FIFO 40 when allowed. It drives no line: the platform's
 * scheduling latency is what is measured. */
enum { PROBE_EDGE_US = 3000, PROBE_PERIOD_US = 10000 };

typedef struct probe_sample {
    uint64_t at_us;
    uint32_t lateness_us;
} probe_sample;

typedef struct probe {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int stop;
    probe_sample *samples;
    size_t capacity, count;
    uint32_t max_us;
} probe;

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* The first edge of the pattern after @p now, like the worker recomputing
 * its schedule from the phase after a late wake. */
static uint64_t probe_next(uint64_t origin, uint64_t now)
{
    uint64_t phase = (now - origin) % PROBE_PERIOD_US;
    uint64_t start = now - phase;
    return phase < PROBE_EDGE_US ? start + PROBE_EDGE_US : start + PROBE_PERIOD_US;
}

static void *probe_main(void *argument)
{
    probe *p = argument;
    uint64_t origin = monotonic_us(), deadline = origin + PROBE_PERIOD_US;
    pthread_mutex_lock(&p->lock);
    while (!p->stop && p->count < p->capacity) {
        struct timespec until;
        uint64_t now, late;
        until.tv_sec = (time_t)(deadline / 1000000u);
        until.tv_nsec = (long)(deadline % 1000000u) * 1000L;
        while (!p->stop && pthread_cond_timedwait(&p->cond, &p->lock, &until) != ETIMEDOUT) {
        }
        now = monotonic_us();
        late = now > deadline ? now - deadline : 0u;
        p->samples[p->count].at_us = now;
        p->samples[p->count].lateness_us = late > UINT32_MAX ? UINT32_MAX : (uint32_t)late;
        if (p->samples[p->count].lateness_us > p->max_us) {
            p->max_us = p->samples[p->count].lateness_us;
        }
        p->count++;
        deadline = probe_next(origin, now);
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

/* As the library starts its worker: SCHED_FIFO 40, SCHED_OTHER on EPERM,
 * async signals blocked so that Ctrl-C reaches the main loop. */
static int probe_start(probe *p, pthread_t *thread, int *realtime)
{
    pthread_attr_t attributes;
    struct sched_param param;
    sigset_t all, previous;
    int error, attempt;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    for (attempt = 1; attempt >= 0; --attempt) {
        pthread_attr_init(&attributes);
        memset(&param, 0, sizeof param);
        param.sched_priority = attempt ? KEEL_RT_PRIORITY : 0;
        pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attributes, attempt ? SCHED_FIFO : SCHED_OTHER);
        pthread_attr_setschedparam(&attributes, &param);
        error = pthread_create(thread, &attributes, probe_main, p);
        pthread_attr_destroy(&attributes);
        if (error != EPERM) {
            *realtime = attempt && error == 0;
            break;
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return error;
}

/* Replays the library's latch rules (SPEC 17.4) over the samples: one wake
 * over 30 ms, or over 20 ms of lateness beyond 1 ms within 100 ms. */
static void probe_latches(const probe *p, unsigned *overruns, unsigned *degraded)
{
    uint64_t sum = 0;
    size_t first = 0, i;
    *overruns = 0;
    *degraded = 0;
    for (i = 0; i < p->count; ++i) {
        const probe_sample *s = &p->samples[i];
        if (s->lateness_us > 30000u) {
            (*overruns)++;
        }
        sum += s->lateness_us > 1000u ? s->lateness_us - 1000u : 0u;
        while (s->at_us - p->samples[first].at_us >= 100000u) {
            const probe_sample *old = &p->samples[first++];
            sum -= old->lateness_us > 1000u ? old->lateness_us - 1000u : 0u;
        }
        if (sum > 20000u) {
            (*degraded)++;
            sum = 0; /* the library would have latched and started over */
            first = i + 1;
        }
    }
}

static int compare_sample(const void *a, const void *b)
{
    uint32_t x = ((const probe_sample *)a)->lateness_us, y = ((const probe_sample *)b)->lateness_us;
    return (x > y) - (x < y);
}

static int32_t run_pwm_jitter(live *l, uint32_t seconds)
{
    probe p;
    pthread_t thread;
    uint64_t started, report_at;
    unsigned overruns, degraded, over_1ms = 0, over_5ms = 0;
    size_t i;
    char detail[256];
    int realtime = 0, error;
    int32_t rc = KEEL_OK;

    memset(&p, 0, sizeof p);
    p.capacity = (size_t)seconds * 2u * (1000000u / PROBE_PERIOD_US) + 64u;
    p.samples = calloc(p.capacity, sizeof p.samples[0]);
    if (!p.samples || pthread_mutex_init(&p.lock, NULL) != 0) {
        free(p.samples);
        result(l, "pwm_jitter_probe", "FAIL", "Out of memory for the samples", KEEL_E_NO_MEMORY);
        return KEEL_E_NO_MEMORY;
    }
    {
        pthread_condattr_t attributes;
        pthread_condattr_init(&attributes);
        pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
        pthread_cond_init(&p.cond, &attributes);
        pthread_condattr_destroy(&attributes);
    }
    error = probe_start(&p, &thread, &realtime);
    if (error) {
        snprintf(detail, sizeof detail, "Could not start the probe thread: %s", strerror(error));
        result(l, "pwm_jitter_probe", "FAIL", detail, KEEL_E_SETUP_IO);
        rc = KEEL_E_SETUP_IO;
    } else {
        snprintf(detail, sizeof detail,
                 "Measuring the PWM worker's wake pattern for %" PRIu32 " s at %s; the device "
                 "stays DISARMED with its heartbeat running",
                 seconds, realtime ? "SCHED_FIFO 40" : "SCHED_OTHER (real-time refused)");
        event("pwm_jitter_start", detail);
        started = now_us(l);
        report_at = started + 10000000u;
        while (rc == KEEL_OK && now_us(l) - started < (uint64_t)seconds * 1000000u) {
            rc = pump(l, 100, 1);
            if (rc == KEEL_OK && now_us(l) >= report_at) {
                pthread_mutex_lock(&p.lock);
                printf("{\"type\":\"event\",\"event\":\"pwm_jitter_progress\",\"elapsed_s\":%" PRIu64
                       ",\"wakes\":%zu,\"max_lateness_us\":%" PRIu32 "}\n",
                       (now_us(l) - started) / 1000000u, p.count, p.max_us);
                pthread_mutex_unlock(&p.lock);
                fflush(stdout);
                report_at += 10000000u;
            }
        }
        pthread_mutex_lock(&p.lock);
        p.stop = 1;
        pthread_cond_signal(&p.cond);
        pthread_mutex_unlock(&p.lock);
        pthread_join(thread, NULL);
    }
    if (rc == KEEL_OK && p.count) {
        probe_latches(&p, &overruns, &degraded);
        for (i = 0; i < p.count; ++i) {
            over_1ms += p.samples[i].lateness_us > 1000u;
            over_5ms += p.samples[i].lateness_us > 5000u;
        }
        qsort(p.samples, p.count, sizeof p.samples[0], compare_sample);
        printf("{\"type\":\"event\",\"event\":\"pwm_jitter_summary\",\"timing\":\"%s\","
               "\"wakes\":%zu,\"p50_us\":%" PRIu32 ",\"p99_us\":%" PRIu32 ",\"p99_9_us\":%" PRIu32
               ",\"max_us\":%" PRIu32 ",\"over_1ms\":%u,\"over_5ms\":%u,\"would_overrun\":%u,"
               "\"would_degrade\":%u}\n",
               realtime ? "realtime" : "best_effort", p.count,
               p.samples[p.count / 2].lateness_us, p.samples[p.count * 99 / 100].lateness_us,
               p.samples[p.count * 999 / 1000].lateness_us, p.samples[p.count - 1].lateness_us,
               over_1ms, over_5ms, overruns, degraded);
        fflush(stdout);
        result(l, "pwm_jitter_realtime", realtime ? "PASS" : "FAIL",
               realtime ? "The probe ran at SCHED_FIFO 40, as the worker will"
                        : "SCHED_FIFO was refused: allow rtprio 40 (sudo keel-setup --user NAME)",
               realtime ? KEEL_OK : KEEL_E_PERMISSION);
        snprintf(detail, sizeof detail,
                 "%u wakes would latch PWM_OVERRUN and %u PWM_DEGRADED (target: none)", overruns,
                 degraded);
        result(l, "pwm_jitter_latches", overruns || degraded ? "FAIL" : "PASS", detail,
               overruns || degraded ? KEEL_E_PWM : KEEL_OK);
        snprintf(detail, sizeof detail, "p99.9 lateness %" PRIu32 " us (target: under 1000 us)",
                 p.samples[p.count * 999 / 1000].lateness_us);
        result(l, "pwm_jitter_p99_9", p.samples[p.count * 999 / 1000].lateness_us < 1000u
                                          ? "PASS"
                                          : "FAIL",
               detail, KEEL_OK);
    }
    if (!error) {
        pthread_cond_destroy(&p.cond);
    }
    pthread_mutex_destroy(&p.lock);
    free(p.samples);
    return rc;
}

/* SPEC 24.2 item 1: the process dies mid-drive; the operator records how
 * fast ARMED falls and the released line levels. Never returns on success. */
static int32_t run_kill_test(live *l)
{
    uint64_t end;
    int32_t rc = arm_zero(l, "kill_test_arm");
    if (rc != KEEL_OK) {
        return rc;
    }
    event("kill_test", "Driving both wheels at 200 permille, SIGKILL in 300 ms: record how long "
                       "ARMED takes to fall (design bound about 86 ms after the last heartbeat "
                       "fall) and the released levels of BCM8-11, 18 and 26");
    end = now_us(l) + 300u * US_PER_MS;
    while (rc == KEEL_OK && now_us(l) < end) {
        rc = service(l, 1);
        if (rc == KEEL_OK) {
            rc = keel_set_motors(l->dev, MOTOR_DUTY, MOTOR_DUTY);
        }
        if (rc == KEEL_OK) {
            rc = wait_slot(l);
        }
    }
    if (rc != KEEL_OK) {
        return rc;
    }
    fflush(stdout);
    raise(SIGKILL);
    return KEEL_E_INTERNAL;
}

/* ---- main ----------------------------------------------------------------- */

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "Usage: %s [MODE] --programmed [OPTIONS]\n"
            "Modes (one; default --io):\n"
            "  --io                heartbeat and button reads for 1.5 s, no arming\n"
            "  --leds              walk every RGB colour and button indicator\n"
            "  --buttons           press and release each selected button\n"
            "  --safety            arm, stop, heartbeat-loss, recovery and emergency stop\n"
            "  --motors            four short single-wheel drives (needs --wheels-raised)\n"
            "  --range             20 ultrasonic measurements\n"
            "  --pwm-jitter SECONDS  measure PWM worker wake lateness (1..3600)\n"
            "  --kill-test         drive, then SIGKILL itself (needs --wheels-raised)\n"
            "Options:\n"
            "  --programmed        confirm the MCU runs the verified safety firmware\n"
            "  --wheels-raised     confirm the wheels are off the ground\n"
            "  --simulator         run against the simulator instead of hardware\n"
            "  --gpio-chip PATH    GPIO chip (default: found by label)\n"
            "  --i2c PATH          I2C adapter (default /dev/i2c-1)\n"
            "  --timeout SECONDS   --buttons time limit (1..300, default 120)\n"
            "  --button-mask MASK  --buttons selection (1..15, default 15; A=1 B=2 X=4 Y=8)\n"
            "  --hold MS           --leds observation time (50..5000, default 1000)\n"
            "JSON Lines go to stdout. Button B or Ctrl-C stops the run with an emergency stop.\n",
            program);
}

typedef struct arguments {
    enum mode mode;
    int have_mode, programmed, wheels_raised, simulator;
    const char *gpiochip, *i2c;
    int32_t timeout_seconds, button_mask, hold_ms, jitter_seconds;
    int have_timeout, have_mask, have_hold;
} arguments;

/* Returns -1 to continue, otherwise the exit status. */
static int parse_arguments(int argc, char **argv, arguments *a)
{
    int i, m;
    memset(a, 0, sizeof *a);
    a->timeout_seconds = 120;
    a->button_mask = KEEL_BUTTON_ALL;
    a->hold_ms = 1000;
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        int selected = -1;
        for (m = 0; m < MODE_COUNT; ++m) {
            if (strncmp(arg, "--", 2) == 0 && strcmp(arg + 2, mode_names[m]) == 0) {
                selected = m;
            }
        }
        if (selected >= 0) {
            if (a->have_mode) {
                usage(stderr, argv[0]);
                return 2;
            }
            a->mode = (enum mode)selected;
            a->have_mode = 1;
            if (a->mode == MODE_PWM_JITTER &&
                (i + 1 >= argc || !tool_parse_int(argv[++i], 1, 3600, &a->jitter_seconds))) {
                usage(stderr, argv[0]);
                return 2;
            }
        } else if (strcmp(arg, "--programmed") == 0) {
            a->programmed = 1;
        } else if (strcmp(arg, "--wheels-raised") == 0) {
            a->wheels_raised = 1;
        } else if (strcmp(arg, "--simulator") == 0) {
            a->simulator = 1;
        } else if (strcmp(arg, "--gpio-chip") == 0 && i + 1 < argc) {
            a->gpiochip = argv[++i];
        } else if (strcmp(arg, "--i2c") == 0 && i + 1 < argc) {
            a->i2c = argv[++i];
        } else if (strcmp(arg, "--timeout") == 0 && i + 1 < argc &&
                   tool_parse_int(argv[i + 1], 1, 300, &a->timeout_seconds)) {
            a->have_timeout = 1;
            ++i;
        } else if (strcmp(arg, "--button-mask") == 0 && i + 1 < argc &&
                   tool_parse_int(argv[i + 1], 1, KEEL_BUTTON_ALL, &a->button_mask)) {
            a->have_mask = 1;
            ++i;
        } else if (strcmp(arg, "--hold") == 0 && i + 1 < argc &&
                   tool_parse_int(argv[i + 1], 50, 5000, &a->hold_ms)) {
            a->have_hold = 1;
            ++i;
        } else if (strcmp(arg, "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (((a->have_timeout || a->have_mask) && a->mode != MODE_BUTTONS) ||
        (a->have_hold && a->mode != MODE_LEDS) ||
        (a->simulator && (a->gpiochip || a->i2c || a->mode == MODE_KILL_TEST))) {
        usage(stderr, argv[0]);
        return 2;
    }
    return -1;
}

static int32_t run_mode(live *l, const arguments *a)
{
    switch (a->mode) {
    case MODE_IO:
        return run_io(l);
    case MODE_LEDS:
        return run_leds(l);
    case MODE_BUTTONS:
        return run_buttons(l, (uint32_t)a->timeout_seconds, (uint32_t)a->button_mask);
    case MODE_SAFETY:
        return run_safety(l);
    case MODE_MOTORS:
        return run_motors(l);
    case MODE_RANGE:
        return run_range(l);
    case MODE_PWM_JITTER:
        return run_pwm_jitter(l, (uint32_t)a->jitter_seconds);
    default:
        return run_kill_test(l);
    }
}

static void open_details(live *l, const keel_open_report *report)
{
    char detail[384];
    snprintf(detail, sizeof detail,
             "Opened %s: outputs LOW, capabilities 0x%" PRIx32 ", PWM timing %s",
             l->simulator ? "the simulator" : "the keel_r3 backend", report->capabilities,
             report->timing_class == KEEL_TIMING_REALTIME
                 ? "real-time"
                 : report->timing_class == KEEL_TIMING_BEST_EFFORT ? "best effort" : "none");
    result(l, "hardware_open", "PASS", detail, KEEL_OK);
    if (report->warnings) {
        snprintf(detail, sizeof detail, "Opened degraded (warnings 0x%" PRIx32 "): %s",
                 report->warnings, report->hint);
        event("open_warning", detail);
    }
}

int main(int argc, char **argv)
{
    arguments a;
    live l;
    keel_open_options options;
    keel_open_report report;
    keel_fault fault;
    int32_t code;
    int exit_code = parse_arguments(argc, argv, &a);
    if (exit_code >= 0) {
        return exit_code;
    }
    exit_code = 0;
    memset(&l, 0, sizeof l);
    l.simulator = a.simulator;
    l.hold_ms = (uint32_t)a.hold_ms;
    l.operation = "initialize";
    if (tool_install_signals() != 0) {
        result(&l, "signal_handlers", "FAIL", "Could not install the stop signal handlers",
               KEEL_E_SETUP_IO);
        return 1;
    }
    if ((keel_abi_version() >> 16) != (KEEL_ABI_VERSION >> 16)) {
        result(&l, "abi_version", "FAIL", "The loaded library's ABI major differs from this tool's",
               KEEL_E_UNSUPPORTED);
        return 1;
    }
    result(&l, "abi_version", "PASS", keel_version_string(), KEEL_OK);
    if (!a.simulator && (!a.programmed || ((a.mode == MODE_MOTORS || a.mode == MODE_KILL_TEST) &&
                                           !a.wheels_raised))) {
        result(&l, "hardware_preconditions", "SKIP",
               !a.programmed ? "BLOCKED: --programmed confirms the MCU runs the verified safety "
                               "firmware; no GPIO or I2C access was made"
                             : "BLOCKED: this mode needs --wheels-raised; no GPIO or I2C access "
                               "was made",
               KEEL_E_STATE);
        return 77;
    }
    event("caller_preconditions",
          a.simulator ? "Simulator: no hardware is touched"
                      : "The caller confirms programmed MCU firmware (and raised wheels where "
                        "needed); these flags do not probe or program anything");
    options = tool_options(a.simulator, a.gpiochip, a.i2c);
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    code = keel_open(&options, &l.dev, &report);
    if (code != KEEL_OK) {
        char detail[384];
        snprintf(detail, sizeof detail, "%s: %s", keel_status_name(code), report.hint);
        result(&l, "hardware_open", "SKIP", detail, code);
        event("blocked", "The robot's lines or devices are unavailable; nothing was simulated");
        return 77;
    }
    open_details(&l, &report);
    l.started_us = now_us(&l);
    l.next_us = l.started_us;
    event("mode", mode_names[a.mode]);
    code = run_mode(&l, &a);

    if (tool_signal || l.button_abort) {
        /* An operator stop is an emergency stop, then a normal close. */
        (void)keel_emergency_stop(l.dev);
        event("aborted", tool_signal ? "A signal stopped the run" : "Button B stopped the run");
        exit_code = tool_signal ? 128 + tool_signal : 130;
    } else if (code != KEEL_OK) {
        if (!l.failed) {
            result(&l, l.operation, "FAIL", keel_status_string(code), code);
        }
        fault_event(&l);
        exit_code = 1;
    } else if (l.failed) {
        exit_code = 1; /* the mode ran to its end, but a result failed */
    }
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    code = keel_close(l.dev, &fault);
    if (code != KEEL_OK) {
        result(&l, "hardware_close", "FAIL",
               "The final stop was not confirmed; the MCU heartbeat timeout remains the backstop",
               code);
        exit_code = 1;
    } else {
        result(&l, "hardware_close", "PASS",
               "Stopped, LEDs off, PWM worker joined and every line released", KEEL_OK);
    }
    printf("{\"type\":\"summary\",\"mode\":\"%s\",\"passed\":%u,\"failed\":%u,"
           "\"needs_observation\":%u,\"exit_code\":%d}\n",
           mode_names[a.mode], l.passed, l.failed, l.observations, exit_code);
    return exit_code;
}
