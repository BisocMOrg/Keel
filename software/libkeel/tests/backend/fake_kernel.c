/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/* The fake kernel behind the keel_r3 backend tests (see fake_kernel.h). */
#define _GNU_SOURCE

#include "fake_kernel.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>

enum { KIND_CHIP = 1, KIND_REQUEST = 2, KIND_I2C = 3 };

fake_kernel keel_fake;
static pthread_mutex_t fake_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Lines the backend must never request or inspect: SWD/NRST, UART, I2C_INT
 * and the J5 servo signal. */
static int forbidden(uint32_t line)
{
    return line == 0 || line == 1 || line == 4 || line == 7 || line == 12 || line == 14 ||
           line == 15;
}

static void violation(const char *what)
{
    if (!keel_fake.violation) {
        keel_fake.violation = what;
    }
}

uint64_t fake_real_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000u;
}

void fake_lock(void)
{
    pthread_mutex_lock(&fake_mutex);
}

void fake_unlock(void)
{
    pthread_mutex_unlock(&fake_mutex);
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

static void add_chip(const char *path, const char *label, uint32_t lines)
{
    fake_chip *chip = &keel_fake.chips[keel_fake.chip_count++];
    copy_text(chip->path, sizeof chip->path, path);
    copy_text(chip->label, sizeof chip->label, label);
    chip->lines = lines;
}

void fake_reset(void)
{
    int role;
    fake_lock();
    memset(&keel_fake, 0, sizeof keel_fake);
    add_chip("/dev/gpiochip0", "pinctrl-bcm2711", 58);
    add_chip("/dev/gpiochip1", "raspberrypi-exp-gpio", 8);
    keel_fake.request_fail_at = -1;
    for (role = 0; role < FAKE_ROLE_COUNT; ++role) {
        keel_fake.role_fd[role] = -1;
    }
    copy_text(keel_fake.i2c_path, sizeof keel_fake.i2c_path, "/dev/i2c-1");
    keel_fake.ready = 1;
    keel_fake.armed_force = -1;
    keel_fake.enable_low_since_us = fake_real_now_us();
    keel_fake.thread_mode = FAKE_THREADS_FIFO_EPERM;
    fake_unlock();
}

unsigned fake_open_fds(void)
{
    unsigned count;
    fake_lock();
    count = keel_fake.opened - keel_fake.closed;
    fake_unlock();
    return count;
}

uint64_t fake_values(int role)
{
    uint64_t values;
    fake_lock();
    values = keel_fake.values[role];
    fake_unlock();
    return values;
}

void fake_push_event(uint64_t time_ns, int rising, uint32_t line_seqno)
{
    struct gpio_v2_line_event *event;
    fake_lock();
    event = &keel_fake.events[keel_fake.event_count++];
    memset(event, 0, sizeof *event);
    event->timestamp_ns = time_ns;
    event->id = rising ? GPIO_V2_LINE_EVENT_RISING_EDGE : GPIO_V2_LINE_EVENT_FALLING_EDGE;
    event->offset = 25;
    event->seqno = line_seqno;
    event->line_seqno = line_seqno;
    fake_unlock();
}

void fake_freeze_clock(uint64_t us)
{
    fake_lock();
    keel_fake.clock_frozen = us != 0;
    keel_fake.frozen_us = us;
    fake_unlock();
}

static int new_fd(int kind, int index)
{
    int i;
    for (i = 0; i < FAKE_FDS; ++i) {
        if (!keel_fake.fd_open[i]) {
            keel_fake.fd_open[i] = 1;
            keel_fake.fd_kind[i] = kind;
            keel_fake.fd_index[i] = index;
            keel_fake.fd_flags[i] = 0;
            keel_fake.opened++;
            return FAKE_FD_BASE + i;
        }
    }
    violation("fake descriptor table full");
    return -1;
}

static int slot_of(int fd)
{
    int slot = fd - FAKE_FD_BASE;
    return slot >= 0 && slot < FAKE_FDS && keel_fake.fd_open[slot] ? slot : -1;
}

static int fail(int error)
{
    errno = error;
    return -1;
}

int keel_fake_open(const char *path, int flags, ...)
{
    unsigned i;
    int fd;
    fake_lock();
    if (!(flags & O_CLOEXEC)) {
        violation("open without O_CLOEXEC");
    }
    for (i = 0; i < keel_fake.chip_count; ++i) {
        if (strcmp(path, keel_fake.chips[i].path) == 0) {
            if (keel_fake.chips[i].open_errno) {
                fd = fail(keel_fake.chips[i].open_errno);
            } else {
                fd = new_fd(KIND_CHIP, (int)i);
            }
            fake_unlock();
            return fd;
        }
    }
    if (strcmp(path, keel_fake.i2c_path) == 0) {
        fd = keel_fake.i2c_open_errno ? fail(keel_fake.i2c_open_errno) : new_fd(KIND_I2C, 0);
    } else {
        fd = fail(ENOENT);
    }
    fake_unlock();
    return fd;
}

int keel_fake_close(int fd)
{
    int slot, rc = 0;
    fake_lock();
    slot = slot_of(fd);
    if (slot < 0) {
        violation("close of a descriptor that is not open");
        rc = fail(EBADF);
    } else {
        if (keel_fake.fd_kind[slot] == KIND_REQUEST) {
            int index = keel_fake.fd_index[slot];
            unsigned i;
            if (keel_fake.role_fd[keel_fake.request_role[index]] == fd) {
                keel_fake.role_fd[keel_fake.request_role[index]] = -1;
            }
            for (i = 0; i < keel_fake.requests[index].num_lines; ++i) {
                keel_fake.line_held[keel_fake.requests[index].offsets[i]] = 0;
            }
        }
        keel_fake.fd_open[slot] = 0;
        keel_fake.closed++;
    }
    fake_unlock();
    return rc;
}

/* ---- the MCU model: ARMED follows a complete acknowledgement --------------- */

static void enable_edge(int high, uint64_t now)
{
    if (high) {
        if (keel_fake.ack_stage == 0 && now - keel_fake.enable_low_since_us >= 100000u) {
            keel_fake.ack_stage = 1;
        } else if (keel_fake.ack_stage == 2) {
            keel_fake.ack_stage = 3;
            keel_fake.high2_at_us = now;
        }
    } else if (keel_fake.ack_stage == 1) {
        keel_fake.ack_stage = 2;
    } else {
        keel_fake.ack_stage = 0;
        keel_fake.enable_low_since_us = now;
    }
}

static int armed_level(uint64_t now)
{
    if (keel_fake.armed_force >= 0) {
        return keel_fake.armed_force;
    }
    return keel_fake.ack_stage == 3 && now - keel_fake.high2_at_us >= 100000u &&
           (keel_fake.values[FAKE_ROLE_CONTROL] & 1u);
}

/* ---- ioctl ---------------------------------------------------------------- */

static int role_of(const struct gpio_v2_line_request *request)
{
    switch (request->offsets[0]) {
    case 26:
        return FAKE_ROLE_CONTROL;
    case 8:
        return FAKE_ROLE_MOTOR;
    case 20:
        return FAKE_ROLE_INPUT;
    case 13:
        return FAKE_ROLE_RANGE;
    default:
        violation("line request with an unexpected first line");
        return FAKE_ROLE_CONTROL;
    }
}

static int chip_ioctl(int index, unsigned long request, void *arg)
{
    const fake_chip *chip = &keel_fake.chips[index];
    unsigned i;
    if (request == GPIO_GET_CHIPINFO_IOCTL) {
        struct gpiochip_info *info = arg;
        if (chip->info_errno) {
            return fail(chip->info_errno);
        }
        memset(info, 0, sizeof *info);
        copy_text(info->name, sizeof info->name, "gpiochip");
        copy_text(info->label, sizeof info->label, chip->label);
        info->lines = chip->lines;
        return 0;
    }
    if (request == GPIO_V2_GET_LINEINFO_IOCTL) {
        struct gpio_v2_line_info *info = arg;
        uint32_t line = info->offset;
        if (line >= 64) {
            return fail(EINVAL);
        }
        keel_fake.queried |= UINT64_C(1) << line;
        if (forbidden(line)) {
            violation("line info asked for a forbidden line");
        }
        if (keel_fake.lineinfo_errno) {
            return fail(keel_fake.lineinfo_errno);
        }
        memset(info, 0, sizeof *info);
        info->offset = line;
        if (keel_fake.line_held[line]) {
            info->flags = GPIO_V2_LINE_FLAG_USED;
            copy_text(info->consumer, sizeof info->consumer, "keel");
        } else if (keel_fake.line_used[line]) {
            info->flags = GPIO_V2_LINE_FLAG_USED;
            copy_text(info->consumer, sizeof info->consumer, keel_fake.line_consumer[line]);
        }
        return 0;
    }
    if (request == GPIO_V2_GET_LINE_IOCTL) {
        struct gpio_v2_line_request *line_request = arg;
        int role, fd;
        if (strcmp(line_request->consumer, "keel") != 0) {
            violation("line request without the consumer \"keel\"");
        }
        for (i = 0; i < line_request->num_lines; ++i) {
            if (forbidden(line_request->offsets[i])) {
                violation("line request with a forbidden line");
            }
            if (keel_fake.line_used[line_request->offsets[i]] ||
                keel_fake.line_held[line_request->offsets[i]]) {
                return fail(EBUSY);
            }
        }
        if (keel_fake.request_fail_at == (int)keel_fake.request_count) {
            keel_fake.request_count++;
            return fail(keel_fake.request_errno);
        }
        if (keel_fake.request_count >= FAKE_REQUESTS) {
            violation("too many line requests");
            return fail(ENOMEM);
        }
        role = role_of(line_request);
        fd = new_fd(KIND_REQUEST, (int)keel_fake.request_count);
        line_request->fd = fd;
        keel_fake.requests[keel_fake.request_count] = *line_request;
        keel_fake.request_role[keel_fake.request_count] = role;
        keel_fake.request_count++;
        keel_fake.role_fd[role] = fd;
        keel_fake.values[role] = 0;
        for (i = 0; i < line_request->num_lines; ++i) {
            keel_fake.line_held[line_request->offsets[i]] = 1;
        }
        return 0;
    }
    return fail(ENOTTY);
}

static void busy_wait_ns(unsigned ns)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000000000L + (now.tv_nsec - start.tv_nsec) <
             (long)ns);
}

static int request_ioctl(int index, unsigned long request, void *arg)
{
    int role = keel_fake.request_role[index];
    struct gpio_v2_line_values *values = arg;
    uint64_t lines = (UINT64_C(1) << keel_fake.requests[index].num_lines) - 1u;
    uint64_t now = fake_real_now_us();
    if ((request == GPIO_V2_LINE_SET_VALUES_IOCTL || request == GPIO_V2_LINE_GET_VALUES_IOCTL) &&
        !(values->mask & lines)) {
        return fail(EINVAL); /* as the kernel: the mask must select a line */
    }
    if (request == GPIO_V2_LINE_SET_VALUES_IOCTL) {
        uint64_t before = keel_fake.values[role];
        if (keel_fake.set_fails[role]) {
            keel_fake.set_fails[role]--;
            return fail(keel_fake.set_errno[role]);
        }
        if (keel_fake.set_delay_ns) {
            busy_wait_ns(keel_fake.set_delay_ns);
        }
        keel_fake.values[role] = (before & ~values->mask) | (values->bits & values->mask);
        if (keel_fake.log_count < FAKE_LOG) {
            fake_write *entry = &keel_fake.log[keel_fake.log_count++];
            entry->role = role;
            entry->mask = values->mask;
            entry->bits = values->bits;
            entry->time_us = now;
        }
        if (role == FAKE_ROLE_CONTROL && ((before ^ keel_fake.values[role]) & 1u)) {
            enable_edge((int)(keel_fake.values[role] & 1u), now);
        }
        return 0;
    }
    if (request == GPIO_V2_LINE_GET_VALUES_IOCTL) {
        uint64_t bits = 0;
        if (keel_fake.get_fails[role]) {
            keel_fake.get_fails[role]--;
            return fail(keel_fake.get_errno[role]);
        }
        if (role == FAKE_ROLE_INPUT) {
            bits = (keel_fake.ready ? 1u : 0u) | (armed_level(now) ? 2u : 0u) |
                   (uint64_t)(keel_fake.buttons & 0xFu) << 2;
        } else if (role == FAKE_ROLE_RANGE) {
            bits = (keel_fake.values[role] & 1u) | (keel_fake.echo ? 2u : 0u);
        } else {
            bits = keel_fake.values[role];
        }
        values->bits = bits & values->mask;
        return 0;
    }
    return fail(ENOTTY);
}

static int i2c_ioctl(unsigned long request, void *arg, unsigned long value)
{
    if (request == I2C_TIMEOUT) {
        if (keel_fake.i2c_timeout_errno) {
            return fail(keel_fake.i2c_timeout_errno);
        }
        keel_fake.i2c_timeout = value;
        return 0;
    }
    if (request == I2C_RDWR) {
        const struct i2c_rdwr_ioctl_data *transfer = arg;
        keel_fake.i2c_transfers++;
        keel_fake.i2c_nmsgs = transfer->nmsgs;
        if (transfer->nmsgs != 1 || transfer->msgs[0].len > sizeof keel_fake.i2c_data) {
            violation("I2C_RDWR without exactly one message");
            return fail(EINVAL);
        }
        keel_fake.i2c_addr = transfer->msgs[0].addr;
        keel_fake.i2c_len = transfer->msgs[0].len;
        memcpy(keel_fake.i2c_data, transfer->msgs[0].buf, transfer->msgs[0].len);
        if (keel_fake.i2c_fail || transfer->msgs[0].addr != 0x54) {
            return fail(EREMOTEIO); /* no acknowledge */
        }
        return 1;
    }
    return fail(ENOTTY);
}

int keel_fake_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *arg = NULL;
    unsigned long value = 0;
    int slot, rc;
    va_start(args, request);
    if (request == I2C_TIMEOUT) {
        value = va_arg(args, unsigned long);
    } else {
        arg = va_arg(args, void *);
    }
    va_end(args);
    fake_lock();
    slot = slot_of(fd);
    if (slot < 0) {
        rc = fail(EBADF);
    } else if (keel_fake.fd_kind[slot] == KIND_CHIP) {
        rc = chip_ioctl(keel_fake.fd_index[slot], request, arg);
    } else if (keel_fake.fd_kind[slot] == KIND_REQUEST) {
        rc = request_ioctl(keel_fake.fd_index[slot], request, arg);
    } else {
        rc = i2c_ioctl(request, arg, value);
    }
    fake_unlock();
    return rc;
}

int keel_fake_fcntl(int fd, int command, ...)
{
    va_list args;
    int slot, rc = 0;
    int value = 0;
    va_start(args, command);
    if (command == F_SETFL) {
        value = va_arg(args, int);
    }
    va_end(args);
    fake_lock();
    slot = slot_of(fd);
    if (slot < 0) {
        rc = fail(EBADF);
    } else if (command == F_GETFL) {
        rc = keel_fake.fd_flags[slot];
    } else if (command == F_SETFL) {
        keel_fake.fd_flags[slot] = value;
    } else {
        rc = fail(EINVAL);
    }
    fake_unlock();
    return rc;
}

ssize_t keel_fake_read(int fd, void *buffer, size_t count)
{
    size_t wanted = count / sizeof(struct gpio_v2_line_event);
    ssize_t rc;
    int slot;
    fake_lock();
    slot = slot_of(fd);
    if (slot < 0 || keel_fake.fd_kind[slot] != KIND_REQUEST ||
        keel_fake.request_role[keel_fake.fd_index[slot]] != FAKE_ROLE_RANGE) {
        rc = fail(EBADF);
    } else if (!(keel_fake.fd_flags[slot] & O_NONBLOCK)) {
        violation("read of ECHO events on a blocking descriptor");
        rc = fail(EINVAL);
    } else if (wanted == 0) {
        rc = fail(EINVAL); /* the kernel needs room for one whole event */
    } else if (keel_fake.event_count == 0) {
        rc = fail(EAGAIN);
    } else {
        size_t n = wanted < keel_fake.event_count ? wanted : keel_fake.event_count;
        memcpy(buffer, keel_fake.events, n * sizeof keel_fake.events[0]);
        memmove(keel_fake.events, keel_fake.events + n,
                (keel_fake.event_count - n) * sizeof keel_fake.events[0]);
        keel_fake.event_count -= (unsigned)n;
        rc = (ssize_t)(n * sizeof keel_fake.events[0]);
    }
    fake_unlock();
    return rc;
}

int keel_fake_clock_gettime(clockid_t clock, struct timespec *ts)
{
    int frozen;
    uint64_t us;
    fake_lock();
    frozen = keel_fake.clock_frozen && clock == CLOCK_MONOTONIC;
    us = keel_fake.frozen_us;
    fake_unlock();
    if (!frozen) {
        return clock_gettime(clock, ts);
    }
    ts->tv_sec = (time_t)(us / UINT64_C(1000000));
    ts->tv_nsec = (long)(us % UINT64_C(1000000)) * 1000L;
    return 0;
}

int keel_fake_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                             void *(*start)(void *), void *arg)
{
    pthread_attr_t plain;
    struct sched_param param;
    sigset_t mask;
    int policy = SCHED_OTHER, inherit = PTHREAD_INHERIT_SCHED, mode, rc;
    size_t stack = 0;
    pthread_attr_getschedpolicy(attr, &policy);
    pthread_attr_getschedparam(attr, &param);
    pthread_attr_getinheritsched(attr, &inherit);
    pthread_attr_getstacksize(attr, &stack);
    pthread_sigmask(SIG_SETMASK, NULL, &mask);
    fake_lock();
    mode = keel_fake.thread_mode;
    keel_fake.last_policy = policy;
    keel_fake.last_priority = param.sched_priority;
    keel_fake.last_inherit = inherit;
    keel_fake.last_stack = stack;
    keel_fake.sigint_blocked = sigismember(&mask, SIGINT) == 1;
    keel_fake.sigsegv_blocked = sigismember(&mask, SIGSEGV) == 1;
    if (policy == SCHED_FIFO) {
        keel_fake.fifo_attempts++;
    }
    fake_unlock();
    if (mode == FAKE_THREADS_FAIL) {
        return EAGAIN;
    }
    if (policy == SCHED_FIFO && mode == FAKE_THREADS_FIFO_EPERM) {
        return EPERM;
    }
    /* The thread really runs SCHED_OTHER, so tests need no privilege. */
    pthread_attr_init(&plain);
    pthread_attr_setstacksize(&plain, stack);
    rc = pthread_create(thread, &plain, start, arg);
    pthread_attr_destroy(&plain);
    if (rc == 0) {
        fake_lock();
        keel_fake.creations++;
        fake_unlock();
    }
    return rc;
}
