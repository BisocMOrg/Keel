/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Simulator backend (SPEC section 18): the board model behind the port.h
 * hooks, with pthread domain locks, a model lock (a leaf: hooks take it and
 * call nothing while holding it) and two clocks:
 *   REALTIME  follows CLOCK_MONOTONIC; every hook first advances the model
 *             to now in 1 ms steps;
 *   VIRTUAL   (KEEL_OPEN_VIRTUAL_CLOCK) moves only inside sleeps, which
 *             makes tests and binding scenarios deterministic.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "backend/hosted.h"
#include "backend/sim/sim_board.h"

enum { DOMAIN_COUNT = KEEL_LOCK_RANGE + 1 };

typedef struct keel__sim {
    pthread_mutex_t domains[DOMAIN_COUNT]; /* CONTROL, LEDS, RANGE for the core */
    pthread_mutex_t model;                 /* the board model and the clock */
    pthread_cond_t wake;                   /* real-time sleepers; cancel_sleep */
    int virtual_clock;
    int cancelled;
    keel__sim_board board;
} keel__sim;

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000u;
}

/* Takes the model lock and brings a real-time model up to now. */
static keel__sim_board *model_enter(keel__sim *sim)
{
    pthread_mutex_lock(&sim->model);
    if (!sim->virtual_clock) {
        keel__board_advance(&sim->board, monotonic_us());
    }
    return &sim->board;
}

static void model_leave(keel__sim *sim)
{
    pthread_mutex_unlock(&sim->model);
}

static uint64_t sim_now_us(void *user)
{
    keel__sim *sim = user;
    uint64_t now;
    if (!sim->virtual_clock) {
        return monotonic_us();
    }
    pthread_mutex_lock(&sim->model);
    now = sim->board.now_us;
    pthread_mutex_unlock(&sim->model);
    return now;
}

static int32_t sim_sleep_until_us(void *user, uint64_t deadline_us)
{
    keel__sim *sim = user;
    pthread_mutex_lock(&sim->model);
    if (sim->virtual_clock) {
        if (!sim->cancelled && deadline_us > sim->board.now_us) {
            keel__board_advance(&sim->board, deadline_us);
        }
    } else {
        struct timespec until;
        until.tv_sec = (time_t)(deadline_us / UINT64_C(1000000));
        until.tv_nsec = (long)(deadline_us % UINT64_C(1000000)) * 1000L;
        while (!sim->cancelled && monotonic_us() < deadline_us) {
            if (pthread_cond_timedwait(&sim->wake, &sim->model, &until) == ETIMEDOUT) {
                break;
            }
        }
        keel__board_advance(&sim->board, monotonic_us());
    }
    pthread_mutex_unlock(&sim->model);
    return KEEL_OK;
}

static int32_t sim_read_inputs(void *user, uint32_t *out_inputs)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_read(model_enter(sim), out_inputs);
    model_leave(sim);
    return rc;
}

static int32_t sim_write_outputs(void *user, uint32_t mask, uint32_t values, uint32_t stop_epoch)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_write_outputs(model_enter(sim), mask, values, stop_epoch);
    model_leave(sim);
    return rc;
}

static int32_t sim_write_motors(void *user, const keel_port_motor_command *command)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_write_motors(model_enter(sim), command);
    model_leave(sim);
    return rc;
}

static int32_t sim_emergency_disable(void *user, uint32_t stop_epoch)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_emergency_disable(model_enter(sim), stop_epoch);
    model_leave(sim);
    return rc;
}

static int32_t sim_get_fault(void *user, keel_port_fault *out)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_get_fault(model_enter(sim), out);
    model_leave(sim);
    return rc;
}

static int32_t sim_reset_fault(void *user)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_reset_fault(model_enter(sim));
    model_leave(sim);
    return rc;
}

static int32_t sim_get_stats(void *user, keel_port_stats *out)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_stats(model_enter(sim), out);
    model_leave(sim);
    return rc;
}

static int32_t sim_i2c_write(void *user, uint8_t address, uint8_t reg, const uint8_t *data,
                             size_t count)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_i2c(model_enter(sim), address, reg, data, count);
    model_leave(sim);
    return rc;
}

static int32_t sim_servo(void *user, const keel_port_servo_command *command)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_servo(model_enter(sim), command);
    model_leave(sim);
    return rc;
}

static int32_t sim_range_trigger(void *user, uint32_t *out_echo_high)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_range_trigger(model_enter(sim), out_echo_high);
    model_leave(sim);
    return rc;
}

static int32_t sim_range_edges(void *user, keel_port_edge *edges, uint32_t capacity,
                               uint32_t *out_count)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_range_edges(model_enter(sim), edges, capacity, out_count);
    model_leave(sim);
    return rc;
}

static int32_t sim_set(void *user, uint32_t item, int32_t value)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_set(model_enter(sim), item, value);
    model_leave(sim);
    return rc;
}

static int32_t sim_get_state(void *user, keel_sim_state *out)
{
    keel__sim *sim = user;
    int32_t rc = keel__board_state(model_enter(sim), out);
    model_leave(sim);
    return rc;
}

static void sim_lock(void *user, uint32_t domain)
{
    pthread_mutex_lock(&((keel__sim *)user)->domains[domain]);
}

static void sim_unlock(void *user, uint32_t domain)
{
    pthread_mutex_unlock(&((keel__sim *)user)->domains[domain]);
}

static uint64_t sim_thread_id(void *user)
{
    (void)user;
    return (uint64_t)syscall(SYS_gettid);
}

static void sim_cancel_sleep(void *user)
{
    keel__sim *sim = user;
    pthread_mutex_lock(&sim->model);
    sim->cancelled = 1;
    pthread_cond_broadcast(&sim->wake);
    pthread_mutex_unlock(&sim->model);
}

static void destroy_locks(keel__sim *sim, unsigned mutexes, int cond)
{
    unsigned i;
    if (cond) {
        pthread_cond_destroy(&sim->wake);
    }
    for (i = 0; i < mutexes; ++i) {
        pthread_mutex_destroy(i < DOMAIN_COUNT ? &sim->domains[i] : &sim->model);
    }
}

/* Called last by keel_close(): the core's device storage lives in this block. */
static void sim_destroy(void *user)
{
    keel__sim *sim = user;
    destroy_locks(sim, DOMAIN_COUNT + 1, 1);
    free(sim);
}

/* Returns 0 or the errno of the first failing initialisation. */
static int init_locks(keel__sim *sim)
{
    pthread_condattr_t attributes;
    unsigned i;
    int rc = 0;
    for (i = 0; i < DOMAIN_COUNT + 1 && rc == 0; ++i) {
        rc = pthread_mutex_init(i < DOMAIN_COUNT ? &sim->domains[i] : &sim->model, NULL);
    }
    if (rc != 0) {
        destroy_locks(sim, i - 1, 0);
        return rc;
    }
    rc = pthread_condattr_init(&attributes);
    if (rc == 0) {
        rc = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
        if (rc == 0) {
            rc = pthread_cond_init(&sim->wake, &attributes);
        }
        pthread_condattr_destroy(&attributes);
    }
    if (rc != 0) {
        destroy_locks(sim, DOMAIN_COUNT + 1, 0);
    }
    return rc;
}

static size_t round_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

int32_t keel__sim_open(const keel_open_options *options, keel_device **out_device,
                       keel_open_report *report)
{
    size_t core_size = keel_port_storage_size();
    size_t core_align = keel_port_storage_align();
    size_t align = core_align > _Alignof(keel__sim) ? core_align : _Alignof(keel__sim);
    size_t offset = round_up(sizeof(keel__sim), core_align);
    keel_port_backend be;
    keel__sim *sim;
    int rc;

    sim = aligned_alloc(align, round_up(offset + core_size, align));
    if (!sim) {
        keel__report_fail(report, KEEL_RESOURCE_MEMORY, ENOMEM, NULL,
                          "out of memory while creating the simulator");
        return KEEL_E_NO_MEMORY;
    }
    memset(sim, 0, sizeof *sim);
    rc = init_locks(sim);
    if (rc != 0) {
        free(sim);
        keel__report_fail(report, KEEL_RESOURCE_BACKEND, rc, NULL,
                          "could not create the simulator's locks");
        return KEEL_E_SETUP_IO;
    }
    sim->virtual_clock = (options->flags & KEEL_OPEN_VIRTUAL_CLOCK) != 0;
    keel__board_init(&sim->board, sim->virtual_clock ? 0u : monotonic_us());

    memset(&be, 0, sizeof be);
    be.struct_size = sizeof be;
    be.port_version = KEEL_PORT_VERSION;
    be.capabilities = KEEL_CAP_MOTORS | KEEL_CAP_BRAKE | KEEL_CAP_RGB | KEEL_CAP_BUTTONS |
                      KEEL_CAP_BUTTON_LEDS | KEEL_CAP_SIMULATED;
    be.user = sim;
    be.now_us = sim_now_us;
    be.sleep_until_us = sim_sleep_until_us;
    be.read_inputs = sim_read_inputs;
    be.write_outputs = sim_write_outputs;
    be.write_motors = sim_write_motors;
    be.emergency_disable = sim_emergency_disable;
    be.get_fault = sim_get_fault;
    be.reset_fault = sim_reset_fault;
    be.destroy = sim_destroy;
    be.lock = sim_lock;
    be.unlock = sim_unlock;
    be.thread_id = sim_thread_id;
    be.cancel_sleep = sim_cancel_sleep;
    be.get_stats = sim_get_stats;
    be.i2c_write = sim_i2c_write;
    be.sim_set = sim_set;
    be.sim_get_state = sim_get_state;
    /* Without a servo hook the model's request line simply stays LOW. */
    if (!(options->flags & KEEL_OPEN_NO_SERVO)) {
        be.capabilities |= KEEL_CAP_SERVO;
        be.servo = sim_servo;
    }
    if (!(options->flags & KEEL_OPEN_NO_RANGE)) {
        be.capabilities |= KEEL_CAP_RANGE;
        be.range_trigger = sim_range_trigger;
        be.range_edges = sim_range_edges;
    }
    return keel__attach((unsigned char *)sim + offset, core_size, &be, options,
                        KEEL_BACKEND_SIMULATOR, out_device, report);
}
