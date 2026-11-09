/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file fake_kernel.h
 * @brief A fake of the kernel interfaces the keel_r3 backend uses: the GPIO
 *        character device (chip info, line info, v2 line requests, values,
 *        edge events), i2c-dev, fcntl, the monotonic clock and thread
 *        creation, plus a minimal MCU model that answers the arming
 *        acknowledgement.
 *
 * The backend sources are compiled into the test program with their system
 * calls renamed to the keel_fake_* functions below (fake_syscalls.h). The
 * fake records every request and write and
 * flags contract violations (a forbidden line, a missing O_CLOEXEC, a
 * blocking read) instead of trusting the backend.
 *
 * All state is behind one mutex, so the PWM worker and the test thread may
 * use the fake concurrently; tests read it through fake_lock()/fake_unlock()
 * or the accessors.
 */
#ifndef KEEL_TEST_FAKE_KERNEL_H
#define KEEL_TEST_FAKE_KERNEL_H

#include <linux/gpio.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/** @brief Fake descriptors start here, far from real ones. */
#define FAKE_FD_BASE 1000
/** @brief Size of the fake descriptor table. */
#define FAKE_FDS 64
/** @brief Chips the fake can expose. */
#define FAKE_CHIPS 4
/** @brief Line requests remembered. */
#define FAKE_REQUESTS 16
/** @brief Line writes remembered. */
#define FAKE_LOG 8192
/** @brief Pending ECHO events. */
#define FAKE_EVENTS 64

/** @brief What a line request is for, from its first line. */
enum fake_role {
    FAKE_ROLE_CONTROL, /**< 26, 18, 19, 23, 22, 17, 27. */
    FAKE_ROLE_MOTOR,   /**< 8, 11, 10, 9. */
    FAKE_ROLE_INPUT,   /**< 20, 21, 5, 6, 16, 24. */
    FAKE_ROLE_RANGE,   /**< 13, 25. */
    FAKE_ROLE_COUNT    /**< Number of roles. */
};

/** @brief How keel_fake_pthread_create() treats a SCHED_FIFO request. */
enum fake_threads {
    FAKE_THREADS_FIFO_EPERM,   /**< SCHED_FIFO is refused with EPERM (an unprivileged user). */
    FAKE_THREADS_FIFO_GRANTED, /**< SCHED_FIFO "succeeds" (the thread really runs SCHED_OTHER). */
    FAKE_THREADS_FAIL          /**< Every creation fails with EAGAIN. */
};

/** @brief One fake GPIO chip. */
typedef struct fake_chip {
    char path[64];   /**< Device node, e.g. "/dev/gpiochip0". */
    char label[32];  /**< GPIO_GET_CHIPINFO label. */
    uint32_t lines;  /**< Number of lines. */
    int open_errno;  /**< errno for open(), or 0. */
    int info_errno;  /**< errno for GPIO_GET_CHIPINFO_IOCTL, or 0. */
} fake_chip;

/** @brief One recorded line write. */
typedef struct fake_write {
    int role;        /**< ::fake_role. */
    uint64_t mask;   /**< Request bits written. */
    uint64_t bits;   /**< Their levels. */
    uint64_t time_us; /**< Real monotonic time of the write. */
} fake_write;

/** @brief The whole fake. Read and change it under fake_lock(). */
typedef struct fake_kernel {
    fake_chip chips[FAKE_CHIPS];      /**< Chips by path. */
    unsigned chip_count;              /**< Chips in use. */
    int line_used[64];                /**< Line flagged USED by someone else. */
    int line_held[64];                /**< Line held by a live fake request. */
    char line_consumer[64][32];       /**< Its consumer label. */
    int lineinfo_errno;               /**< errno for GPIO_V2_GET_LINEINFO_IOCTL, or 0. */
    uint64_t queried;                 /**< Lines whose info was asked for. */

    int request_errno;                /**< errno for request number @c request_fail_at. */
    int request_fail_at;              /**< Request index that fails, or -1. */
    struct gpio_v2_line_request requests[FAKE_REQUESTS]; /**< Requests made. */
    int request_role[FAKE_REQUESTS];  /**< ::fake_role of each. */
    unsigned request_count;           /**< Requests made. */
    int role_fd[FAKE_ROLE_COUNT];     /**< Live descriptor of each role, or -1. */

    int fd_open[FAKE_FDS];            /**< Descriptor is open. */
    int fd_kind[FAKE_FDS];            /**< 1 chip, 2 request, 3 I2C. */
    int fd_index[FAKE_FDS];           /**< Chip or request index. */
    int fd_flags[FAKE_FDS];           /**< F_SETFL flags. */
    unsigned opened;                  /**< Descriptors handed out. */
    unsigned closed;                  /**< Descriptors closed. */

    uint64_t values[FAKE_ROLE_COUNT]; /**< Output levels, in request bit order. */
    int set_errno[FAKE_ROLE_COUNT];   /**< errno for the next @c set_fails writes. */
    unsigned set_fails[FAKE_ROLE_COUNT]; /**< Writes still to fail. */
    int get_errno[FAKE_ROLE_COUNT];   /**< errno for the next @c get_fails reads. */
    unsigned get_fails[FAKE_ROLE_COUNT]; /**< Reads still to fail. */
    unsigned set_delay_ns;            /**< Busy time added to every write (lock contention). */
    fake_write log[FAKE_LOG];         /**< Writes, oldest first (drops when full). */
    unsigned log_count;               /**< Writes recorded. */

    int ready;                        /**< READY level. */
    int armed_force;                  /**< -1: MCU model; 0 or 1: ARMED forced. */
    uint32_t buttons;                 /**< Pressed buttons (logical, A = 1). */
    int echo;                         /**< ECHO level. */
    int ack_stage;                    /**< MCU model: 0 idle, 1 HIGH1, 2 LOW2, 3 HIGH2. */
    uint64_t enable_low_since_us;     /**< MCU model: ENABLE LOW since. */
    uint64_t high2_at_us;             /**< MCU model: HIGH2 began. */

    char i2c_path[64];                /**< Path of the I2C adapter. */
    int i2c_open_errno;               /**< errno for its open(), or 0. */
    int i2c_timeout_errno;            /**< errno for I2C_TIMEOUT, or 0. */
    unsigned long i2c_timeout;        /**< Value given to I2C_TIMEOUT. */
    int i2c_fail;                     /**< I2C_RDWR fails (EREMOTEIO). */
    unsigned i2c_transfers;           /**< I2C_RDWR calls. */
    uint16_t i2c_addr;                /**< Last message address. */
    uint16_t i2c_len;                 /**< Last message length. */
    uint8_t i2c_data[64];             /**< Last message bytes. */
    uint32_t i2c_nmsgs;               /**< Last nmsgs. */

    struct gpio_v2_line_event events[FAKE_EVENTS]; /**< Pending ECHO events. */
    unsigned event_count;             /**< Events pending. */

    int clock_frozen;                 /**< CLOCK_MONOTONIC reads @c frozen_us. */
    uint64_t frozen_us;               /**< The frozen time. */

    int thread_mode;                  /**< ::fake_threads. */
    unsigned fifo_attempts;           /**< SCHED_FIFO creations asked for. */
    unsigned creations;               /**< Successful creations. */
    int last_policy;                  /**< Policy of the last creation. */
    int last_priority;                /**< Its priority. */
    int last_inherit;                 /**< Its inherit-scheduler setting. */
    size_t last_stack;                /**< Its stack size. */
    int sigint_blocked;               /**< SIGINT was blocked during the last creation. */
    int sigsegv_blocked;              /**< SIGSEGV was blocked during the last creation. */

    const char *violation;            /**< First contract violation, or NULL. */
} fake_kernel;

/** @brief The fake's state. */
extern fake_kernel keel_fake;

/** @brief Restores the default fake: one BCM2711 chip at /dev/gpiochip0, a
 *  second chip labelled "raspberrypi-exp-gpio", /dev/i2c-1, READY HIGH,
 *  the MCU model for ARMED, SCHED_FIFO refused, the real clock. */
void fake_reset(void);
/** @brief Takes the fake's lock. */
void fake_lock(void);
/** @brief Releases the fake's lock. */
void fake_unlock(void);
/** @brief Real CLOCK_MONOTONIC, us. @return Time. */
uint64_t fake_real_now_us(void);
/** @brief Descriptors handed out and not closed. @return Count. */
unsigned fake_open_fds(void);
/** @brief Output levels of a role. @param[in] role ::fake_role. @return Levels. */
uint64_t fake_values(int role);
/** @brief Queues one ECHO event. @param[in] time_ns Kernel timestamp.
 *  @param[in] rising Rising edge. @param[in] line_seqno Line sequence number. */
void fake_push_event(uint64_t time_ns, int rising, uint32_t line_seqno);
/** @brief Sets the frozen clock. @param[in] us Time; 0 returns to the real clock. */
void fake_freeze_clock(uint64_t us);

/** @brief open(2). @param[in] path Path. @param[in] flags Flags. @return fd or -1. */
int keel_fake_open(const char *path, int flags, ...);
/** @brief close(2). @param[in] fd Descriptor. @return 0 or -1. */
int keel_fake_close(int fd);
/** @brief ioctl(2). @param[in] fd Descriptor. @param[in] request Request. @return Result. */
int keel_fake_ioctl(int fd, unsigned long request, ...);
/** @brief fcntl(2). @param[in] fd Descriptor. @param[in] command Command. @return Result. */
int keel_fake_fcntl(int fd, int command, ...);
/** @brief read(2). @param[in] fd Descriptor. @param[out] buffer Buffer.
 *  @param[in] count Size. @return Bytes or -1. */
ssize_t keel_fake_read(int fd, void *buffer, size_t count);
/** @brief clock_gettime(2). @param[in] clock Clock. @param[out] ts Time. @return 0. */
int keel_fake_clock_gettime(clockid_t clock, struct timespec *ts);
/** @brief pthread_create(3), see ::fake_threads. @param[out] thread Thread.
 *  @param[in] attr Attributes. @param[in] start Routine. @param[in] arg Argument.
 *  @return 0 or an error. */
int keel_fake_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                             void *(*start)(void *), void *arg);

#endif /* KEEL_TEST_FAKE_KERNEL_H */
