/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file fake_syscalls.h
 * @brief Renames the system calls of code included after it to the fake
 *        kernel (fake_kernel.h). Include every system header the code uses
 *        first: their include guards then keep the real declarations intact,
 *        and only the calls in the included code are redirected.
 */
#ifndef KEEL_TEST_FAKE_SYSCALLS_H
#define KEEL_TEST_FAKE_SYSCALLS_H

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "fake_kernel.h"

/** @brief open(2) goes to the fake. */
#define open keel_fake_open
/** @brief close(2) goes to the fake. */
#define close keel_fake_close
/** @brief ioctl(2) goes to the fake. */
#define ioctl keel_fake_ioctl
/** @brief fcntl(2) goes to the fake. */
#define fcntl keel_fake_fcntl
/** @brief read(2) goes to the fake. */
#define read keel_fake_read
/** @brief clock_gettime(2) goes to the fake. */
#define clock_gettime keel_fake_clock_gettime
/** @brief pthread_create(3) goes to the fake. */
#define pthread_create keel_fake_pthread_create

#endif /* KEEL_TEST_FAKE_SYSCALLS_H */
