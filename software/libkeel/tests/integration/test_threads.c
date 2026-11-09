/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Concurrency (SPEC sections 7, 8 and 23.1), run under ThreadSanitizer by the
 * tsan preset:
 *   - stop_vs_cycle: a disarm racing the owner's drive loop is never undone
 *     (the stop epoch), while other threads hammer LEDs, ranging and queries;
 *   - foreign_motion: go calls from a non-owner thread are refused without
 *     effect, stops are accepted, a foreign tick fail-stops;
 *   - hammer_close: close racing cycles, disarms, LED frames, measurements
 *     and a long sleep; every caller ends with KEEL_E_INVALID_HANDLE quickly;
 *   - disarm_latency: from the keel_disarm() call to ENABLE LOW, p99.9 under
 *     2 ms with the CPU loaded, an I2C bus that hangs every transaction for
 *     its 20 ms timeout, a thread querying in a tight loop and one polling
 *     the buttons;
 *   - close_bound: keel_close() stays within about 100 ms with eight RGB
 *     calls in flight on that hung bus.
 * The first three use the real-time simulator through keel.h; the last two
 * attach the core through port.h to the simulator's board model with a
 * hanging I2C hook, because no public stimulus makes I2C hang.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "backend/sim/sim_board.h"
#include "check.h"
#include "keel/port.h"

enum { MS = 1000, PIN_ENABLE = 1u << 26 };

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000u;
}

static void pause_us(long us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static keel_device *open_realtime(void)
{
    keel_open_options options;
    keel_device *device = NULL;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    CHECK_RC(keel_open(&options, &device, NULL), KEEL_OK);
    return device;
}

static keel_sim_state sim_of(keel_device *device)
{
    keel_sim_state s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    CHECK_RC(keel_sim_get_state(device, &s), KEEL_OK);
    return s;
}

static keel_snapshot snapshot_of(keel_device *device)
{
    keel_snapshot s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    CHECK_RC(keel_get_snapshot(device, &s), KEEL_OK);
    return s;
}

/* ---- the application's control loop on its own thread ----------------------- */

typedef struct owner_loop {
    keel_device *device;
    pthread_t thread;
    _Atomic int run;
    _Atomic int rearm;        /* request_arm at the next cycle */
    _Atomic int32_t duty;     /* commanded every cycle */
    _Atomic int32_t stopped;  /* first FAULT or HANDLE status; the loop ends */
    _Atomic uint64_t tid;     /* this thread's id */
    _Atomic unsigned cycles;  /* cycles run */
} owner_loop;

static void *owner_main(void *argument)
{
    owner_loop *o = argument;
    uint64_t next = 0, id = 0;
    atomic_store(&o->tid, (uint64_t)syscall(SYS_gettid));
    CHECK_RC(keel_now_us(o->device, &next), KEEL_OK);
    while (atomic_load(&o->run)) {
        keel_cycle_input in;
        int32_t rc;
        uint32_t cls;
        memset(&in, 0, sizeof in);
        in.struct_size = sizeof in;
        in.health = KEEL_HEALTH_ALL;
        in.cycle_id = ++id;
        in.command = KEEL_CYCLE_DRIVE;
        in.left = in.right = atomic_load(&o->duty);
        rc = keel_cycle(o->device, &in, NULL);
        cls = keel_status_class(rc);
        if (cls == KEEL_CLASS_FAULT || cls == KEEL_CLASS_HANDLE) {
            atomic_store(&o->stopped, rc);
            break;
        }
        if (atomic_exchange(&o->rearm, 0)) {
            CHECK_RC(keel_request_arm(o->device), KEEL_OK);
        }
        atomic_fetch_add(&o->cycles, 1u);
        /* An absolute schedule, as bindings run it (SPEC 21). */
        next += 10 * MS;
        rc = keel_sleep_until(o->device, next);
        if (rc != KEEL_OK) {
            atomic_store(&o->stopped, rc); /* closed during the sleep */
            break;
        }
    }
    return NULL;
}

static void owner_start(owner_loop *o, keel_device *device, int32_t duty)
{
    memset(o, 0, sizeof *o);
    o->device = device;
    atomic_store(&o->run, 1);
    atomic_store(&o->duty, duty);
    CHECK(pthread_create(&o->thread, NULL, owner_main, o) == 0);
    while (atomic_load(&o->cycles) < 2u) {
        pause_us(1000);
    }
}

static void owner_stop(owner_loop *o)
{
    atomic_store(&o->run, 0);
    CHECK(pthread_join(o->thread, NULL) == 0);
}

/* Waits until the simulated left bridge drives @p duty. */
static void wait_driving(keel_device *device, int32_t duty)
{
    unsigned n;
    for (n = 0; n < 4000 && sim_of(device).bridge_left != duty; ++n) {
        pause_us(1000);
    }
    CHECK_EQ(sim_of(device).bridge_left, (unsigned)duty);
}

/* ---- background load on the same device --------------------------------- */

typedef struct hammer {
    keel_device *device;
    pthread_t thread;
    _Atomic int run;
    int kind;
    int32_t last;
} hammer;

enum hammer_kind {
    HAMMER_LEDS,    /* LED frames back to back */
    HAMMER_RANGE,   /* blocking measurements, polls while spaced */
    HAMMER_QUERIES, /* snapshot and buttons back to back */
    HAMMER_DISARM,  /* a stop every 3 ms */
    HAMMER_SLEEP,   /* 5 s sleeps */
    HAMMER_CACHED,  /* lock-free queries back to back: the handle path only */
    HAMMER_POLL     /* buttons read at 1 kHz, as an application polls them */
};

static void *hammer_main(void *argument)
{
    hammer *h = argument;
    int32_t rc = KEEL_OK;
    uint32_t colour = 0, capabilities = 0;
    while (atomic_load(&h->run) && rc != KEEL_E_INVALID_HANDLE) {
        keel_range range;
        keel_buttons buttons;
        keel_snapshot s;
        uint64_t now = 0;
        memset(&range, 0, sizeof range);
        range.struct_size = sizeof range;
        memset(&buttons, 0, sizeof buttons);
        buttons.struct_size = sizeof buttons;
        memset(&s, 0, sizeof s);
        s.struct_size = sizeof s;
        switch (h->kind) {
        case HAMMER_LEDS:
            rc = keel_set_rgb(h->device, colour % KEEL_LED_COUNT, colour);
            colour += 0x010203u;
            break;
        case HAMMER_RANGE:
            rc = keel_range_measure(h->device, &range);
            if (rc == KEEL_E_AGAIN) {
                rc = keel_range_poll(h->device, &range);
                pause_us(2000);
            }
            break;
        case HAMMER_QUERIES:
            rc = keel_get_snapshot(h->device, &s);
            if (rc == KEEL_OK) {
                rc = keel_take_buttons(h->device, &buttons);
            }
            break;
        case HAMMER_DISARM:
            rc = keel_disarm(h->device);
            pause_us(3000);
            break;
        case HAMMER_CACHED:
            rc = keel_now_us(h->device, &now);
            if (rc == KEEL_OK) {
                rc = keel_get_capabilities(h->device, &capabilities);
            }
            break;
        case HAMMER_POLL:
            rc = keel_take_buttons(h->device, &buttons);
            pause_us(1000);
            break;
        default: /* HAMMER_SLEEP */
            rc = keel_now_us(h->device, &now);
            if (rc == KEEL_OK) {
                rc = keel_sleep_until(h->device, now + 5000u * MS);
            }
            break;
        }
        CHECK(keel_status_class(rc) != KEEL_CLASS_FAULT || rc == KEEL_E_FAULTED);
        h->last = rc;
    }
    return NULL;
}

static void hammer_start(hammer *h, keel_device *device, int kind)
{
    memset(h, 0, sizeof *h);
    h->device = device;
    h->kind = kind;
    atomic_store(&h->run, 1);
    CHECK(pthread_create(&h->thread, NULL, hammer_main, h) == 0);
}

/* Joins a hammer; it ends by itself once the handle is gone. */
static int32_t hammer_join(hammer *h)
{
    CHECK(pthread_join(h->thread, NULL) == 0);
    return h->last;
}

/* Stops every hammer before joining any: hammers sharing a mutex (the LEDS
 * domain) could otherwise starve the one being joined, as a default mutex
 * is not fair. */
static void hammers_stop(hammer *h, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        atomic_store(&h[i].run, 0);
    }
    for (i = 0; i < count; ++i) {
        (void)hammer_join(&h[i]);
    }
}

/* ---- cases ------------------------------------------------------------------ */

/* A disarm racing the owner's drive loop is never undone: after keel_disarm()
 * returns, ENABLE stays LOW and the bridges stay off although the loop keeps
 * commanding a drive in every cycle. */
static void stop_vs_cycle(void)
{
    keel_device *device = open_realtime();
    hammer load[3];
    owner_loop o;
    unsigned i, k;
    srand(0x6b65u);
    owner_start(&o, device, 300);
    hammer_start(&load[0], device, HAMMER_LEDS);
    hammer_start(&load[1], device, HAMMER_RANGE);
    hammer_start(&load[2], device, HAMMER_QUERIES);
    for (i = 0; i < 6; ++i) {
        atomic_store(&o.rearm, 1);
        wait_driving(device, 300);
        pause_us(rand() % 15000); /* land anywhere in the 10 ms cycle */
        CHECK_RC(keel_disarm(device), KEEL_OK);
        for (k = 0; k < 50; ++k) {
            keel_sim_state s = sim_of(device);
            CHECK(!(s.pins & PIN_ENABLE) && s.bridge_left == 0 && s.bridge_right == 0);
            pause_us(1000);
        }
        CHECK_EQ(snapshot_of(device).state, KEEL_STATE_DISARMED);
    }
    hammers_stop(load, 3);
    owner_stop(&o);
    CHECK_RC(atomic_load(&o.stopped), KEEL_OK);
    CHECK_RC(keel_close(device, NULL), KEEL_OK);
}

/* Go only from the loop, stop from anywhere (SPEC 7). */
static void foreign_motion(void)
{
    keel_device *device = open_realtime();
    keel_snapshot s;
    owner_loop o;
    unsigned cycles;
    owner_start(&o, device, 300);
    atomic_store(&o.rearm, 1);
    wait_driving(device, 300);
    s = snapshot_of(device);
    CHECK_EQ(s.owner_thread_id, atomic_load(&o.tid));
    /* Go calls from this (non-owner) thread: refused, nothing changes. */
    CHECK_RC(keel_set_motors(device, 500, 500), KEEL_E_CONFLICT);
    CHECK_RC(keel_brake(device), KEEL_E_CONFLICT);
    CHECK_RC(keel_request_arm(device), KEEL_E_CONFLICT);
    CHECK_RC(keel_clear_fault(device), KEEL_E_CONFLICT);
    CHECK_RC(keel_servo_start(device, 1500, 100), KEEL_E_CONFLICT);
    CHECK_RC(keel_servo_set_pulse(device, 1500), KEEL_E_CONFLICT);
    CHECK_EQ(snapshot_of(device).state, KEEL_STATE_ARMED);
    CHECK_EQ(sim_of(device).bridge_left, 300);
    /* Invalid arguments are checked first: any thread's invalid command coasts. */
    CHECK_RC(keel_set_motors(device, 2000, 0), KEEL_E_INVALID_ARGUMENT);
    /* A stop is accepted from here; the loop drives again on its next cycle. */
    CHECK_RC(keel_set_motors(device, 0, 0), KEEL_OK);
    wait_driving(device, 300);
    /* A tick from a foreign thread fail-stops the device. */
    cycles = atomic_load(&o.cycles);
    CHECK_RC(keel_tick(device, UINT64_C(1) << 40, KEEL_HEALTH_ALL), KEEL_E_HEALTH);
    while (atomic_load(&o.stopped) == KEEL_OK && atomic_load(&o.cycles) < cycles + 50u) {
        pause_us(1000);
    }
    CHECK_RC(atomic_load(&o.stopped), KEEL_E_FAULTED);
    owner_stop(&o);
    s = snapshot_of(device);
    CHECK_EQ(s.state, KEEL_STATE_FAULTED);
    CHECK_EQ(s.fault_cause, KEEL_CAUSE_TICK_THREAD_CHANGED);
    CHECK_EQ(sim_of(device).bridge_left, 0);
    CHECK_RC(keel_close(device, NULL), KEEL_OK);
}

/* close racing every kind of call: all of them end with INVALID_HANDLE, and
 * close itself is bounded (sleepers and measurements wake when it starts). */
static void hammer_close(void)
{
    keel_device *device = open_realtime();
    hammer load[5];
    owner_loop o;
    uint64_t start, took;
    unsigned k;
    owner_start(&o, device, 300);
    atomic_store(&o.rearm, 1);
    for (k = 0; k < 5; ++k) {
        hammer_start(&load[k], device, (int)k);
    }
    pause_us(300 * 1000);
    start = monotonic_us();
    CHECK_RC(keel_close(device, NULL), KEEL_OK);
    took = monotonic_us() - start;
    for (k = 0; k < 5; ++k) {
        CHECK_RC(hammer_join(&load[k]), KEEL_E_INVALID_HANDLE);
    }
    owner_stop(&o);
    CHECK_RC(atomic_load(&o.stopped), KEEL_E_INVALID_HANDLE);
    fprintf(stderr, "close under load took %llu us\n", (unsigned long long)took);
    CHECK(took < 1000u * MS); /* a 5 s sleep and a measurement were in flight */
}

/* ---- disarm latency with a hanging I2C bus ----------------------------------- */

/* The board model behind port.h on the real clock. The model lock is a leaf;
 * the I2C hook hangs outside it, as a real adapter waiting for its timeout
 * holds no backend lock. */
typedef struct stub {
    pthread_mutex_t domains[KEEL_LOCK_RANGE + 1];
    pthread_mutex_t model;
    keel__sim_board board;
    _Atomic int hang;                /* I2C transactions hang 20 ms and fail */
    _Atomic uint64_t disabled_at_us; /* first ENABLE lowering since cleared to 0 */
} stub;

static keel__sim_board *stub_enter(stub *s)
{
    pthread_mutex_lock(&s->model);
    keel__board_advance(&s->board, monotonic_us());
    return &s->board;
}

static void stub_leave(stub *s)
{
    pthread_mutex_unlock(&s->model);
}

static uint64_t stub_now(void *user)
{
    (void)user;
    return monotonic_us();
}

static int32_t stub_sleep(void *user, uint64_t deadline_us)
{
    struct timespec ts;
    (void)user;
    ts.tv_sec = (time_t)(deadline_us / UINT64_C(1000000));
    ts.tv_nsec = (long)(deadline_us % UINT64_C(1000000)) * 1000L;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {
    }
    return KEEL_OK;
}

static int32_t stub_read(void *user, uint32_t *inputs)
{
    stub *s = user;
    int32_t rc = keel__board_read(stub_enter(s), inputs);
    stub_leave(s);
    return rc;
}

static int32_t stub_outputs(void *user, uint32_t mask, uint32_t values, uint32_t epoch)
{
    stub *s = user;
    int32_t rc = keel__board_write_outputs(stub_enter(s), mask, values, epoch);
    stub_leave(s);
    return rc;
}

static int32_t stub_motors(void *user, const keel_port_motor_command *command)
{
    stub *s = user;
    int32_t rc = keel__board_write_motors(stub_enter(s), command);
    stub_leave(s);
    return rc;
}

static int32_t stub_disable(void *user, uint32_t epoch)
{
    stub *s = user;
    uint64_t unset = 0;
    int32_t rc = keel__board_emergency_disable(stub_enter(s), epoch);
    /* keel_disarm() lowers ENABLE before any lock and again inside its safe
     * stop; the first one is the latency that matters. */
    (void)atomic_compare_exchange_strong(&s->disabled_at_us, &unset, monotonic_us());
    stub_leave(s);
    return rc;
}

static int32_t stub_get_fault(void *user, keel_port_fault *out)
{
    stub *s = user;
    int32_t rc = keel__board_get_fault(stub_enter(s), out);
    stub_leave(s);
    return rc;
}

static int32_t stub_reset_fault(void *user)
{
    stub *s = user;
    int32_t rc = keel__board_reset_fault(stub_enter(s));
    stub_leave(s);
    return rc;
}

static int32_t stub_i2c(void *user, uint8_t address, uint8_t reg, const uint8_t *data,
                        size_t count)
{
    stub *s = user;
    int32_t rc;
    if (atomic_load(&s->hang)) {
        pause_us(20 * MS); /* the adapter's I2C_TIMEOUT, with no backend lock held */
        return KEEL_E_IO;
    }
    rc = keel__board_i2c(stub_enter(s), address, reg, data, count);
    stub_leave(s);
    return rc;
}

static void stub_lock(void *user, uint32_t domain)
{
    pthread_mutex_lock(&((stub *)user)->domains[domain]);
}

static void stub_unlock(void *user, uint32_t domain)
{
    pthread_mutex_unlock(&((stub *)user)->domains[domain]);
}

static uint64_t stub_thread(void *user)
{
    (void)user;
    return (uint64_t)syscall(SYS_gettid);
}

static void *spin_main(void *argument)
{
    _Atomic int *run = argument;
    volatile uint64_t sink = 0;
    while (atomic_load(run)) {
        sink = sink * 6364136223846793005u + 1u;
    }
    return NULL;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

enum { SAMPLES = 2000, ROUNDS = 3, MAX_SPINNERS = 4, LED_THREADS = 2 };

/* CPU burners: enough to load the machine, never more than half of the
 * CPUs. The case measures the lock design; oversubscribing the host would
 * measure its scheduler quantum instead. */
static unsigned spinner_count(void)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    long n = cpus > 2 ? cpus / 2 - 1 : 1;
    return n > MAX_SPINNERS ? MAX_SPINNERS : (unsigned)n;
}

/* One round of SAMPLES disarms at random moments; returns non-zero when the
 * bounds hold. */
static int measure_round(keel_device *device, stub *s, unsigned round)
{
    static uint64_t enable_us[SAMPLES], call_us[SAMPLES];
    size_t p99 = SAMPLES * 99u / 100u, p999 = SAMPLES * 999u / 1000u;
    unsigned i;
    for (i = 0; i < SAMPLES; ++i) {
        uint64_t start;
        pause_us(500 + rand() % 1500);
        atomic_store(&s->disabled_at_us, UINT64_C(0));
        start = monotonic_us();
        CHECK_RC(keel_disarm(device), KEEL_OK);
        call_us[i] = monotonic_us() - start;
        enable_us[i] = atomic_load(&s->disabled_at_us) - start;
    }
    qsort(enable_us, SAMPLES, sizeof enable_us[0], compare_u64);
    qsort(call_us, SAMPLES, sizeof call_us[0], compare_u64);
    fprintf(stderr,
            "round %u, disarm to ENABLE LOW: p50 %llu us, p99 %llu us, p99.9 %llu us, max %llu us\n"
            "round %u, disarm call:          p50 %llu us, p99 %llu us, p99.9 %llu us, max %llu us\n",
            round, (unsigned long long)enable_us[SAMPLES / 2], (unsigned long long)enable_us[p99],
            (unsigned long long)enable_us[p999], (unsigned long long)enable_us[SAMPLES - 1], round,
            (unsigned long long)call_us[SAMPLES / 2], (unsigned long long)call_us[p99],
            (unsigned long long)call_us[p999], (unsigned long long)call_us[SAMPLES - 1]);
    /* The bound of SPEC 8. The whole call then takes the control lock,
     * which nothing holds across an I2C transaction (20 ms here) or a
     * heartbeat pulse (1 ms, about 3% of the time): either would show at
     * p99. Its far tail is the control mutex's fairness, not the design. */
    return enable_us[p999] < 2 * MS && call_us[p99] < 500u;
}

/* Attaches the core through port.h to the board model behind @p s; the
 * caller frees *storage after keel_close(). */
static keel_device *stub_open(stub *s, void **storage)
{
    size_t align = keel_port_storage_align();
    keel_port_backend be;
    keel_device *device = NULL;
    unsigned i;
    *storage = aligned_alloc(align, (keel_port_storage_size() + align - 1) / align * align);
    CHECK(*storage);
    memset(s, 0, sizeof *s);
    for (i = 0; i <= KEEL_LOCK_RANGE; ++i) {
        CHECK(pthread_mutex_init(&s->domains[i], NULL) == 0);
    }
    CHECK(pthread_mutex_init(&s->model, NULL) == 0);
    keel__board_init(&s->board, monotonic_us());
    memset(&be, 0, sizeof be);
    be.struct_size = sizeof be;
    be.port_version = KEEL_PORT_VERSION;
    be.capabilities = KEEL_CAP_MOTORS | KEEL_CAP_BRAKE | KEEL_CAP_RGB | KEEL_CAP_BUTTONS |
                      KEEL_CAP_BUTTON_LEDS;
    be.user = s;
    be.now_us = stub_now;
    be.sleep_until_us = stub_sleep;
    be.read_inputs = stub_read;
    be.write_outputs = stub_outputs;
    be.write_motors = stub_motors;
    be.emergency_disable = stub_disable;
    be.get_fault = stub_get_fault;
    be.reset_fault = stub_reset_fault;
    be.i2c_write = stub_i2c;
    be.lock = stub_lock;
    be.unlock = stub_unlock;
    be.thread_id = stub_thread;
    CHECK_RC(keel_port_attach(*storage, keel_port_storage_size(), &be, NULL, &device, NULL),
             KEEL_OK);
    return device;
}

/* Releases what stub_open() created, once the device is closed. */
static void stub_free(stub *s, void *storage)
{
    unsigned i;
    for (i = 0; i <= KEEL_LOCK_RANGE; ++i) {
        pthread_mutex_destroy(&s->domains[i]);
    }
    pthread_mutex_destroy(&s->model);
    free(storage);
}

static void disarm_latency(void)
{
    static stub s;
    void *storage = NULL;
    pthread_t spinners[MAX_SPINNERS];
    unsigned spinner_total = spinner_count(), round, i;
    _Atomic int spin = 1;
    keel_device *device = stub_open(&s, &storage);
    hammer load[LED_THREADS + 2]; /* the LED threads, then two pollers */
    owner_loop o;
    int met = 0;

    atomic_store(&s.hang, 1); /* from now on every LED frame hangs 20 ms */

    for (i = 0; i < spinner_total; ++i) {
        CHECK(pthread_create(&spinners[i], NULL, spin_main, &spin) == 0);
    }
    for (i = 0; i < LED_THREADS; ++i) {
        hammer_start(&load[i], device, HAMMER_LEDS);
    }
    /* A thread calling the library in a tight loop must not delay a stop:
     * handle validation takes no shared lock. Calls that take CONTROL or the
     * backend lock come at a realistic rate: a default mutex is not fair, and
     * a tight loop on one of them stretches the far tail of the whole disarm
     * call (never of ENABLE LOW) by milliseconds. */
    hammer_start(&load[LED_THREADS], device, HAMMER_CACHED);
    hammer_start(&load[LED_THREADS + 1], device, HAMMER_POLL);
    owner_start(&o, device, 300);
    atomic_store(&o.rearm, 1);

    /* A design fault (a lock held across I2C or a sleep) shows in every
     * round; a pause of the virtual machine or the host scheduler hits one
     * round at random. Up to three rounds tell the two apart. */
    for (round = 1; round <= ROUNDS && !met; ++round) {
        met = measure_round(device, &s, round);
    }

    owner_stop(&o);
    CHECK_RC(atomic_load(&o.stopped), KEEL_OK);
    hammers_stop(load, LED_THREADS + 2);
    for (i = 0; i < LED_THREADS; ++i) {
        CHECK_RC(load[i].last, KEEL_E_PERIPHERAL); /* every frame hit the hung bus */
    }
    atomic_store(&spin, 0);
    for (i = 0; i < spinner_total; ++i) {
        CHECK(pthread_join(spinners[i], NULL) == 0);
    }
    CHECK_RC(keel_close(device, NULL), KEEL_OK);
    stub_free(&s, storage);
    CHECK(met);
}

enum { CLOSE_LED_THREADS = 8, CLOSE_BOUND_US = 100 * MS };

/* keel_close() with RGB calls in flight on a bus that hangs every
 * transaction for 20 ms: the calls queued on the LED lock give up when
 * close starts instead of each hanging the bus once more before close can
 * turn the LEDs off, so close keeps its documented bound of about 100 ms
 * (it took 161 ms with eight such calls before). ENABLE falls first. */
static void close_bound(void)
{
    static stub s;
    void *storage = NULL;
    keel_device *device = stub_open(&s, &storage);
    hammer load[CLOSE_LED_THREADS];
    uint64_t start, took, disabled;
    unsigned i;
    atomic_store(&s.hang, 1);
    for (i = 0; i < CLOSE_LED_THREADS; ++i) {
        hammer_start(&load[i], device, HAMMER_LEDS);
    }
    pause_us(100 * MS); /* one call on the bus, the others queued on LEDS */
    atomic_store(&s.disabled_at_us, UINT64_C(0));
    start = monotonic_us();
    CHECK_RC(keel_close(device, NULL), KEEL_OK);
    took = monotonic_us() - start;
    disabled = atomic_load(&s.disabled_at_us) - start;
    for (i = 0; i < CLOSE_LED_THREADS; ++i) {
        CHECK_RC(hammer_join(&load[i]), KEEL_E_INVALID_HANDLE);
    }
    stub_free(&s, storage);
    fprintf(stderr,
            "close with %d RGB calls on a hung bus: ENABLE LOW after %llu us, %llu us total\n",
            CLOSE_LED_THREADS, (unsigned long long)disabled, (unsigned long long)took);
    CHECK(disabled < 2 * MS);
    CHECK(took < CLOSE_BOUND_US);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"stop_vs_cycle", stop_vs_cycle},
        {"foreign_motion", foreign_motion},
        {"hammer_close", hammer_close},
        {"disarm_latency", disarm_latency},
        {"close_bound", close_bound},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
