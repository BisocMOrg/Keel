/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Attaching the core to a backend (SPEC section 19): option and backend-table
 * validation, safe output levels, LED initialisation and handle publication.
 * keel_port_attach() and the storage queries form the C porting interface;
 * they are hidden in libkeel.so and usable from libkeel.a.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "backend/hosted.h"
#include "core/internal.h"

enum {
    KNOWN_CAPABILITIES = KEEL_CAP_MOTORS | KEEL_CAP_BRAKE | KEEL_CAP_RGB | KEEL_CAP_BUTTONS |
                         KEEL_CAP_BUTTON_LEDS | KEEL_CAP_SERVO | KEEL_CAP_RANGE |
                         KEEL_CAP_SIMULATED | KEEL_CAP_REALTIME_PWM,
    KNOWN_OPEN_FLAGS = KEEL_OPEN_NO_SERVO | KEEL_OPEN_NO_RANGE | KEEL_OPEN_NO_REALTIME |
                       KEEL_OPEN_VIRTUAL_CLOCK
};

/* Capabilities the options switch off, whatever the backend offers. */
static uint32_t refused_capabilities(const keel_open_options *options)
{
    return ((options->flags & KEEL_OPEN_NO_SERVO) ? (uint32_t)KEEL_CAP_SERVO : 0u) |
           ((options->flags & KEEL_OPEN_NO_RANGE) ? (uint32_t)KEEL_CAP_RANGE : 0u);
}

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t length = strlen(src);
    if (length >= size) {
        length = size - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static int terminated(const char *text, size_t size)
{
    return memchr(text, '\0', size) != NULL;
}

void keel__report_init(keel_open_report *report)
{
    memset(report, 0, sizeof *report);
    report->struct_size = sizeof *report;
    report->line = -1;
}

void keel__report_fail(keel_open_report *report, uint32_t resource, int32_t os_error,
                       const char *path, const char *hint)
{
    report->resource = resource;
    report->os_error = os_error;
    if (path) {
        copy_text(report->path, sizeof report->path, path);
    }
    copy_text(report->hint, sizeof report->hint, hint);
}

static int32_t bad_options(keel_open_report *report, const char *hint)
{
    keel__report_fail(report, KEEL_RESOURCE_OPTIONS, 0, NULL, hint);
    return KEEL_E_INVALID_ARGUMENT;
}

int32_t keel__options_check(const keel_open_options *in, keel_open_options *out,
                            keel_open_report *report)
{
    int32_t rc;
    memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    if (!in) {
        return KEEL_OK;
    }
    rc = keel__in_check(in, sizeof *in);
    if (rc == KEEL_E_UNSUPPORTED) {
        /* keel_open() may only fail with INVALID_ARGUMENT or SETUP codes. */
        return bad_options(report, "the options struct sets fields this library does not know");
    }
    if (rc != KEEL_OK) {
        return bad_options(report,
                           "options struct_size is below the libkeel 1.0 size or above "
                           "KEEL_STRUCT_SIZE_MAX");
    }
    memcpy(out, in, sizeof *out);
    out->struct_size = sizeof *out;
    if (out->backend > KEEL_BACKEND_SIMULATOR) {
        return bad_options(report, "unknown backend (use AUTO, R3 or SIMULATOR)");
    }
    if (out->flags & ~(uint32_t)KNOWN_OPEN_FLAGS) {
        return bad_options(report, "unknown open flag");
    }
    if (out->command_timeout_ms != 0 &&
        (out->command_timeout_ms < KEEL_COMMAND_TIMEOUT_MIN_MS ||
         out->command_timeout_ms > KEEL_COMMAND_TIMEOUT_MAX_MS)) {
        return bad_options(report, "command_timeout_ms must be 0 (default) or 20..1000");
    }
    if (!terminated(out->gpiochip_path, sizeof out->gpiochip_path) ||
        !terminated(out->i2c_path, sizeof out->i2c_path)) {
        return bad_options(report, "a path option is not NUL-terminated");
    }
    return KEEL_OK;
}

/* SPEC 19: required hooks, capability hooks, lock hooks or single-threaded. */
static int backend_valid(const keel_port_backend *be)
{
    uint32_t caps;
    if (!be || keel__in_check(be, sizeof *be) != KEEL_OK || be->port_version != KEEL_PORT_VERSION) {
        return 0;
    }
    if (!be->now_us || !be->sleep_until_us || !be->read_inputs || !be->write_outputs ||
        !be->write_motors || !be->emergency_disable || !be->get_fault || !be->reset_fault) {
        return 0;
    }
    caps = be->capabilities;
    if ((caps & ~(uint32_t)KNOWN_CAPABILITIES) || (be->flags & ~(uint32_t)KEEL_PORT_SINGLE_THREADED)) {
        return 0;
    }
    if (((caps & KEEL_CAP_SERVO) && !be->servo) ||
        ((caps & KEEL_CAP_RANGE) && (!be->range_trigger || !be->range_edges)) ||
        ((caps & KEEL_CAP_RGB) && !be->i2c_write)) {
        return 0;
    }
    if (!be->lock != !be->unlock) {
        return 0;
    }
    return be->lock || (be->flags & KEEL_PORT_SINGLE_THREADED);
}

/* port.h: a failed attach releases the backend through destroy. */
static void release_backend(const keel_port_backend *be)
{
    if (be && be->struct_size >= sizeof *be && be->destroy) {
        be->destroy(be->user);
    }
}

/* Drives every output to its safe level, whatever the backend did before. */
static int32_t safe_levels(keel__device *dev, uint32_t *raw)
{
    keel_port_motor_command coast;
    int32_t rc = dev->be.reset_fault(dev->be.user);
    memset(&coast, 0, sizeof coast);
    coast.mode = KEEL_MOTOR_COAST;
    if (rc == KEEL_OK) {
        rc = dev->be.emergency_disable(dev->be.user, 0);
    }
    if (rc == KEEL_OK) {
        rc = dev->be.write_motors(dev->be.user, &coast);
    }
    if (rc == KEEL_OK) {
        rc = dev->be.write_outputs(dev->be.user, KEEL_OUT_ALL, 0, 0);
    }
    if (rc == KEEL_OK && dev->be.servo) {
        keel_port_servo_command off;
        memset(&off, 0, sizeof off);
        off.mode = KEEL_PORT_SERVO_OFF;
        rc = dev->be.servo(dev->be.user, &off);
    }
    if (rc == KEEL_OK) {
        rc = dev->be.read_inputs(dev->be.user, raw);
    }
    if (rc == KEEL_OK && (*raw & KEEL_IN_BACKEND_FAULT)) {
        rc = KEEL_E_IO;
    }
    return rc;
}

int32_t keel__attach(void *storage, size_t storage_size, const keel_port_backend *backend,
                     const keel_open_options *options, uint32_t backend_kind,
                     keel_device **out_device, keel_open_report *report)
{
    keel__device *dev = storage;
    keel_port_stats stats;
    keel_port_fault fault;
    uint32_t raw = 0;
    uint64_t now;
    int32_t rc;

    *out_device = NULL;
    report->backend = backend_kind;
    if (!backend_valid(backend)) {
        keel__report_fail(report, KEEL_RESOURCE_BACKEND, 0, NULL,
                          "invalid backend table (size, version, hooks, capabilities or locks)");
        release_backend(backend);
        return KEEL_E_INVALID_ARGUMENT;
    }
    if (!storage || storage_size < sizeof *dev ||
        (uintptr_t)storage % _Alignof(keel__device) != 0) {
        keel__report_fail(report, KEEL_RESOURCE_MEMORY, 0, NULL,
                          "device storage is smaller than keel_port_storage_size() or misaligned");
        release_backend(backend);
        return KEEL_E_INVALID_ARGUMENT;
    }

    memset(dev, 0, sizeof *dev);
    memcpy(&dev->be, backend, sizeof dev->be);
    dev->be.struct_size = sizeof dev->be;
    dev->backend_kind = backend_kind;
    dev->caps = backend->capabilities & ~refused_capabilities(options);
    dev->command_timeout_us = KEEL__MS(options->command_timeout_ms ? options->command_timeout_ms
                                                                   : KEEL_COMMAND_TIMEOUT_DEFAULT_MS);
    dev->single_threaded = (backend->flags & KEEL_PORT_SINGLE_THREADED) && backend->thread_id;
    dev->attach_thread = backend->thread_id ? backend->thread_id(backend->user) : 0u;
    atomic_init(&dev->stop_epoch, 0u);
    atomic_init(&dev->closing, 0);
    atomic_init(&dev->last_rejection, KEEL_OK);
    atomic_init(&dev->ready_drops, 0u);
    atomic_init(&dev->echo_seen, 0);
    atomic_init(&dev->echo_rise_us, UINT64_C(0));
    dev->state = KEEL_STATE_DISARMED;
    dev->record.struct_size = sizeof dev->record;

    rc = safe_levels(dev, &raw);
    if (rc != KEEL_OK) {
        memset(&fault, 0, sizeof fault);
        fault.struct_size = sizeof fault;
        (void)dev->be.get_fault(dev->be.user, &fault);
        keel__report_fail(report, KEEL_RESOURCE_BACKEND, fault.os_error, NULL,
                          "driving the outputs to their safe levels failed");
        release_backend(&dev->be);
        return KEEL_E_SETUP_IO;
    }
    now = dev->be.now_us(dev->be.user);
    dev->observed_us = now;
    dev->inputs = raw & (KEEL_IN_READY | KEEL_IN_ARMED | KEEL_IN_BUTTONS);
    keel__servo_init(dev, now);
    keel__range_init(dev);
    keel__note_inputs(dev, now);
    keel__buttons_init(dev, now);
    keel__motion_init(dev, now);
    dev->enable_low_since_us = now;
    /* The MCU needs falls at least 20 ms apart; the first pulse waits 30 ms. */
    dev->hb_not_before_us = now + KEEL__MS(KEEL_HEARTBEAT_MIN_MS);

    if ((dev->caps & KEEL_CAP_RGB) && keel__leds_init(dev) != KEEL_OK) {
        /* LEDs are not safety-relevant: degrade instead of failing. */
        dev->caps &= ~(uint32_t)KEEL_CAP_RGB;
        report->warnings |= KEEL_OPEN_WARN_NO_RGB;
        if (report->resource == KEEL_RESOURCE_NONE) {
            keel__report_fail(report, KEEL_RESOURCE_LED_DRIVER, 0, NULL,
                              "the RGB LED driver at I2C address 0x54 did not respond; RGB LEDs "
                              "are disabled");
        }
    }

    rc = keel__registry_add(dev, out_device);
    if (rc != KEEL_OK) {
        keel__report_fail(report, KEEL_RESOURCE_MEMORY, 0, NULL,
                          "no free device handle (64 per process); close unused devices");
        release_backend(&dev->be);
        return rc;
    }
    report->capabilities = dev->caps;
    memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;
    if (dev->be.get_stats && dev->be.get_stats(dev->be.user, &stats) == KEEL_OK) {
        report->timing_class = stats.timing_class;
    }
    return KEEL_OK;
}

size_t keel_port_storage_size(void)
{
    return sizeof(keel__device);
}

size_t keel_port_storage_align(void)
{
    return _Alignof(keel__device);
}

int32_t keel_port_attach(void *storage, size_t storage_size, const keel_port_backend *backend,
                         const keel_open_options *options, keel_device **out_device,
                         keel_open_report *report)
{
    keel_open_report local;
    keel_open_options resolved;
    int32_t rc;
    if (report && !keel__out_ok(report, sizeof *report)) {
        release_backend(backend);
        return KEEL_E_INVALID_ARGUMENT;
    }
    keel__report_init(&local);
    local.backend = KEEL_BACKEND_PORT;
    if (!out_device) {
        keel__report_fail(&local, KEEL_RESOURCE_NONE, 0, NULL, "out_device must not be NULL");
        release_backend(backend);
        rc = KEEL_E_INVALID_ARGUMENT;
    } else {
        *out_device = NULL;
        rc = keel__options_check(options, &resolved, &local);
        if (rc == KEEL_OK &&
            (resolved.backend != KEEL_BACKEND_AUTO || resolved.gpiochip_path[0] ||
             resolved.i2c_path[0] || (resolved.flags & KEEL_OPEN_VIRTUAL_CLOCK))) {
            rc = bad_options(&local, "a port takes no backend, path or virtual-clock option");
        }
        if (rc == KEEL_OK) {
            rc = keel__attach(storage, storage_size, backend, &resolved, KEEL_BACKEND_PORT,
                              out_device, &local);
        } else {
            release_backend(backend);
        }
    }
    local.status = rc;
    if (report) {
        keel__out_write(report, &local, sizeof local);
    }
    return rc;
}
