/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * A plain C99 application that sees nothing but <keel/keel.h> and the shared
 * library: it opens the simulator, runs the control loop the way the
 * bindings will (one keel_cycle per 10 ms on the device clock), arms,
 * drives, disarms and closes.
 */
#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *what, int32_t status)
{
    fprintf(stderr, "c_consumer: %s: %s (%s)\n", what, keel_status_name(status),
            keel_status_string(status));
    return 1;
}

int main(void)
{
    keel_open_options options;
    keel_open_report report;
    keel_cycle_input in;
    keel_snapshot out;
    keel_fault fault;
    keel_device *device = NULL;
    uint64_t next = 0;
    int32_t status;
    int i;

    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    options.flags = KEEL_OPEN_VIRTUAL_CLOCK;
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    status = keel_open(&options, &device, &report);
    if (status != KEEL_OK) {
        fprintf(stderr, "c_consumer: %s\n", report.hint);
        return fail("keel_open", status);
    }
    printf("libkeel %s, ABI %#x\n", keel_version_string(), (unsigned)keel_abi_version());

    memset(&in, 0, sizeof in);
    in.struct_size = sizeof in;
    in.health = KEEL_HEALTH_ALL;
    memset(&out, 0, sizeof out);
    out.struct_size = sizeof out;
    status = keel_now_us(device, &next);
    if (status == KEEL_OK) {
        status = keel_request_arm(device);
    }
    for (i = 0; i < 200 && status == KEEL_OK; ++i) {
        ++in.cycle_id;
        if (out.state == KEEL_STATE_ARMED) {
            in.command = KEEL_CYCLE_DRIVE;
            in.left = 250;
            in.right = 250;
        }
        status = keel_cycle(device, &in, &out);
        next += (uint64_t)KEEL_TICK_PERIOD_MS * 1000u;
        if (status == KEEL_OK) {
            status = keel_sleep_until(device, next);
        }
    }
    if (status != KEEL_OK) {
        return fail("control loop", status);
    }
    if (out.state != KEEL_STATE_ARMED || out.output_left != 250 || out.output_right != 250) {
        fprintf(stderr, "c_consumer: not driving (state %u)\n", (unsigned)out.state);
        return 1;
    }
    status = keel_disarm(device);
    if (status != KEEL_OK) {
        return fail("keel_disarm", status);
    }
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    status = keel_close(device, &fault);
    if (status != KEEL_OK || fault.status != KEEL_OK) {
        return fail("keel_close", status);
    }
    puts("c_consumer: armed, drove and closed the simulator");
    return 0;
}
