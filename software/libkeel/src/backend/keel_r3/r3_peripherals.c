/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel_r3 peripherals: the RGB LED driver's I2C adapter and the HC-SR04 on
 * J1. Both are peripheral paths: every failure is reported to the core as a
 * bare status, which it turns into KEEL_E_PERIPHERAL without latching.
 *
 * I2C uses one I2C_RDWR transaction per write with the address in the
 * message, so there is no per-descriptor address state, and a 20 ms adapter
 * timeout, so a held bus cannot stall LED calls for the default second. The
 * bus is not flock()ed: the exclusive GPIO requests already prevent a second
 * keel program, and other I2C users of the bus are legitimate.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "backend/keel_r3/r3.h"

static const char default_i2c_path[] = "/dev/i2c-1";

/* ---- I2C ------------------------------------------------------------------ */

static void i2c_unavailable(keel_open_report *report, const char *path, int error)
{
    const char *hint;
    report->warnings |= KEEL_OPEN_WARN_NO_RGB;
    if (report->resource != KEEL_RESOURCE_NONE) {
        return; /* the report describes the first degradation only */
    }
    switch (error) {
    case ENOENT:
    case ENODEV:
    case ENXIO:
        hint = "the I2C bus is missing: enable it (dtparam=i2c_arm=on) and load i2c-dev with sudo "
               "keel-setup; RGB LEDs are disabled";
        break;
    case EACCES:
    case EPERM:
        hint = "no permission for the I2C bus: add the user to the i2c (or keel) group with sudo "
               "keel-setup --user NAME; RGB LEDs are disabled";
        break;
    default:
        hint = "the I2C bus could not be set up; RGB LEDs are disabled (run keel-doctor)";
        break;
    }
    keel__r3_report(report, KEEL_RESOURCE_I2C_BUS, error, path, -1, NULL, hint);
}

void keel__r3_i2c_open(keel__r3 *b, const keel_open_options *options, keel_open_report *report)
{
    const char *path = options->i2c_path[0] ? options->i2c_path : default_i2c_path;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    int error;
    if (fd < 0) {
        error = errno ? errno : EIO;
        i2c_unavailable(report, path, error);
        return;
    }
    /* I2C_TIMEOUT counts in 10 ms units. ENOTTY here means the node is not
     * an I2C adapter. */
    if (ioctl(fd, I2C_TIMEOUT, (unsigned long)KEEL__R3_I2C_TIMEOUT_10MS) < 0) {
        error = errno ? errno : EIO;
        (void)close(fd);
        i2c_unavailable(report, path, error);
        return;
    }
    b->i2c_fd = fd;
}

int32_t keel__r3_i2c_write(void *user, uint8_t address, uint8_t reg, const uint8_t *data,
                           size_t count)
{
    keel__r3 *b = user;
    uint8_t buffer[1 + KEEL__R3_I2C_MAX_DATA];
    struct i2c_msg message;
    struct i2c_rdwr_ioctl_data transfer;
    if (!data || count == 0 || count > KEEL__R3_I2C_MAX_DATA || address > 0x7Fu) {
        return KEEL_E_IO;
    }
    buffer[0] = reg;
    memcpy(buffer + 1, data, count);
    memset(&message, 0, sizeof message);
    message.addr = address;
    message.flags = 0;
    message.len = (uint16_t)(count + 1u);
    message.buf = buffer;
    transfer.msgs = &message;
    transfer.nmsgs = 1;
    /* Never retried: a partly delivered write is ambiguous, and the next
     * frame rewrites every PWM byte anyway. */
    return ioctl(b->i2c_fd, I2C_RDWR, &transfer) < 0 ? KEEL_E_IO : KEEL_OK;
}

/* ---- ranging -------------------------------------------------------------- */

int32_t keel__r3_range_trigger(void *user, uint32_t *out_echo_high)
{
    keel__r3 *b = user;
    struct timespec pulse;
    uint64_t echo = 0;
    *out_echo_high = 0;
    if (keel__r3_get_values(b->range_fd, KEEL__R3_RANGE_ECHO, &echo)) {
        return KEEL_E_IO;
    }
    if (echo) {
        /* The previous echo has not ended; a trigger now would be lost. */
        *out_echo_high = 1;
        return KEEL_OK;
    }
    if (keel__r3_set_values(b->range_fd, KEEL__R3_RANGE_TRIG, KEEL__R3_RANGE_TRIG)) {
        return KEEL_E_IO;
    }
    /* At least 10 us HIGH; a longer pulse is harmless, the burst starts on
     * the falling edge. */
    pulse.tv_sec = 0;
    pulse.tv_nsec = KEEL__R3_TRIG_PULSE_US * 1000L;
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &pulse, &pulse) == EINTR) {
    }
    return keel__r3_set_values(b->range_fd, KEEL__R3_RANGE_TRIG, 0) ? KEEL_E_IO : KEEL_OK;
}

int32_t keel__r3_range_edges(void *user, keel_port_edge *edges, uint32_t capacity,
                             uint32_t *out_count)
{
    keel__r3 *b = user;
    struct gpio_v2_line_event events[KEEL__R3_EDGE_BATCH];
    uint32_t wanted = capacity < KEEL__R3_EDGE_BATCH ? capacity : KEEL__R3_EDGE_BATCH;
    uint32_t count, i;
    ssize_t got;
    int lost = 0;
    *out_count = 0;
    if (wanted == 0) {
        return KEEL_OK;
    }
    /* The descriptor is non-blocking: no event pending is EAGAIN. */
    got = read(b->range_fd, events, wanted * sizeof events[0]);
    if (got < 0) {
        return errno == EAGAIN ? KEEL_OK : KEEL_E_IO;
    }
    count = (uint32_t)((size_t)got / sizeof events[0]);
    for (i = 0; i < count; ++i) {
        /* The kernel drops the oldest event when its buffer overflows; a gap
         * in the line's sequence numbers is the only trace of that. */
        if (b->echo_seqno && events[i].line_seqno != b->echo_seqno + 1u) {
            lost = 1;
        }
        b->echo_seqno = events[i].line_seqno;
        memset(&edges[i], 0, sizeof edges[i]);
        edges[i].time_us = events[i].timestamp_ns / 1000u;
        edges[i].rising = events[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE ? 1u : 0u;
    }
    *out_count = count;
    return lost ? KEEL_E_IO : KEEL_OK;
}
