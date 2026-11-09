/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/* Helpers shared by the keel tools (see tool_common.h). */
#define _POSIX_C_SOURCE 200809L

#include "tool_common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t tool_signal;

static void on_signal(int number)
{
    tool_signal = number;
}

int tool_install_signals(void)
{
    static const int stops[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    struct sigaction action;
    size_t i;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < sizeof stops / sizeof stops[0]; ++i) {
        if (sigaction(stops[i], &action, NULL) != 0) {
            return -1;
        }
    }
    /* A closed stdout pipe must not kill the tool before it stops the robot. */
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL) != 0 ? -1 : 0;
}

int tool_parse_int(const char *text, long minimum, long maximum, int32_t *out)
{
    char *end;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > maximum) {
        return 0;
    }
    *out = (int32_t)value;
    return 1;
}

void tool_json_string(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', out);
    for (; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p < 0x20u) {
            fprintf(out, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

const char *tool_state_name(uint32_t state)
{
    switch (state) {
    case KEEL_STATE_DISARMED:
        return "DISARMED";
    case KEEL_STATE_ARMING:
        return "ARMING";
    case KEEL_STATE_ARMED:
        return "ARMED";
    case KEEL_STATE_SERVO:
        return "SERVO";
    case KEEL_STATE_FAULTED:
        return "FAULTED";
    default:
        return "UNKNOWN";
    }
}

static void copy_path(char *dst, size_t size, const char *src)
{
    size_t length = strlen(src);
    if (length >= size) {
        length = size - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

keel_open_options tool_options(int simulator, const char *gpiochip, const char *i2c)
{
    keel_open_options options;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = simulator ? KEEL_BACKEND_SIMULATOR : KEEL_BACKEND_R3;
    if (gpiochip) {
        copy_path(options.gpiochip_path, sizeof options.gpiochip_path, gpiochip);
    }
    if (i2c) {
        copy_path(options.i2c_path, sizeof options.i2c_path, i2c);
    }
    return options;
}

void tool_print_report(FILE *out, const keel_open_report *report)
{
    fprintf(out, "keel_open: %s (%s)\n", keel_status_name(report->status),
            keel_status_string(report->status));
    if (report->path[0]) {
        fprintf(out, "  path: %s\n", report->path);
    }
    if (report->line >= 0) {
        fprintf(out, "  line: BCM%d%s%s\n", (int)report->line, report->consumer[0] ? ", used by " : "",
                report->consumer);
    }
    if (report->os_error) {
        fprintf(out, "  error: %s\n", strerror(report->os_error));
    }
    if (report->hint[0]) {
        fprintf(out, "  remedy: %s\n", report->hint);
    }
}

void tool_get_fault(const keel_device *device, keel_fault *out)
{
    memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    if (keel_get_fault(device, out) != KEEL_OK) {
        memset(out, 0, sizeof *out);
        out->struct_size = sizeof *out;
    }
}
