/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel-demo: a small, complete libkeel program, written the way an
 * application loop should be (SPEC 21): one thread, a 10 ms period on the
 * device clock, one keel_cycle per period carrying the tick and the motor
 * command, and every status handled by its class.
 *
 * By default it mirrors the buttons on their indicator LEDs and prints
 * button edges. With --drive LEFT RIGHT MS it also arms and drives for MS
 * milliseconds, then disarms. Button B ends the demo normally; Ctrl-C ends
 * it with an emergency stop, as stop buttons in applications should.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <string.h>

#include "tool_common.h"

enum {
    PERIOD_US = KEEL_TICK_PERIOD_MS * 1000,
    ARM_LIMIT_US = 5000000, /* arming normally takes about 0.6 s */
    IDLE_BLUE = 0x000008    /* dim blue: the demo is running */
};

typedef struct demo {
    keel_device *dev;
    int drive;
    int32_t left, right, drive_ms, duration_ms;
    uint64_t started_us, driving_since_us;
    uint32_t last_state;
} demo;

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "Usage: %s [--drive LEFT RIGHT MS] [--duration MS] [--simulator]\n"
            "          [--gpio-chip PATH] [--i2c PATH]\n"
            "Mirrors the buttons on their LEDs; button B ends the demo, Ctrl-C stops the robot.\n"
            "  --drive LEFT RIGHT MS  arm, drive at LEFT/RIGHT permille (-1000..1000) for MS ms\n"
            "                         (1..2000), then disarm and end\n"
            "  --duration MS          end after MS ms (1..3600000)\n"
            "  --simulator            use the simulator instead of the robot\n",
            program);
}

/* Returns -1 to run, otherwise the exit status. */
static int parse(int argc, char **argv, demo *d, int *simulator, const char **gpiochip,
                 const char **i2c)
{
    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--drive") == 0 && i + 3 < argc &&
            tool_parse_int(argv[i + 1], -KEEL_DUTY_MAX, KEEL_DUTY_MAX, &d->left) &&
            tool_parse_int(argv[i + 2], -KEEL_DUTY_MAX, KEEL_DUTY_MAX, &d->right) &&
            tool_parse_int(argv[i + 3], 1, 2000, &d->drive_ms)) {
            d->drive = 1;
            i += 3;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc &&
                   tool_parse_int(argv[i + 1], 1, 3600000, &d->duration_ms)) {
            ++i;
        } else if (strcmp(argv[i], "--simulator") == 0) {
            *simulator = 1;
        } else if (strcmp(argv[i], "--gpio-chip") == 0 && i + 1 < argc) {
            *gpiochip = argv[++i];
        } else if (strcmp(argv[i], "--i2c") == 0 && i + 1 < argc) {
            *i2c = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (*simulator && (*gpiochip || *i2c)) {
        usage(stderr, argv[0]);
        return 2;
    }
    return -1;
}

static void print_fault(const demo *d)
{
    keel_fault fault;
    tool_get_fault(d->dev, &fault);
    if (fault.status != KEEL_OK) {
        fprintf(stderr, "fault: %s, cause %s, detail %" PRIu64 "%s%s\n",
                keel_status_name(fault.status), keel_fault_cause_name(fault.cause), fault.detail,
                fault.os_error ? ", " : "", fault.os_error ? strerror(fault.os_error) : "");
    }
}

/* The motor command for this cycle: drive once ARMED, until the time is up. */
static void plan(demo *d, const keel_snapshot *last, uint64_t now, keel_cycle_input *in)
{
    if (!d->drive || last->state != KEEL_STATE_ARMED) {
        return;
    }
    if (!d->driving_since_us) {
        d->driving_since_us = now;
    }
    in->command = KEEL_CYCLE_DRIVE;
    in->left = d->left;
    in->right = d->right;
}

/* Returns 1 while the demo continues, 0 when it ended normally, and -1 on
 * an error. */
static int step(demo *d, uint64_t cycle, keel_snapshot *snapshot)
{
    keel_cycle_input in;
    uint64_t now = 0;
    int32_t rc;
    (void)keel_now_us(d->dev, &now);
    memset(&in, 0, sizeof in);
    in.struct_size = sizeof in;
    in.health = KEEL_HEALTH_ALL; /* this loop's work is the cycle itself */
    in.cycle_id = cycle;
    in.flags = KEEL_CYCLE_TAKE_BUTTONS;
    plan(d, snapshot, now, &in);
    rc = keel_cycle(d->dev, &in, snapshot);
    switch (keel_status_class(rc)) {
    case KEEL_CLASS_OK:
        break;
    case KEEL_CLASS_REJECTED:
        if (rc == KEEL_E_INVALID_ARGUMENT || rc == KEEL_E_UNSUPPORTED) {
            fprintf(stderr, "keel_cycle: %s (a defect in this program)\n", keel_status_name(rc));
            return -1;
        }
        fprintf(stderr, "refused: %s\n", keel_status_string(rc)); /* e.g. not armed yet */
        break;
    case KEEL_CLASS_HANDLE:
        fprintf(stderr, "keel_cycle: %s\n", keel_status_name(rc));
        return -1;
    default:
        fprintf(stderr, "stopped: %s\n", keel_status_string(rc));
        print_fault(d);
        return -1;
    }
    if (snapshot->state != d->last_state) {
        printf("state: %s\n", tool_state_name(snapshot->state));
        d->last_state = snapshot->state;
    }
    if (snapshot->buttons_pressed || snapshot->buttons_released) {
        printf("buttons: held=%" PRIx32 " pressed=%" PRIx32 " released=%" PRIx32 "\n",
               snapshot->buttons_held, snapshot->buttons_pressed, snapshot->buttons_released);
    }
    fflush(stdout);
    if (snapshot->buttons_held & KEEL_BUTTON_B) {
        return 0;
    }
    if (snapshot->button_leds != snapshot->buttons_held &&
        keel_set_button_leds(d->dev, KEEL_BUTTON_ALL, snapshot->buttons_held) != KEEL_OK) {
        return -1;
    }
    if (d->drive && d->driving_since_us &&
        now - d->driving_since_us >= (uint64_t)d->drive_ms * 1000u) {
        return 0;
    }
    if (d->drive && !d->driving_since_us && now - d->started_us > ARM_LIMIT_US) {
        fprintf(stderr, "arming did not complete within 5 s\n");
        return -1;
    }
    if (d->duration_ms && now - d->started_us >= (uint64_t)d->duration_ms * 1000u) {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    demo d;
    keel_open_options options;
    keel_open_report report;
    keel_snapshot snapshot;
    keel_fault fault;
    const char *gpiochip = NULL, *i2c = NULL;
    uint64_t next, cycle = 0;
    int simulator = 0, running = 1, status;
    int32_t rc;

    memset(&d, 0, sizeof d);
    status = parse(argc, argv, &d, &simulator, &gpiochip, &i2c);
    if (status >= 0) {
        return status;
    }
    if (tool_install_signals() != 0) {
        fprintf(stderr, "could not install signal handlers\n");
        return 1;
    }
    options = tool_options(simulator, gpiochip, i2c);
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    rc = keel_open(&options, &d.dev, &report);
    if (rc != KEEL_OK || report.warnings) {
        tool_print_report(rc != KEEL_OK ? stderr : stdout, &report);
    }
    if (rc != KEEL_OK) {
        return 1;
    }
    if (keel_set_rgb_all(d.dev, IDLE_BLUE) != KEEL_OK) {
        printf("RGB LEDs unavailable; continuing without them\n");
    }
    (void)keel_now_us(d.dev, &d.started_us);
    d.last_state = KEEL_STATE_DISARMED;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.struct_size = sizeof snapshot;
    status = 0;
    if (d.drive && (rc = keel_request_arm(d.dev)) != KEEL_OK) {
        fprintf(stderr, "keel_request_arm: %s\n", keel_status_string(rc));
        status = 1;
        running = 0;
    }
    next = d.started_us;
    while (running && !tool_signal) {
        int outcome = step(&d, ++cycle, &snapshot);
        if (outcome <= 0) {
            status = outcome < 0;
            break;
        }
        next += PERIOD_US;
        if (keel_sleep_until(d.dev, next) != KEEL_OK) {
            status = 1;
            break;
        }
    }
    if (tool_signal) {
        (void)keel_emergency_stop(d.dev); /* Ctrl-C is a stop button */
        printf("emergency stop (signal %d)\n", (int)tool_signal);
        status = 130;
    } else if (keel_disarm(d.dev) != KEEL_OK) {
        print_fault(&d);
        status = 1;
    } else if (d.last_state == KEEL_STATE_ARMING || d.last_state == KEEL_STATE_ARMED) {
        printf("state: DISARMED\n");
    }
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    if (keel_close(d.dev, &fault) != KEEL_OK) {
        fprintf(stderr, "close: the final stop was not confirmed (%s)\n",
                keel_status_name(fault.cleanup_status));
        status = 1;
    }
    return status;
}
