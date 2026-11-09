/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file os.h
 * @brief The OS services behind the handle registry: its lock, the wait for
 *        in-flight calls during close, the cached process id with fork
 *        handling, and backend selection from the environment.
 *
 * os_posix.c implements it with pthreads; os_none.c is the single-threaded
 * variant for ports without an OS.
 */
#ifndef KEEL_OS_H
#define KEEL_OS_H

#include <stdint.h>

/** @brief One-time setup (registers the fork handler); idempotent and thread-safe. */
void keel__os_init(void);
/** @brief Takes the registry lock. */
void keel__os_lock(void);
/** @brief Releases the registry lock. */
void keel__os_unlock(void);
/** @brief Waits, with the registry lock held, until keel__os_wake() is called. */
void keel__os_wait(void);
/** @brief Wakes every keel__os_wait() caller (registry lock held). */
void keel__os_wake(void);
/** @brief Process id cached by keel__os_init() and refreshed in a forked
 *  child; reading it makes no system call. 0 before initialisation. */
int32_t keel__os_pid(void);
/** @brief Backend named by the KEEL_BACKEND environment variable:
 *  ::KEEL_BACKEND_AUTO when unset or empty, ::KEEL_BACKEND_R3 for "r3",
 *  ::KEEL_BACKEND_SIMULATOR for "simulator", UINT32_MAX otherwise. */
uint32_t keel__os_backend_env(void);

#endif /* KEEL_OS_H */
