/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Handle registry (SPEC section 6), through the public API on simulator
 * devices: stale, double-closed and forged handles, slot exhaustion, close
 * racing calls on other threads (run under ThreadSanitizer by the tsan
 * preset), and fork.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"

static keel_device *open_sim(uint32_t flags)
{
    keel_open_options options;
    keel_device *device = NULL;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    options.flags = flags;
    CHECK_RC(keel_open(&options, &device, NULL), KEEL_OK);
    CHECK(device != NULL);
    return device;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void pause_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void stale_handle(void)
{
    keel_device *first = open_sim(KEEL_OPEN_VIRTUAL_CLOCK);
    keel_device *second;
    uint32_t state = 0;
    CHECK_RC(keel_close(first, NULL), KEEL_OK);
    CHECK_RC(keel_get_state(first, &state), KEEL_E_INVALID_HANDLE);
    second = open_sim(KEEL_OPEN_VIRTUAL_CLOCK);
    CHECK(second != first); /* the slot is reused under a new generation */
    CHECK_RC(keel_get_state(first, &state), KEEL_E_INVALID_HANDLE);
    CHECK_RC(keel_tick(first, 1, KEEL_HEALTH_ALL), KEEL_E_INVALID_HANDLE);
    CHECK_RC(keel_disarm(first), KEEL_E_INVALID_HANDLE);
    CHECK_RC(keel_get_state(second, &state), KEEL_OK);
    CHECK_RC(keel_close(second, NULL), KEEL_OK);
}

static void double_close(void)
{
    keel_device *device = open_sim(KEEL_OPEN_VIRTUAL_CLOCK);
    keel_fault fault;
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    CHECK_RC(keel_close(device, &fault), KEEL_OK);
    CHECK_RC(fault.status, KEEL_OK);
    CHECK_RC(fault.cleanup_status, KEEL_OK);
    CHECK_RC(keel_close(device, &fault), KEEL_E_INVALID_HANDLE);
    CHECK_RC(keel_close(NULL, NULL), KEEL_OK);
}

static void forged_handles(void)
{
    keel_device *real = open_sim(KEEL_OPEN_VIRTUAL_CLOCK);
    uintptr_t token = (uintptr_t)real;
    int on_stack = 0;
    keel_device *forged[7];
    uint32_t state = 0;
    size_t i;
    forged[0] = NULL;
    forged[1] = (keel_device *)(uintptr_t)1;    /* generation 0 never exists */
    forged[2] = (keel_device *)(uintptr_t)0x41; /* slot 65: outside the table */
    forged[3] = (keel_device *)(uintptr_t)0xFF;
    forged[4] = (keel_device *)(token + ((uintptr_t)1 << 8)); /* next generation */
    forged[5] = (keel_device *)(token ^ 0x3u);                 /* another slot */
    forged[6] = (keel_device *)(void *)&on_stack;              /* a real address */
    for (i = 0; i < sizeof forged / sizeof forged[0]; ++i) {
        CHECK_RC(keel_get_state(forged[i], &state), KEEL_E_INVALID_HANDLE);
        CHECK_RC(keel_set_motors(forged[i], 100, 100), KEEL_E_INVALID_HANDLE);
        CHECK_RC(keel_emergency_stop(forged[i]), KEEL_E_INVALID_HANDLE);
        if (forged[i]) {
            CHECK_RC(keel_close(forged[i], NULL), KEEL_E_INVALID_HANDLE);
        }
    }
    CHECK_RC(keel_get_state(real, &state), KEEL_OK);
    CHECK_EQ(state, KEEL_STATE_DISARMED);
    CHECK_RC(keel_close(real, NULL), KEEL_OK);
}

static void slot_exhaustion(void)
{
    keel_device *devices[64];
    keel_device *extra = (keel_device *)(uintptr_t)0x1234;
    keel_open_options options;
    keel_open_report report;
    size_t i;
    for (i = 0; i < 64; ++i) {
        devices[i] = open_sim(KEEL_OPEN_VIRTUAL_CLOCK);
    }
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    options.flags = KEEL_OPEN_VIRTUAL_CLOCK;
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    CHECK_RC(keel_open(&options, &extra, &report), KEEL_E_NO_MEMORY);
    CHECK(extra == NULL);
    CHECK_RC(report.status, KEEL_E_NO_MEMORY);
    CHECK_EQ(report.resource, KEEL_RESOURCE_MEMORY);
    CHECK(report.hint[0] != '\0');
    for (i = 0; i < 64; ++i) {
        CHECK_RC(keel_close(devices[i], NULL), KEEL_OK);
    }
    extra = open_sim(KEEL_OPEN_VIRTUAL_CLOCK);
    CHECK_RC(keel_close(extra, NULL), KEEL_OK);
}

/* ---- close racing calls on other threads -------------------------------- */

enum { HAMMER_KINDS = 7 };

typedef struct race {
    keel_device *device;
    atomic_int stop;
    atomic_int unexpected; /* first status outside {OK, INVALID_HANDLE} */
    atomic_int sleeper_rc;
    _Atomic uint64_t sleeper_woke_us;
} race;

typedef struct worker {
    race *race;
    int kind;
} worker;

static int32_t hammer_once(keel_device *device, int kind, uint64_t *cycle)
{
    keel_buttons buttons;
    keel_snapshot snapshot;
    uint64_t now = 0;
    uint32_t state = 0;
    memset(&buttons, 0, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.struct_size = sizeof snapshot;
    switch (kind) {
    case 0:
        return keel_get_state(device, &state);
    case 1:
        return keel_take_buttons(device, &buttons);
    case 2:
        return keel_set_rgb_all(device, (uint32_t)*cycle & 0xFFFFFFu);
    case 3:
        return keel_disarm(device);
    case 4:
        return keel_get_snapshot(device, &snapshot);
    case 5:
        return keel_set_motors(device, 0, 0);
    default: {
        /* The control loop: tick, then sleep on the device clock. */
        int32_t rc = keel_tick(device, ++*cycle, KEEL_HEALTH_ALL);
        if (rc == KEEL_OK) {
            rc = keel_now_us(device, &now);
        }
        return rc == KEEL_OK ? keel_sleep_until(device, now + 10000) : rc;
    }
    }
}

static void *hammer(void *argument)
{
    worker *w = argument;
    uint64_t cycle = 0;
    while (!atomic_load(&w->race->stop)) {
        int32_t rc = hammer_once(w->race->device, w->kind, &cycle);
        if (rc != KEEL_OK && rc != KEEL_E_INVALID_HANDLE) {
            int expected = 0;
            atomic_compare_exchange_strong(&w->race->unexpected, &expected, (int)rc);
        }
    }
    return NULL;
}

static void *long_sleeper(void *argument)
{
    race *r = argument;
    uint64_t now = 0;
    int32_t rc = keel_now_us(r->device, &now);
    if (rc == KEEL_OK) {
        rc = keel_sleep_until(r->device, now + 5000000u);
    }
    atomic_store(&r->sleeper_woke_us, monotonic_us());
    atomic_store(&r->sleeper_rc, (int)rc);
    return NULL;
}

/* Every call racing keel_close() returns OK or INVALID_HANDLE, never touches
 * freed memory, and close wakes a five-second sleeper at once. The device
 * stays DISARMED (no arming), so a slow sanitizer build cannot fault it. */
static void close_vs_call(void)
{
    race r;
    worker workers[HAMMER_KINDS];
    pthread_t threads[HAMMER_KINDS + 1];
    uint64_t close_started, close_took;
    int i;
    memset(&r, 0, sizeof r);
    r.device = open_sim(0);
    atomic_init(&r.sleeper_rc, 1);
    for (i = 0; i < HAMMER_KINDS; ++i) {
        workers[i].race = &r;
        workers[i].kind = i;
        CHECK(pthread_create(&threads[i], NULL, hammer, &workers[i]) == 0);
    }
    CHECK(pthread_create(&threads[HAMMER_KINDS], NULL, long_sleeper, &r) == 0);
    pause_ms(150);
    close_started = monotonic_us();
    CHECK_RC(keel_close(r.device, NULL), KEEL_OK);
    close_took = monotonic_us() - close_started;
    atomic_store(&r.stop, 1);
    for (i = 0; i <= HAMMER_KINDS; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    if (atomic_load(&r.unexpected) != 0) {
        fprintf(stderr, "racing call returned %s\n", keel_status_name(atomic_load(&r.unexpected)));
    }
    CHECK(atomic_load(&r.unexpected) == 0);
    CHECK_RC(atomic_load(&r.sleeper_rc), KEEL_E_INVALID_HANDLE);
    /* Close is bounded by about 100 ms; sanitizers get a generous margin. */
    CHECK(close_took < 2000000u);
    CHECK(atomic_load(&r.sleeper_woke_us) - close_started < 2000000u);
}

/* ---- fork ------------------------------------------------------------------ */

static void *fork_hammer(void *argument)
{
    race *r = argument;
    uint32_t state = 0;
    while (!atomic_load(&r->stop)) {
        if (keel_get_state(r->device, &state) != KEEL_OK) {
            atomic_store(&r->unexpected, 1);
        }
    }
    return NULL;
}

/* A child forked while another thread hammers the device gets NOT_OWNER from
 * every call within 100 ms and touches nothing; the parent is unaffected. */
static void fork_child(void)
{
    race r;
    pthread_t thread;
    uint32_t state = 0;
    pid_t child;
    int status = 0;
    memset(&r, 0, sizeof r);
    r.device = open_sim(0);
    CHECK(pthread_create(&thread, NULL, fork_hammer, &r) == 0);
    pause_ms(20);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        uint64_t started = monotonic_us();
        int ok = keel_get_state(r.device, &state) == KEEL_E_NOT_OWNER &&
                 keel_tick(r.device, 1, KEEL_HEALTH_ALL) == KEEL_E_NOT_OWNER &&
                 keel_disarm(r.device) == KEEL_E_NOT_OWNER &&
                 keel_set_rgb_all(r.device, 0xFF0000) == KEEL_E_NOT_OWNER &&
                 keel_close(r.device, NULL) == KEEL_E_NOT_OWNER;
        ok = ok && monotonic_us() - started < 100000u;
        _exit(ok ? 0 : 1);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    atomic_store(&r.stop, 1);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(atomic_load(&r.unexpected) == 0);
    CHECK_RC(keel_get_state(r.device, &state), KEEL_OK);
    CHECK_RC(keel_close(r.device, NULL), KEEL_OK);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"stale_handle", stale_handle},
        {"double_close", double_close},
        {"forged_handles", forged_handles},
        {"slot_exhaustion", slot_exhaustion},
        {"close_vs_call", close_vs_call},
        {"fork_child", fork_child},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
