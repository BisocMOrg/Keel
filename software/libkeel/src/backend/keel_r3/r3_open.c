/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel_r3 lifetime (SPEC 6 and 17): one allocation holds the backend and
 * the core's device storage; open acquires the lines, the I2C bus and the
 * PWM worker in that order and attaches the core; destroy lowers every line,
 * joins the worker and releases everything.
 *
 * The core's lock domains are priority-inheritance mutexes. Besides
 * protecting the real-time worker from priority inversion, a PI mutex hands
 * a contended lock straight to the waiting thread on unlock, so a thread
 * calling in a tight loop cannot keep re-taking a domain ahead of a stop.
 *
 * A forked child closes its copies of the line, chip and I2C descriptors.
 * The parent's lines stay requested, and a child that outlived its parent
 * cannot keep the robot's lines (ENABLE among them) requested.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "backend/hosted.h"
#include "backend/keel_r3/r3.h"

/* Synchronisation objects, initialised in this order and destroyed in
 * reverse: the domains, the backend lock, the worker's condition variable,
 * the sleep lock and the sleepers' condition variable. */
enum {
    SYNC_LOCK = KEEL__R3_DOMAINS,
    SYNC_WORK,
    SYNC_SLEEP_LOCK,
    SYNC_SLEEP_COND,
    SYNC_COUNT
};

static pthread_mutex_t open_list_lock = PTHREAD_MUTEX_INITIALIZER;
static keel__r3 *open_list;
static pthread_once_t fork_once = PTHREAD_ONCE_INIT;

/* ---- clock, report and errno ---------------------------------------------- */

uint64_t keel__r3_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000u;
}

void keel__r3_report(keel_open_report *report, uint32_t resource, int32_t os_error,
                     const char *path, int32_t line, const char *consumer, const char *hint)
{
    memset(report->path, 0, sizeof report->path);
    memset(report->consumer, 0, sizeof report->consumer);
    keel__report_fail(report, resource, os_error, path, hint);
    report->line = line;
    if (consumer) {
        size_t length = strlen(consumer);
        if (length >= sizeof report->consumer) {
            length = sizeof report->consumer - 1;
        }
        memcpy(report->consumer, consumer, length);
    }
}

int32_t keel__r3_open_status(int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
        return KEEL_E_PERMISSION;
    case ENOENT:
    case ENODEV:
    case ENXIO:
        return KEEL_E_NOT_FOUND;
    case EBUSY:
        return KEEL_E_IN_USE;
    case ENOMEM:
        return KEEL_E_NO_MEMORY;
    default:
        return KEEL_E_SETUP_IO;
    }
}

/* ---- synchronisation ------------------------------------------------------ */

static int mutex_init(pthread_mutex_t *mutex, int protocol)
{
    pthread_mutexattr_t attributes;
    int error = pthread_mutexattr_init(&attributes);
    if (error) {
        return error;
    }
    error = pthread_mutexattr_setprotocol(&attributes, protocol);
    if (!error) {
        error = pthread_mutex_init(mutex, &attributes);
    }
    pthread_mutexattr_destroy(&attributes);
    return error;
}

static int cond_init(pthread_cond_t *cond)
{
    pthread_condattr_t attributes;
    int error = pthread_condattr_init(&attributes);
    if (error) {
        return error;
    }
    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (!error) {
        error = pthread_cond_init(cond, &attributes);
    }
    pthread_condattr_destroy(&attributes);
    return error;
}

static int sync_init(keel__r3 *b)
{
    int error = 0;
    while (!error && b->sync_ready < SYNC_COUNT) {
        unsigned i = b->sync_ready;
        if (i < KEEL__R3_DOMAINS) {
            error = mutex_init(&b->domains[i], PTHREAD_PRIO_INHERIT);
        } else if (i == SYNC_LOCK) {
            error = mutex_init(&b->lock, PTHREAD_PRIO_INHERIT);
        } else if (i == SYNC_WORK) {
            error = cond_init(&b->work);
        } else if (i == SYNC_SLEEP_LOCK) {
            error = mutex_init(&b->sleep_lock, PTHREAD_PRIO_NONE);
        } else {
            error = cond_init(&b->sleep_cond);
        }
        if (!error) {
            b->sync_ready++;
        }
    }
    return error;
}

static void sync_destroy(keel__r3 *b)
{
    while (b->sync_ready) {
        unsigned i = --b->sync_ready;
        if (i < KEEL__R3_DOMAINS) {
            pthread_mutex_destroy(&b->domains[i]);
        } else if (i == SYNC_LOCK) {
            pthread_mutex_destroy(&b->lock);
        } else if (i == SYNC_WORK) {
            pthread_cond_destroy(&b->work);
        } else if (i == SYNC_SLEEP_LOCK) {
            pthread_mutex_destroy(&b->sleep_lock);
        } else {
            pthread_cond_destroy(&b->sleep_cond);
        }
    }
}

/* ---- descriptors and fork ------------------------------------------------- */

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void close_all(keel__r3 *b)
{
    close_fd(&b->range_fd);
    close_fd(&b->input_fd);
    close_fd(&b->motor_fd);
    close_fd(&b->control_fd);
    close_fd(&b->chip_fd);
    close_fd(&b->i2c_fd);
}

/* Holding the list lock across fork() means the child never sees it
 * mid-update; the forking thread releases it on both sides. */
static void fork_prepare(void)
{
    pthread_mutex_lock(&open_list_lock);
}

static void fork_parent(void)
{
    pthread_mutex_unlock(&open_list_lock);
}

/* Only close: a write from the child would move the parent's lines, which
 * share the same kernel requests. */
static void fork_child(void)
{
    keel__r3 *b;
    for (b = open_list; b; b = b->next_open) {
        close_all(b);
    }
    pthread_mutex_unlock(&open_list_lock);
}

static void fork_register(void)
{
    (void)pthread_atfork(fork_prepare, fork_parent, fork_child);
}

static void list_add(keel__r3 *b)
{
    pthread_once(&fork_once, fork_register);
    pthread_mutex_lock(&open_list_lock);
    b->next_open = open_list;
    open_list = b;
    pthread_mutex_unlock(&open_list_lock);
}

static void list_remove(keel__r3 *b)
{
    keel__r3 **link;
    pthread_mutex_lock(&open_list_lock);
    for (link = &open_list; *link; link = &(*link)->next_open) {
        if (*link == b) {
            *link = b->next_open;
            break;
        }
    }
    pthread_mutex_unlock(&open_list_lock);
}

/* Every resource in reverse order of acquisition; safe on a partial open. */
static void release(keel__r3 *b)
{
    if (b->sync_ready == SYNC_COUNT) {
        keel__r3_worker_stop(b);
    }
    list_remove(b);
    close_all(b);
    sync_destroy(b);
    if (b->memory_locked) {
        (void)munlock(b, b->block_size);
    }
    free(b);
}

/* ---- hooks ---------------------------------------------------------------- */

static uint64_t r3_now_us(void *user)
{
    (void)user;
    return keel__r3_now_us();
}

static int32_t r3_sleep_until_us(void *user, uint64_t deadline_us)
{
    keel__r3 *b = user;
    struct timespec until = keel__r3_timespec(deadline_us);
    pthread_mutex_lock(&b->sleep_lock);
    while (!b->sleep_cancelled && keel__r3_now_us() < deadline_us) {
        if (pthread_cond_timedwait(&b->sleep_cond, &b->sleep_lock, &until) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&b->sleep_lock);
    return KEEL_OK;
}

static void r3_cancel_sleep(void *user)
{
    keel__r3 *b = user;
    pthread_mutex_lock(&b->sleep_lock);
    b->sleep_cancelled = 1;
    pthread_cond_broadcast(&b->sleep_cond);
    pthread_mutex_unlock(&b->sleep_lock);
}

static void r3_lock(void *user, uint32_t domain)
{
    pthread_mutex_lock(&((keel__r3 *)user)->domains[domain]);
}

static void r3_unlock(void *user, uint32_t domain)
{
    pthread_mutex_unlock(&((keel__r3 *)user)->domains[domain]);
}

static uint64_t r3_thread_id(void *user)
{
    (void)user;
    return (uint64_t)gettid();
}

/* Called last by keel_close() (or by a failed attach). */
static void r3_destroy(void *user)
{
    release(user);
}

/* ---- open ----------------------------------------------------------------- */

static size_t round_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

/* Everything idle and closed; returns 0 or the errno of a failing lock. */
static int backend_init(keel__r3 *b, size_t block_size)
{
    memset(b, 0, sizeof *b);
    b->block_size = block_size;
    b->chip_fd = b->control_fd = b->motor_fd = b->input_fd = b->range_fd = b->i2c_fd = -1;
    b->motor.mode = KEEL_MOTOR_COAST;
    b->fault.struct_size = sizeof b->fault;
    b->stats.struct_size = sizeof b->stats;
    keel__pwm_budget_reset(&b->budget);
    atomic_init(&b->latched, 0);
    return sync_init(b);
}

static void fill_table(keel__r3 *b, keel_port_backend *be)
{
    memset(be, 0, sizeof *be);
    be->struct_size = sizeof *be;
    be->port_version = KEEL_PORT_VERSION;
    /* No servo hook: J5 is not fitted, and BCM19 stays LOW. */
    be->capabilities = KEEL_CAP_MOTORS | KEEL_CAP_BRAKE | KEEL_CAP_BUTTONS | KEEL_CAP_BUTTON_LEDS;
    be->user = b;
    be->now_us = r3_now_us;
    be->sleep_until_us = r3_sleep_until_us;
    be->read_inputs = keel__r3_read_inputs;
    be->write_outputs = keel__r3_write_outputs;
    be->write_motors = keel__r3_write_motors;
    be->emergency_disable = keel__r3_emergency_disable;
    be->get_fault = keel__r3_get_fault;
    be->reset_fault = keel__r3_reset_fault;
    be->destroy = r3_destroy;
    be->lock = r3_lock;
    be->unlock = r3_unlock;
    be->thread_id = r3_thread_id;
    be->cancel_sleep = r3_cancel_sleep;
    be->get_stats = keel__r3_get_stats;
    if (b->i2c_fd >= 0) {
        be->capabilities |= KEEL_CAP_RGB;
        be->i2c_write = keel__r3_i2c_write;
    }
    if (b->range_fd >= 0) {
        be->capabilities |= KEEL_CAP_RANGE;
        be->range_trigger = keel__r3_range_trigger;
        be->range_edges = keel__r3_range_edges;
    }
    if (b->timing_class == KEEL_TIMING_REALTIME) {
        be->capabilities |= KEEL_CAP_REALTIME_PWM;
    }
}

int32_t keel__r3_open(const keel_open_options *options, keel_device **out_device,
                      keel_open_report *report)
{
    size_t core_size = keel_port_storage_size();
    size_t core_align = keel_port_storage_align();
    size_t align = core_align > _Alignof(keel__r3) ? core_align : _Alignof(keel__r3);
    size_t offset = round_up(sizeof(keel__r3), core_align);
    size_t size = round_up(offset + core_size, align);
    keel_port_backend be;
    keel__r3 *b;
    int32_t rc;
    int error;

    *out_device = NULL;
    b = aligned_alloc(align, size);
    if (!b) {
        keel__r3_report(report, KEEL_RESOURCE_MEMORY, ENOMEM, NULL, -1, NULL,
                        "out of memory while opening the keel_r3 backend");
        return KEEL_E_NO_MEMORY;
    }
    error = backend_init(b, size);
    if (error) {
        keel__r3_report(report, KEEL_RESOURCE_BACKEND, error, NULL, -1, NULL,
                        "could not create the keel_r3 locks");
        rc = keel__r3_open_status(error);
        release(b);
        return rc;
    }
    rc = keel__r3_gpio_open(b, options, report);
    if (rc == KEEL_OK) {
        keel__r3_i2c_open(b, options, report);
        rc = keel__r3_worker_start(b, options, report);
    }
    if (rc != KEEL_OK) {
        /* No device: degradations recorded on the way no longer apply. */
        report->warnings = 0;
        report->realtime_os_error = 0;
        release(b);
        return rc;
    }
    /* Best effort: the worker touches this block on every wake. */
    b->memory_locked = mlock(b, size) == 0;
    list_add(b);
    fill_table(b, &be);
    /* On failure the core calls r3_destroy, which releases everything. */
    return keel__attach((unsigned char *)b + offset, core_size, &be, options, KEEL_BACKEND_R3,
                        out_device, report);
}
