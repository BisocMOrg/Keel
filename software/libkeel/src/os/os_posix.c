/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * POSIX OS layer: registry mutex and condition variable, cached process id
 * and the pthread_atfork handlers that make every inherited handle foreign
 * in a forked child (SPEC section 6).
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "keel/keel.h"
#include "os/os.h"

static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t registry_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static _Atomic int32_t cached_pid;

/* Holding the registry lock across fork() means the child never inherits it
 * mid-update; the forking thread releases it on both sides. */
static void before_fork(void)
{
    pthread_mutex_lock(&registry_mutex);
}

static void after_fork_parent(void)
{
    pthread_mutex_unlock(&registry_mutex);
}

/* The child keeps the parent's entries, whose owner pid no longer matches
 * the refreshed cached pid, so every inherited handle reports NOT_OWNER. The
 * condition variable may record waiters from threads that do not exist in
 * the child, hence the fresh one. */
static void after_fork_child(void)
{
    pthread_mutex_unlock(&registry_mutex);
    pthread_cond_init(&registry_cond, NULL);
    atomic_store(&cached_pid, (int32_t)getpid());
}

static void init_process(void)
{
    atomic_store(&cached_pid, (int32_t)getpid());
    pthread_atfork(before_fork, after_fork_parent, after_fork_child);
}

void keel__os_init(void)
{
    pthread_once(&init_once, init_process);
}

void keel__os_lock(void)
{
    pthread_mutex_lock(&registry_mutex);
}

void keel__os_unlock(void)
{
    pthread_mutex_unlock(&registry_mutex);
}

void keel__os_wait(void)
{
    pthread_cond_wait(&registry_cond, &registry_mutex);
}

void keel__os_wake(void)
{
    pthread_cond_broadcast(&registry_cond);
}

int32_t keel__os_pid(void)
{
    return atomic_load(&cached_pid);
}

uint32_t keel__os_backend_env(void)
{
    const char *value = getenv("KEEL_BACKEND");
    if (!value || !*value) {
        return KEEL_BACKEND_AUTO;
    }
    if (strcmp(value, "r3") == 0) {
        return KEEL_BACKEND_R3;
    }
    if (strcmp(value, "simulator") == 0) {
        return KEEL_BACKEND_SIMULATOR;
    }
    return UINT32_MAX;
}
