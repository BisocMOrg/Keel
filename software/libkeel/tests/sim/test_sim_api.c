/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * The simulator through the public API only (keel.h): the end-to-end loop
 * on both clocks, the PWM worker regressions, LEDs and buttons, the struct
 * versioning rules, thread ownership, the latching stop and the metadata
 * tables.
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

#include "check.h"
#include "keel_build_config.h"

/* A control loop as bindings run it: cycle, then sleep on the device clock. */
typedef struct loop {
    keel_device *device;
    uint64_t next_us;
    uint64_t cycle_id;
} loop;

static keel_device *open_sim(uint32_t flags, keel_open_report *report)
{
    keel_open_options options;
    keel_device *device = NULL;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    options.flags = flags;
    CHECK_RC(keel_open(&options, &device, report), KEEL_OK);
    return device;
}

static void loop_start(loop *l, keel_device *device)
{
    l->device = device;
    l->cycle_id = 0;
    CHECK_RC(keel_now_us(device, &l->next_us), KEEL_OK);
}

static int32_t step_flags(loop *l, uint32_t command, int32_t left, int32_t right, uint32_t flags,
                          keel_snapshot *out)
{
    keel_cycle_input in;
    int32_t rc;
    memset(&in, 0, sizeof in);
    in.struct_size = sizeof in;
    in.health = KEEL_HEALTH_ALL;
    in.cycle_id = ++l->cycle_id;
    in.command = command;
    in.left = left;
    in.right = right;
    in.flags = flags;
    if (out) {
        memset(out, 0, sizeof *out);
        out->struct_size = sizeof *out;
    }
    rc = keel_cycle(l->device, &in, out);
    l->next_us += 10000;
    CHECK_RC(keel_sleep_until(l->device, l->next_us), KEEL_OK);
    return rc;
}

static int32_t step(loop *l, uint32_t command, int32_t left, int32_t right, keel_snapshot *out)
{
    return step_flags(l, command, left, right, 0, out);
}

static void idle(loop *l, unsigned steps)
{
    unsigned i;
    for (i = 0; i < steps; ++i) {
        CHECK_RC(step(l, KEEL_CYCLE_NONE, 0, 0, NULL), KEEL_OK);
    }
}

static uint32_t state_of(keel_device *device)
{
    uint32_t state = UINT32_MAX;
    CHECK_RC(keel_get_state(device, &state), KEEL_OK);
    return state;
}

static keel_snapshot snapshot_of(keel_device *device)
{
    keel_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.struct_size = sizeof snapshot;
    CHECK_RC(keel_get_snapshot(device, &snapshot), KEEL_OK);
    return snapshot;
}

static keel_fault fault_of(keel_device *device)
{
    keel_fault fault;
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    CHECK_RC(keel_get_fault(device, &fault), KEEL_OK);
    return fault;
}

static keel_sim_state sim_of(keel_device *device)
{
    keel_sim_state state;
    memset(&state, 0, sizeof state);
    state.struct_size = sizeof state;
    CHECK_RC(keel_sim_get_state(device, &state), KEEL_OK);
    return state;
}

static void arm(loop *l)
{
    unsigned n;
    CHECK_RC(keel_request_arm(l->device), KEEL_OK);
    for (n = 0; n < 200 && state_of(l->device) != KEEL_STATE_ARMED; ++n) {
        CHECK_RC(step(l, KEEL_CYCLE_NONE, 0, 0, NULL), KEEL_OK);
    }
    CHECK(state_of(l->device) == KEEL_STATE_ARMED);
}

static void close_clean(keel_device *device)
{
    keel_fault fault;
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    CHECK_RC(keel_close(device, &fault), KEEL_OK);
    CHECK_RC(fault.cleanup_status, KEEL_OK);
}

enum { PIN_ENABLE = 1u << 26, PIN_BUTTON_A = 1u << 5, PIN_LED_B = 1u << 22 };

/* An R3 open reaches the keel_r3 backend whenever it is built in: off the
 * robot it fails inside the backend (the GPIO chip), on the robot it may
 * open. Without the backend it fails before it (resource BACKEND). */
static void expect_r3_reached(int32_t rc, keel_device *device, const keel_open_report *report)
{
#if KEEL_HAVE_BACKEND_R3
    if (rc == KEEL_OK) {
        CHECK_RC(keel_close(device, NULL), KEEL_OK);
        return;
    }
    CHECK(report->resource != KEEL_RESOURCE_BACKEND);
#else
    CHECK_RC(rc, KEEL_E_NOT_FOUND);
    CHECK_EQ(report->resource, KEEL_RESOURCE_BACKEND);
#endif
    CHECK(device == NULL);
    CHECK_EQ(keel_status_class(rc), KEEL_CLASS_SETUP);
    CHECK_RC(report->status, rc);
    CHECK_EQ(report->backend, KEEL_BACKEND_R3);
}

/* open, request_arm, the tick loop, set_motors and close, end to end. */
static void run_end_to_end(uint32_t flags)
{
    keel_open_report report;
    keel_snapshot out;
    keel_sim_state sim;
    keel_fault fault;
    loop l;
    unsigned n;
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    loop_start(&l, open_sim(flags, &report));
    CHECK_RC(report.status, KEEL_OK);
    CHECK_EQ(report.backend, KEEL_BACKEND_SIMULATOR);
    CHECK_EQ(report.warnings, 0);
    CHECK_EQ(report.timing_class, KEEL_TIMING_NONE);
    CHECK((report.capabilities & KEEL_CAP_SIMULATED) && (report.capabilities & KEEL_CAP_MOTORS));
    CHECK(snapshot_of(l.device).flags & KEEL_SNAP_SIMULATED);
    arm(&l);
    CHECK(sim_of(l.device).flags & KEEL_SIM_MOTOR_PERMIT);
    for (n = 0; n < 30; ++n) {
        CHECK_RC(step(&l, KEEL_CYCLE_DRIVE, 400, -300, &out), KEEL_OK);
    }
    CHECK(out.output_left == 400 && out.output_right == -300);
    CHECK(out.flags & KEEL_SNAP_HEARTBEAT_RUNNING);
    sim = sim_of(l.device);
    CHECK(sim.bridge_left == 400 && sim.bridge_right == -300);
    CHECK((sim.pins & PIN_ENABLE) && (sim.flags & KEEL_SIM_READY));
    CHECK_EQ(sim.mcu_faults, 0);
    /* Stop re-sending: the deadman coasts after 500 ms; still ARMED, no fault. */
    for (n = 0; n < 55; ++n) {
        CHECK_RC(step(&l, KEEL_CYCLE_NONE, 0, 0, &out), KEEL_OK);
    }
    CHECK(out.flags & KEEL_SNAP_COMMAND_EXPIRED);
    CHECK(out.state == KEEL_STATE_ARMED && out.output_left == 0);
    sim = sim_of(l.device);
    CHECK(sim.bridge_left == 0 && sim.bridge_right == 0);
    CHECK_RC(keel_set_motors(l.device, -200, 200), KEEL_OK);
    CHECK(sim_of(l.device).bridge_left == -200);
    CHECK_RC(keel_disarm(l.device), KEEL_OK);
    CHECK(state_of(l.device) == KEEL_STATE_DISARMED);
    CHECK(!(sim_of(l.device).pins & PIN_ENABLE));
    idle(&l, 3);
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    CHECK_RC(keel_close(l.device, &fault), KEEL_OK);
    CHECK_RC(fault.status, KEEL_OK);
    CHECK_EQ(fault.cause, KEEL_CAUSE_NONE);
    CHECK_RC(fault.cleanup_status, KEEL_OK);
}

static void end_to_end_virtual(void)
{
    run_end_to_end(KEEL_OPEN_VIRTUAL_CLOCK);
}

static void end_to_end_realtime(void)
{
    run_end_to_end(0);
}

static void drive_armed(loop *l, unsigned steps)
{
    unsigned n;
    for (n = 0; n < steps; ++n) {
        CHECK_RC(step(l, KEEL_CYCLE_DRIVE, 500, 500, NULL), KEEL_OK);
    }
}

/* SPEC 12.3 named regression: a worker overrun between two ticks with no
 * pulse due is reported by the next call that reads the inputs. */
static void worker_overrun(void)
{
    keel_buttons buttons;
    keel_fault fault;
    uint64_t now = 0;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    arm(&l);
    drive_armed(&l, 5);
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_PWM_LATENESS_US, 31000), KEEL_OK);
    CHECK_RC(keel_now_us(l.device, &now), KEEL_OK);
    CHECK_RC(keel_sleep_until(l.device, now + 2000), KEEL_OK);
    CHECK(!(sim_of(l.device).pins & PIN_ENABLE)); /* the worker stopped itself */
    memset(&buttons, 0, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    CHECK_RC(keel_take_buttons(l.device, &buttons), KEEL_E_PWM);
    fault = fault_of(l.device);
    CHECK_RC(fault.status, KEEL_E_PWM);
    CHECK_EQ(fault.cause, KEEL_CAUSE_PWM_OVERRUN);
    CHECK_EQ(fault.detail, 31000);
    CHECK(state_of(l.device) == KEEL_STATE_FAULTED);
    close_clean(l.device);
}

/* PWM wakes 2.5 ms and 9.9 ms late are inside the lateness budget: they are
 * counted and the device stays ARMED. */
static void worker_lateness_tolerated(void)
{
    keel_snapshot snapshot;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    arm(&l);
    drive_armed(&l, 3);
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_PWM_LATENESS_US, 2500), KEEL_OK);
    drive_armed(&l, 3);
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_PWM_LATENESS_US, 9900), KEEL_OK);
    drive_armed(&l, 3);
    snapshot = snapshot_of(l.device);
    CHECK(snapshot.state == KEEL_STATE_ARMED);
    CHECK_EQ(snapshot.pwm_late_wakes, 2);
    CHECK_EQ(snapshot.pwm_max_lateness_us, 9900);
    close_clean(l.device);
}

static void worker_stall(void)
{
    unsigned n;
    int32_t rc = KEEL_OK;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    arm(&l);
    drive_armed(&l, 3);
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_PWM_STALL, 1), KEEL_OK);
    for (n = 0; n < 6 && rc == KEEL_OK; ++n) {
        rc = step(&l, KEEL_CYCLE_DRIVE, 500, 500, NULL);
    }
    CHECK_RC(rc, KEEL_E_PWM);
    CHECK_EQ(fault_of(l.device).cause, KEEL_CAUSE_PWM_STALL);
    close_clean(l.device);
}

static void leds_and_buttons(void)
{
    static const uint32_t frame[KEEL_LED_COUNT] = {0x010203, 0x040506, 0x070809,
                                                   0x0A0B0C, 0x0D0E0F, 0x101112};
    keel_buttons buttons;
    keel_snapshot out;
    uint32_t rgb = 0;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    CHECK_RC(keel_set_rgb(l.device, 1, 0xFF123456u), KEEL_OK); /* bits 24-31 are ignored */
    CHECK_RC(keel_get_rgb(l.device, 1, &rgb), KEEL_OK);
    CHECK_EQ(rgb, 0x123456);
    CHECK_RC(keel_set_rgb_frame(l.device, frame, KEEL_LED_COUNT), KEEL_OK);
    CHECK_RC(keel_get_rgb(l.device, 5, &rgb), KEEL_OK);
    CHECK_EQ(rgb, 0x101112);
    CHECK_RC(keel_set_rgb_frame(l.device, NULL, 1), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_set_rgb_frame(l.device, frame, 0), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_set_rgb_frame(l.device, frame, 7), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_FAIL_I2C, 1), KEEL_OK);
    CHECK_RC(keel_set_rgb(l.device, 0, 0xFFFFFF), KEEL_E_PERIPHERAL);
    CHECK_RC(keel_get_rgb(l.device, 0, &rgb), KEEL_OK);
    CHECK_EQ(rgb, 0x010203);
    CHECK(state_of(l.device) == KEEL_STATE_DISARMED);

    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_BUTTONS, KEEL_BUTTON_A | KEEL_BUTTON_Y), KEEL_OK);
    CHECK(!(sim_of(l.device).pins & PIN_BUTTON_A)); /* active low */
    idle(&l, 3);
    memset(&buttons, 0, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    CHECK_RC(keel_take_buttons(l.device, &buttons), KEEL_OK);
    CHECK(buttons.held == (KEEL_BUTTON_A | KEEL_BUTTON_Y) && buttons.pressed == buttons.held);
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_BUTTONS, 0), KEEL_OK);
    idle(&l, 3);
    CHECK_EQ(snapshot_of(l.device).buttons_released, KEEL_BUTTON_A | KEEL_BUTTON_Y); /* peek */
    CHECK_RC(step_flags(&l, KEEL_CYCLE_NONE, 0, 0, KEEL_CYCLE_TAKE_BUTTONS, &out), KEEL_OK);
    CHECK_EQ(out.buttons_released, KEEL_BUTTON_A | KEEL_BUTTON_Y);
    CHECK_EQ(snapshot_of(l.device).buttons_released, 0); /* taken by the cycle */
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_BUTTONS, 16), KEEL_E_INVALID_ARGUMENT);

    CHECK_RC(keel_set_button_leds(l.device, KEEL_BUTTON_ALL, KEEL_BUTTON_B), KEEL_OK);
    CHECK(sim_of(l.device).pins & PIN_LED_B);
    CHECK_EQ(snapshot_of(l.device).button_leds, KEEL_BUTTON_B);
    close_clean(l.device);
}

static void struct_rules(void)
{
    unsigned char buffer[sizeof(keel_snapshot) + 16];
    keel_open_options options;
    keel_open_report report;
    keel_cycle_input in[2];
    keel_snapshot out;
    keel_fault fault;
    keel_device *device = NULL;
    uint32_t size, state = 0;
    size_t i;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));

    /* Output: below the 1.0 size is refused and untouched ... */
    memset(buffer, 0xAB, sizeof buffer);
    size = 8;
    memcpy(buffer, &size, sizeof size);
    CHECK_RC(keel_get_snapshot(l.device, (keel_snapshot *)(void *)buffer), KEEL_E_INVALID_ARGUMENT);
    for (i = sizeof size; i < sizeof buffer; ++i) {
        CHECK_EQ(buffer[i], 0xAB);
    }
    /* ... a larger caller struct gets a zeroed tail and keeps its size. */
    size = (uint32_t)sizeof buffer;
    memcpy(buffer, &size, sizeof size);
    CHECK_RC(keel_get_snapshot(l.device, (keel_snapshot *)(void *)buffer), KEEL_OK);
    memcpy(&size, buffer, sizeof size);
    CHECK_EQ(size, sizeof buffer);
    for (i = sizeof(keel_snapshot); i < sizeof buffer; ++i) {
        CHECK_EQ(buffer[i], 0);
    }

    /* Input: a larger struct with a non-zero tail has no effect; zero tail is fine. */
    memset(in, 0, sizeof in);
    in[0].struct_size = (uint32_t)sizeof in;
    in[0].health = KEEL_HEALTH_ALL;
    in[0].cycle_id = 1;
    in[1].struct_size = 1;
    CHECK_RC(keel_cycle(l.device, &in[0], NULL), KEEL_E_UNSUPPORTED);
    CHECK_EQ(snapshot_of(l.device).last_cycle_id, 0); /* no tick */
    CHECK_RC(snapshot_of(l.device).last_rejection, KEEL_E_UNSUPPORTED);
    in[1].struct_size = 0;
    CHECK_RC(keel_cycle(l.device, &in[0], NULL), KEEL_OK);
    CHECK_EQ(snapshot_of(l.device).last_cycle_id, 1);
    in[0].struct_size = 8;
    in[0].cycle_id = 2;
    CHECK_RC(keel_cycle(l.device, &in[0], NULL), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_cycle(l.device, NULL, NULL), KEEL_E_INVALID_ARGUMENT);
    /* A bad command or output is decided after the tick, which still runs. */
    in[0].struct_size = sizeof in[0];
    in[0].command = 7;
    CHECK_RC(keel_cycle(l.device, &in[0], NULL), KEEL_E_INVALID_ARGUMENT);
    CHECK_EQ(snapshot_of(l.device).last_cycle_id, 2);
    in[0].command = KEEL_CYCLE_NONE;
    in[0].cycle_id = 3;
    memset(&out, 0xCD, sizeof out);
    out.struct_size = 4;
    CHECK_RC(keel_cycle(l.device, &in[0], &out), KEEL_E_INVALID_ARGUMENT);
    CHECK_EQ(snapshot_of(l.device).last_cycle_id, 3);
    CHECK_EQ(out.state, 0xCDCDCDCDu);
    close_clean(l.device);

    /* keel_open reports option problems as INVALID_ARGUMENT, never UNSUPPORTED. */
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    {
        unsigned char big[sizeof options + 8];
        memset(big, 0, sizeof big);
        size = (uint32_t)sizeof big;
        memcpy(big, &size, sizeof size);
        big[sizeof options + 3] = 1;
        CHECK_RC(keel_open((const keel_open_options *)(const void *)big, &device, &report),
                 KEEL_E_INVALID_ARGUMENT);
        CHECK(device == NULL);
        CHECK_EQ(report.resource, KEEL_RESOURCE_OPTIONS);
    }
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    options.flags = 16;
    CHECK_RC(keel_open(&options, &device, NULL), KEEL_E_INVALID_ARGUMENT);
    options.flags = 0;
    options.command_timeout_ms = 1001;
    CHECK_RC(keel_open(&options, &device, NULL), KEEL_E_INVALID_ARGUMENT);
    options.command_timeout_ms = 0;
    options.i2c_path[0] = '/';
    CHECK_RC(keel_open(&options, &device, NULL), KEEL_E_INVALID_ARGUMENT);
    options.i2c_path[0] = '\0';
    options.backend = KEEL_BACKEND_R3;
    options.flags = KEEL_OPEN_VIRTUAL_CLOCK;
    CHECK_RC(keel_open(&options, &device, NULL), KEEL_E_INVALID_ARGUMENT);
    options.backend = KEEL_BACKEND_PORT;
    options.flags = 0;
    CHECK_RC(keel_open(&options, &device, NULL), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_open(NULL, NULL, NULL), KEEL_E_INVALID_ARGUMENT);
    report.struct_size = 4;
    device = (keel_device *)&report; /* any non-NULL value */
    CHECK_RC(keel_open(NULL, &device, &report), KEEL_E_INVALID_ARGUMENT);
    CHECK(device == NULL); /* even a bad report leaves no device behind */

    /* AUTO follows KEEL_BACKEND and never falls back to the simulator. */
    report.struct_size = sizeof report;
    CHECK(unsetenv("KEEL_BACKEND") == 0);
    {
        int32_t rc = keel_open(NULL, &device, &report);
        expect_r3_reached(rc, device, &report);
    }
    CHECK(setenv("KEEL_BACKEND", "bogus", 1) == 0);
    CHECK_RC(keel_open(NULL, &device, &report), KEEL_E_NOT_FOUND);
    CHECK(setenv("KEEL_BACKEND", "simulator", 1) == 0);
    CHECK_RC(keel_open(NULL, &device, &report), KEEL_OK);
    CHECK_EQ(report.backend, KEEL_BACKEND_SIMULATOR);
    CHECK(unsetenv("KEEL_BACKEND") == 0);

    /* A bad final-fault struct is reported, but the device is closed anyway. */
    memset(&fault, 0, sizeof fault);
    fault.struct_size = 4;
    CHECK_RC(keel_close(device, &fault), KEEL_E_INVALID_ARGUMENT);
    CHECK_RC(keel_get_state(device, &state), KEEL_E_INVALID_HANDLE);
}

/* ---- the struct versioning handshake for every output struct ----------------- */

typedef int32_t (*filler)(keel_device *device, void *out);

static uint64_t handshake_cycle;

static int32_t fill_snapshot(keel_device *device, void *out)
{
    return keel_get_snapshot(device, out);
}

static int32_t fill_fault(keel_device *device, void *out)
{
    return keel_get_fault(device, out);
}

static int32_t fill_buttons(keel_device *device, void *out)
{
    return keel_take_buttons(device, out);
}

static int32_t fill_range(keel_device *device, void *out)
{
    return keel_range_poll(device, out);
}

static int32_t fill_measure(keel_device *device, void *out)
{
    return keel_range_measure(device, out);
}

static int32_t fill_sim(keel_device *device, void *out)
{
    return keel_sim_get_state(device, out);
}

static int32_t fill_cycle(keel_device *device, void *out)
{
    keel_cycle_input in;
    memset(&in, 0, sizeof in);
    in.struct_size = sizeof in;
    in.health = KEEL_HEALTH_ALL;
    in.cycle_id = ++handshake_cycle;
    return keel_cycle(device, &in, out);
}

typedef struct output_case {
    const char *name;
    filler fill;
    size_t known;
    int optional; /* NULL is a valid output */
} output_case;

static const output_case outputs[] = {
    {"snapshot", fill_snapshot, sizeof(keel_snapshot), 0},
    {"fault", fill_fault, sizeof(keel_fault), 0},
    {"buttons", fill_buttons, sizeof(keel_buttons), 0},
    {"range_poll", fill_range, sizeof(keel_range), 0},
    {"range_measure", fill_measure, sizeof(keel_range), 0},
    {"sim_state", fill_sim, sizeof(keel_sim_state), 0},
    {"cycle", fill_cycle, sizeof(keel_snapshot), 1},
};

enum { FILL = 0xA5, HANDSHAKE_BUFFER = sizeof(keel_snapshot) + 64 };

/* Fills @p buffer with FILL and sets its struct_size. */
static void prime(unsigned char *buffer, uint32_t size)
{
    memset(buffer, FILL, HANDSHAKE_BUFFER);
    memcpy(buffer, &size, sizeof size);
}

/* Everything after the struct_size field still holds FILL. */
static int untouched(const unsigned char *buffer)
{
    size_t i;
    for (i = sizeof(uint32_t); i < HANDSHAKE_BUFFER; ++i) {
        if (buffer[i] != FILL) {
            return 0;
        }
    }
    return 1;
}

static void check_output(keel_device *device, const output_case *c)
{
    static const uint32_t extras[] = {0, 4, 64};
    _Alignas(8) unsigned char buffer[HANDSHAKE_BUFFER];
    size_t e, i;
    fprintf(stderr, "output %s\n", c->name);
    for (e = 0; e < sizeof extras / sizeof extras[0]; ++e) {
        uint32_t size = (uint32_t)c->known + extras[e], seen;
        prime(buffer, size);
        CHECK_RC(c->fill(device, buffer), KEEL_OK);
        memcpy(&seen, buffer, sizeof seen);
        CHECK_EQ(seen, size); /* struct_size is never changed */
        for (i = c->known; i < size; ++i) {
            CHECK_EQ(buffer[i], 0); /* the unknown tail is zeroed */
        }
        for (i = size; i < sizeof buffer; ++i) {
            CHECK_EQ(buffer[i], FILL); /* nothing beyond the caller's size */
        }
    }
    /* Below the 1.0 size: INVALID_ARGUMENT, untouched. */
    prime(buffer, (uint32_t)c->known - 4u);
    CHECK_RC(c->fill(device, buffer), KEEL_E_INVALID_ARGUMENT);
    CHECK(untouched(buffer));
    /* Above KEEL_STRUCT_SIZE_MAX (an uninitialised field): the same, and
     * nothing is written past the caller's buffer. */
    prime(buffer, (uint32_t)KEEL_STRUCT_SIZE_MAX + 1u);
    CHECK_RC(c->fill(device, buffer), KEEL_E_INVALID_ARGUMENT);
    CHECK(untouched(buffer));
    prime(buffer, UINT32_MAX);
    CHECK_RC(c->fill(device, buffer), KEEL_E_INVALID_ARGUMENT);
    CHECK(untouched(buffer));
    CHECK_RC(c->fill(device, NULL), c->optional ? KEEL_OK : KEEL_E_INVALID_ARGUMENT);
}

/* SPEC 4: oversize outputs get a zeroed tail, undersize ones and every
 * HANDLE-class result leave the output untouched, every other status writes
 * it fully; oversize inputs need a zero tail. */
static void struct_handshake(void)
{
    _Alignas(8) unsigned char buffer[HANDSHAKE_BUFFER];
    _Alignas(8) unsigned char options_buffer[sizeof(keel_open_options) + 16];
    keel_open_options options;
    keel_device *device;
    size_t c;
    int status = 0;
    pid_t child;

    device = open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL);
    for (c = 0; c < sizeof outputs / sizeof outputs[0]; ++c) {
        check_output(device, &outputs[c]);
    }
    /* A forked child gets NOT_OWNER and touches no output. */
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int ok = 1;
        for (c = 0; c < sizeof outputs / sizeof outputs[0]; ++c) {
            prime(buffer, (uint32_t)outputs[c].known);
            ok = ok && outputs[c].fill(device, buffer) == KEEL_E_NOT_OWNER && untouched(buffer);
        }
        _exit(ok ? 0 : 1);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* An UNSUPPORTED cycle (non-zero unknown input tail) still fills out. */
    {
        _Alignas(8) unsigned char in_buffer[sizeof(keel_cycle_input) + 8];
        keel_cycle_input in;
        memset(&in, 0, sizeof in);
        in.struct_size = (uint32_t)sizeof in_buffer;
        in.health = KEEL_HEALTH_ALL;
        in.cycle_id = ++handshake_cycle;
        memset(in_buffer, 0, sizeof in_buffer);
        memcpy(in_buffer, &in, sizeof in);
        in_buffer[sizeof in] = 1;
        prime(buffer, (uint32_t)sizeof(keel_snapshot));
        CHECK_RC(keel_cycle(device, (const keel_cycle_input *)(const void *)in_buffer,
                            (keel_snapshot *)(void *)buffer),
                 KEEL_E_UNSUPPORTED);
        CHECK(!untouched(buffer));
        {
            keel_snapshot seen;
            memcpy(&seen, buffer, sizeof seen);
            CHECK_RC(seen.last_rejection, KEEL_E_UNSUPPORTED);
        }
    }
    CHECK_RC(keel_close(device, NULL), KEEL_OK);
    /* A closed handle: INVALID_HANDLE, untouched. */
    for (c = 0; c < sizeof outputs / sizeof outputs[0]; ++c) {
        prime(buffer, (uint32_t)outputs[c].known);
        CHECK_RC(outputs[c].fill(device, buffer), KEEL_E_INVALID_HANDLE);
        CHECK(untouched(buffer));
    }

    /* The open report follows the same output rule. */
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    options.flags = KEEL_OPEN_VIRTUAL_CLOCK;
    {
        _Alignas(8) unsigned char report_buffer[sizeof(keel_open_report) + 16];
        uint32_t size = (uint32_t)sizeof report_buffer;
        memset(report_buffer, FILL, sizeof report_buffer);
        memcpy(report_buffer, &size, sizeof size);
        CHECK_RC(keel_open(&options, &device, (keel_open_report *)(void *)report_buffer),
                 KEEL_OK);
        for (c = sizeof(keel_open_report); c < sizeof report_buffer; ++c) {
            CHECK_EQ(report_buffer[c], 0);
        }
    }
    CHECK_RC(keel_close(device, NULL), KEEL_OK);
    /* Oversize options with a zero tail are accepted. */
    memset(options_buffer, 0, sizeof options_buffer);
    options.struct_size = (uint32_t)sizeof options_buffer;
    memcpy(options_buffer, &options, sizeof options);
    CHECK_RC(keel_open((const keel_open_options *)(const void *)options_buffer, &device, NULL),
             KEEL_OK);
    CHECK_RC(keel_close(device, NULL), KEEL_OK);

    /* Inputs with an uninitialised struct_size: INVALID_ARGUMENT without
     * scanning past the struct for a non-zero byte. */
    {
        _Alignas(8) unsigned char report_buffer[sizeof(keel_open_report) + 16];
        keel_open_report report;
        keel_cycle_input in;
        keel_fault fault;
        device = open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL);
        memset(&in, 0, sizeof in);
        in.struct_size = UINT32_MAX;
        in.health = KEEL_HEALTH_ALL;
        in.cycle_id = ++handshake_cycle;
        CHECK_RC(keel_cycle(device, &in, NULL), KEEL_E_INVALID_ARGUMENT);
        memset(&fault, FILL, sizeof fault);
        fault.struct_size = UINT32_MAX;
        CHECK_RC(keel_close(device, &fault), KEEL_E_INVALID_ARGUMENT);
        for (c = sizeof(uint32_t); c < sizeof fault; ++c) {
            CHECK_EQ(((const unsigned char *)&fault)[c], FILL); /* untouched */
        }
        options.struct_size = UINT32_MAX;
        memset(&report, 0, sizeof report);
        report.struct_size = sizeof report;
        CHECK_RC(keel_open(&options, &device, &report), KEEL_E_INVALID_ARGUMENT);
        CHECK(device == NULL);
        CHECK_EQ(report.resource, KEEL_RESOURCE_OPTIONS);
        /* The report is the one output written on INVALID_ARGUMENT, once its
         * own struct_size is valid; an invalid one is left alone. */
        options.struct_size = sizeof options;
        options.flags = UINT32_MAX;
        memset(report_buffer, FILL, sizeof report_buffer);
        report.struct_size = (uint32_t)sizeof report_buffer;
        memcpy(report_buffer, &report.struct_size, sizeof report.struct_size);
        CHECK_RC(keel_open(&options, &device, (keel_open_report *)(void *)report_buffer),
                 KEEL_E_INVALID_ARGUMENT);
        memcpy(&report, report_buffer, sizeof report);
        CHECK_EQ(report.resource, KEEL_RESOURCE_OPTIONS);
        CHECK(report.hint[0] != '\0');
        memset(report_buffer, FILL, sizeof report_buffer);
        report.struct_size = UINT32_MAX;
        memcpy(report_buffer, &report.struct_size, sizeof report.struct_size);
        options.flags = 0;
        CHECK_RC(keel_open(&options, &device, (keel_open_report *)(void *)report_buffer),
                 KEEL_E_INVALID_ARGUMENT);
        CHECK(device == NULL);
        for (c = sizeof(uint32_t); c < sizeof report_buffer; ++c) {
            CHECK_EQ(report_buffer[c], FILL);
        }
    }
}

/* ---- servo and ranging through the public API --------------------------------- */

static void servo_and_range(void)
{
    keel_snapshot s;
    keel_range range;
    uint32_t caps = 0;
    unsigned n;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    CHECK_RC(keel_get_capabilities(l.device, &caps), KEEL_OK);
    CHECK((caps & (KEEL_CAP_SERVO | KEEL_CAP_RANGE)) == (KEEL_CAP_SERVO | KEEL_CAP_RANGE));
    idle(&l, 30);
    /* A measurement while DISARMED: sensor power needs only the heartbeat. */
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_RANGE_MM, 750), KEEL_OK);
    memset(&range, 0, sizeof range);
    range.struct_size = sizeof range;
    CHECK_RC(keel_range_measure(l.device, &range), KEEL_OK);
    CHECK(range.status == KEEL_RANGE_VALID && range.distance_mm == 750);
    CHECK_EQ(snapshot_of(l.device).sensor_power, KEEL_SENSOR_CONFIRMED);
    /* A burst once the cooldown has passed, ended by its lease. */
    idle(&l, 220);
    arm(&l);
    idle(&l, 3);
    CHECK_RC(keel_servo_start(l.device, 1800, 100), KEEL_OK);
    CHECK_EQ(state_of(l.device), KEEL_STATE_SERVO);
    CHECK_RC(step(&l, KEEL_CYCLE_NONE, 0, 0, NULL), KEEL_OK); /* same instant: no grant yet */
    CHECK_EQ(sim_of(l.device).servo_pulse_us, 0); /* not before the grant */
    CHECK_RC(step(&l, KEEL_CYCLE_NONE, 0, 0, &s), KEEL_OK);
    CHECK(s.flags & KEEL_SNAP_SERVO_GRANTED);
    CHECK(sim_of(l.device).servo_pulse_us == 1800 && (sim_of(l.device).flags & KEEL_SIM_SERVO_PERMIT));
    CHECK_RC(step(&l, KEEL_CYCLE_DRIVE, 200, 200, NULL), KEEL_E_CONFLICT);
    for (n = 0; n < 20 && state_of(l.device) == KEEL_STATE_SERVO; ++n) {
        CHECK_RC(step(&l, KEEL_CYCLE_NONE, 0, 0, NULL), KEEL_OK);
    }
    CHECK_EQ(state_of(l.device), KEEL_STATE_DISARMED);
    CHECK(sim_of(l.device).servo_pulse_us == 0 && !(sim_of(l.device).flags & KEEL_SIM_SERVO_PERMIT));
    CHECK(snapshot_of(l.device).servo_cooldown_remaining_ms > 2000);
    close_clean(l.device);

    /* Switched off by the options. */
    l.device = open_sim(KEEL_OPEN_VIRTUAL_CLOCK | KEEL_OPEN_NO_SERVO | KEEL_OPEN_NO_RANGE, NULL);
    CHECK_RC(keel_get_capabilities(l.device, &caps), KEEL_OK);
    CHECK(!(caps & (KEEL_CAP_SERVO | KEEL_CAP_RANGE)));
    CHECK_RC(keel_servo_start(l.device, 1500, 100), KEEL_E_UNSUPPORTED);
    CHECK_RC(keel_range_start(l.device), KEEL_E_UNSUPPORTED);
    CHECK_RC(keel_servo_end(l.device), KEEL_OK);
    close_clean(l.device);
}

/* ---- thread ownership -------------------------------------------------------- */

typedef struct other_call {
    keel_device *device;
    int32_t results[8];
} other_call;

static void *other_thread(void *argument)
{
    other_call *call = argument;
    uint32_t state = 0;
    call->results[0] = keel_request_arm(call->device);
    call->results[1] = keel_set_motors(call->device, 100, 100);
    call->results[2] = keel_set_motors(call->device, 0, 0);
    call->results[3] = keel_brake(call->device);
    call->results[4] = keel_clear_fault(call->device);
    call->results[5] = keel_disarm(call->device);
    call->results[6] = keel_get_state(call->device, &state);
    call->results[7] = keel_tick(call->device, 1000, KEEL_HEALTH_ALL);
    return NULL;
}

/* Stop from anywhere, go only from the loop (SPEC section 7). */
static void owner_rules(void)
{
    static const int32_t expected[8] = {KEEL_E_CONFLICT, KEEL_E_CONFLICT, KEEL_OK,
                                        KEEL_E_CONFLICT, KEEL_E_CONFLICT, KEEL_OK,
                                        KEEL_OK,         KEEL_E_HEALTH};
    other_call call;
    pthread_t thread;
    size_t i;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    CHECK_EQ(snapshot_of(l.device).owner_thread_id, 0);
    idle(&l, 2);
    CHECK(snapshot_of(l.device).owner_thread_id != 0);
    memset(&call, 0, sizeof call);
    call.device = l.device;
    CHECK(pthread_create(&thread, NULL, other_thread, &call) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    for (i = 0; i < 8; ++i) {
        CHECK_RC(call.results[i], expected[i]);
    }
    CHECK_EQ(fault_of(l.device).cause, KEEL_CAUSE_TICK_THREAD_CHANGED);
    CHECK(state_of(l.device) == KEEL_STATE_FAULTED);
    CHECK_RC(keel_clear_fault(l.device), KEEL_OK); /* the owner may clear */
    CHECK_EQ(snapshot_of(l.device).owner_thread_id, 0);
    close_clean(l.device);
}

static void *stop_from_other(void *argument)
{
    return (void *)(intptr_t)keel_emergency_stop(argument);
}

static void emergency_stop(void)
{
    keel_fault fault;
    pthread_t thread;
    void *result = NULL;
    uint32_t falls;
    unsigned n;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    arm(&l);
    drive_armed(&l, 3);
    CHECK(pthread_create(&thread, NULL, stop_from_other, l.device) == 0);
    CHECK(pthread_join(thread, &result) == 0);
    CHECK_RC((int32_t)(intptr_t)result, KEEL_OK);
    fault = fault_of(l.device);
    CHECK_RC(fault.status, KEEL_E_FAULTED);
    CHECK_EQ(fault.cause, KEEL_CAUSE_USER_STOP);
    falls = sim_of(l.device).heartbeat_falls;
    for (n = 0; n < 5; ++n) {
        CHECK_RC(step(&l, KEEL_CYCLE_NONE, 0, 0, NULL), KEEL_E_FAULTED);
    }
    CHECK_EQ(sim_of(l.device).heartbeat_falls, falls); /* the heartbeat stopped */
    CHECK(!(sim_of(l.device).flags & KEEL_SIM_MOTOR_PERMIT));
    CHECK_RC(keel_clear_fault(l.device), KEEL_OK);
    CHECK_RC(keel_request_arm(l.device), KEEL_E_STATE); /* re-arm hold-off */
    idle(&l, 100);
    arm(&l);
    close_clean(l.device);
}

/* ---- regressions of the 1.0 verification ------------------------------------- */

/* keel_open() reaches keel_r3 through the public API. /dev/null opens but is
 * no GPIO chip, so the backend itself refuses it, naming the path. */
static void r3_dispatch(void)
{
    keel_open_options options;
    keel_open_report report;
    keel_device *device = (keel_device *)&options; /* any non-NULL value */
    int32_t rc;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_R3;
    snprintf(options.gpiochip_path, sizeof options.gpiochip_path, "/dev/null");
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    rc = keel_open(&options, &device, &report);
    CHECK(device == NULL);
#if KEEL_HAVE_BACKEND_R3
    CHECK_RC(rc, KEEL_E_NOT_FOUND);
    CHECK_EQ(report.resource, KEEL_RESOURCE_GPIO_CHIP);
    CHECK(strcmp(report.path, "/dev/null") == 0);
#endif
    expect_r3_reached(rc, device, &report);
    /* The default discovery runs inside the backend as well. */
    options.gpiochip_path[0] = '\0';
    rc = keel_open(&options, &device, &report);
    expect_r3_reached(rc, device, &report);
}

/* A stop call's fault record names the state the stop ended, although its
 * own stop epoch has already taken the device to DISARMED. */
static void stop_records_prior_state(void)
{
    keel_fault fault;
    uint64_t now = 0;
    loop l;
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    arm(&l);
    drive_armed(&l, 3);
    CHECK_RC(keel_emergency_stop(l.device), KEEL_OK);
    fault = fault_of(l.device);
    CHECK_EQ(fault.cause, KEEL_CAUSE_USER_STOP);
    CHECK_EQ(fault.state, KEEL_STATE_ARMED);
    CHECK_EQ(fault.ack_phase, KEEL_ACK_NONE);
    close_clean(l.device);

    /* A backend latch found by keel_disarm(): the record says ARMED too. */
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    arm(&l);
    drive_armed(&l, 3);
    CHECK_RC(keel_sim_set(l.device, KEEL_SIM_PWM_LATENESS_US, 31000), KEEL_OK);
    CHECK_RC(keel_now_us(l.device, &now), KEEL_OK);
    CHECK_RC(keel_sleep_until(l.device, now + 2000), KEEL_OK);
    CHECK_RC(keel_disarm(l.device), KEEL_E_PWM);
    fault = fault_of(l.device);
    CHECK_EQ(fault.cause, KEEL_CAUSE_PWM_OVERRUN);
    CHECK_EQ(fault.state, KEEL_STATE_ARMED);
    close_clean(l.device);

    /* Arming: the record keeps the acknowledgement phase as well. */
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    idle(&l, 30);
    CHECK_RC(keel_request_arm(l.device), KEEL_OK);
    idle(&l, 2);
    CHECK_EQ(state_of(l.device), KEEL_STATE_ARMING);
    CHECK_RC(keel_emergency_stop(l.device), KEEL_OK);
    fault = fault_of(l.device);
    CHECK_EQ(fault.state, KEEL_STATE_ARMING);
    CHECK_EQ(fault.ack_phase, KEEL_ACK_LOW1);
    close_clean(l.device);
}

/* SPEC 12.3: (0, 0) and a refused motion command check the backend latch
 * right after their coast, so a worker overrun between two ticks is
 * reported at once instead of a stale ARMED. */
static void motion_stop_checks_latch(void)
{
    static const int32_t duties[2][2] = {{0, 0}, {5000, 0}};
    unsigned c;
    for (c = 0; c < 2; ++c) {
        keel_fault fault;
        uint64_t now = 0;
        loop l;
        loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
        arm(&l);
        drive_armed(&l, 3);
        CHECK_RC(keel_sim_set(l.device, KEEL_SIM_PWM_LATENESS_US, 31000), KEEL_OK);
        CHECK_RC(keel_now_us(l.device, &now), KEEL_OK);
        CHECK_RC(keel_sleep_until(l.device, now + 3000), KEEL_OK);
        CHECK_RC(keel_set_motors(l.device, duties[c][0], duties[c][1]), KEEL_E_PWM);
        CHECK_EQ(state_of(l.device), KEEL_STATE_FAULTED);
        fault = fault_of(l.device);
        CHECK_EQ(fault.cause, KEEL_CAUSE_PWM_OVERRUN);
        CHECK_EQ(fault.state, KEEL_STATE_ARMED);
        close_clean(l.device);
    }
}

/* Runs a DISARMED loop on @p period_ms ticks until the sensor is powered. */
static void power_sensor(loop *l, unsigned period_ms)
{
    unsigned n;
    for (n = 0; n < 40; ++n) {
        l->next_us += (uint64_t)period_ms * 1000u;
        CHECK_RC(keel_sleep_until(l->device, l->next_us), KEEL_OK);
        CHECK_RC(keel_tick(l->device, ++l->cycle_id, KEEL_HEALTH_ALL), KEEL_OK);
    }
    CHECK(snapshot_of(l->device).sensor_power >= KEEL_SENSOR_PERMITTED);
}

/* Expects keel_range_start() to report UNAVAILABLE without driving TRIG. */
static void expect_unavailable(keel_device *device)
{
    keel_range range;
    CHECK_RC(keel_range_start(device), KEEL_OK);
    memset(&range, 0, sizeof range);
    range.struct_size = sizeof range;
    CHECK_RC(keel_range_poll(device, &range), KEEL_OK);
    CHECK_EQ(range.status, KEEL_RANGE_UNAVAILABLE);
}

/* TRIG is never driven into a sensor the MCU has switched off since the
 * last tick: keel_range_start() confirms READY with a fresh read, and a
 * heartbeat older than the 70 ms tick window already counts as stopped
 * (the MCU's 80 ms window ends earlier on a fast MCU clock). */
static void range_fresh_ready(void)
{
    static const struct {
        uint32_t item;
        uint32_t settle_us;
        unsigned period_ms;
    } stimuli[] = {
        {KEEL_SIM_MCU_RESET, 1000, 10},
        {KEEL_SIM_HARDWARE_FAULT, 2000, 10},
        {KEEL_SIM_HARDWARE_FAULT, 2000, 60},
    };
    size_t i;
    loop l;
    for (i = 0; i < sizeof stimuli / sizeof stimuli[0]; ++i) {
        loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
        power_sensor(&l, stimuli[i].period_ms);
        CHECK_RC(keel_sim_set(l.device, stimuli[i].item, 1), KEEL_OK);
        l.next_us += stimuli[i].settle_us;
        CHECK_RC(keel_sleep_until(l.device, l.next_us), KEEL_OK);
        CHECK(!(sim_of(l.device).flags & KEEL_SIM_SENSOR_PERMIT));
        expect_unavailable(l.device);
        CHECK_RC(keel_close(l.device, NULL), KEEL_OK);
    }
    /* No tick for 71 ms after a fall: the heartbeat counts as stopped. */
    loop_start(&l, open_sim(KEEL_OPEN_VIRTUAL_CLOCK, NULL));
    power_sensor(&l, 10);
    CHECK_RC(keel_now_us(l.device, &l.next_us), KEEL_OK);
    l.next_us += 71000u;
    CHECK_RC(keel_sleep_until(l.device, l.next_us), KEEL_OK);
    CHECK_EQ(snapshot_of(l.device).sensor_power, KEEL_SENSOR_OFF);
    expect_unavailable(l.device);
    CHECK_RC(keel_close(l.device, NULL), KEEL_OK);
}

static void metadata(void)
{
    static const uint32_t classes[22] = {
        KEEL_CLASS_OK,       KEEL_CLASS_REJECTED, KEEL_CLASS_FAULT,    KEEL_CLASS_REJECTED,
        KEEL_CLASS_FAULT,    KEEL_CLASS_FAULT,    KEEL_CLASS_FAULT,    KEEL_CLASS_REJECTED,
        KEEL_CLASS_SETUP,    KEEL_CLASS_FAULT,    KEEL_CLASS_FAULT,    KEEL_CLASS_SETUP,
        KEEL_CLASS_SETUP,    KEEL_CLASS_SETUP,    KEEL_CLASS_REJECTED, KEEL_CLASS_REJECTED,
        KEEL_CLASS_PERIPHERAL, KEEL_CLASS_FAULT,  KEEL_CLASS_HANDLE,   KEEL_CLASS_HANDLE,
        KEEL_CLASS_FAULT,    KEEL_CLASS_SETUP};
    const char *version = keel_version_string();
    int32_t status;
    uint32_t cause;
    CHECK_EQ(keel_abi_version(), 0x00010000u);
    CHECK(strncmp(version, "1.0.0 (", 7) == 0 && strstr(version, "simulator") != NULL);
    CHECK(strlen(version) < KEEL_STRING_MAX);
    for (status = -30; status <= 5; ++status) {
        CHECK(strlen(keel_status_name(status)) < KEEL_STRING_MAX);
        CHECK(strlen(keel_status_string(status)) < KEEL_STRING_MAX);
        if (status <= 0 && status >= -21) {
            CHECK_EQ(keel_status_class(status), classes[-status]);
            CHECK(strcmp(keel_status_name(status), "KEEL_E_UNKNOWN") != 0);
        } else {
            CHECK_EQ(keel_status_class(status), KEEL_CLASS_FAULT);
            CHECK(strcmp(keel_status_name(status), "KEEL_E_UNKNOWN") == 0);
        }
    }
    CHECK(strcmp(keel_status_name(KEEL_E_PWM), "KEEL_E_PWM") == 0);
    CHECK(strcmp(keel_status_name(KEEL_E_SETUP_IO), "KEEL_E_SETUP_IO") == 0);
    for (cause = 0; cause <= KEEL_CAUSE_USER_STOP + 1; ++cause) {
        CHECK(strlen(keel_fault_cause_name(cause)) < KEEL_STRING_MAX);
    }
    CHECK(strcmp(keel_fault_cause_name(KEEL_CAUSE_NONE), "NONE") == 0);
    CHECK(strcmp(keel_fault_cause_name(KEEL_CAUSE_PWM_OVERRUN), "PWM_OVERRUN") == 0);
    CHECK(strcmp(keel_fault_cause_name(KEEL_CAUSE_USER_STOP), "USER_STOP") == 0);
    CHECK(strcmp(keel_fault_cause_name(KEEL_CAUSE_USER_STOP + 1), "UNKNOWN") == 0);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"end_to_end_virtual", end_to_end_virtual},
        {"end_to_end_realtime", end_to_end_realtime},
        {"worker_overrun", worker_overrun},
        {"worker_lateness_tolerated", worker_lateness_tolerated},
        {"worker_stall", worker_stall},
        {"leds_and_buttons", leds_and_buttons},
        {"struct_rules", struct_rules},
        {"struct_handshake", struct_handshake},
        {"servo_and_range", servo_and_range},
        {"owner_rules", owner_rules},
        {"emergency_stop", emergency_stop},
        {"metadata", metadata},
        {"r3_dispatch", r3_dispatch},
        {"stop_records_prior_state", stop_records_prior_state},
        {"motion_stop_checks_latch", motion_stop_checks_latch},
        {"range_fresh_ready", range_fresh_ready},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
