/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Single-threaded OS layer for ports without an operating system (bare
 * metal, a single RTOS task). There is one process and one thread, so the
 * registry needs no lock and close never waits: the only reference to a
 * device during keel_close() is the closing call itself.
 */
#include "keel/keel.h"
#include "os/os.h"

void keel__os_init(void)
{
}

void keel__os_lock(void)
{
}

void keel__os_unlock(void)
{
}

void keel__os_wait(void)
{
}

void keel__os_wake(void)
{
}

int32_t keel__os_pid(void)
{
    return 1;
}

uint32_t keel__os_backend_env(void)
{
    return KEEL_BACKEND_AUTO;
}
