/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel_r3 motor path (SPEC 17.4 and 17.5): the fault latch, the interlocks
 * and stop epoch in front of the motor lines, and the 100 Hz PWM worker.
 *
 * BCM8-11 have no hardware PWM, so a userspace thread generates the pattern.
 * A late wake only stretches or shortens one pulse (the output is recomputed
 * from the current phase), so single late wakes are counted, never fatal:
 * the worker latches on one wake more than 30 ms late, on more than 20 ms of
 * excess lateness within 100 ms, or on a failed line write. The control
 * thread catches a worker that stopped running altogether (the stall check
 * in write_motors). The hard bounds stay the 70 ms lease, that stall check
 * and the MCU's 80 ms heartbeat window.
 *
 * The worker waits on a CLOCK_MONOTONIC condition variable for absolute
 * deadlines (the equivalent of clock_nanosleep with TIMER_ABSTIME that can
 * also be woken when the pattern changes). It holds the backend lock only
 * while it reads the command and writes one set of levels.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "backend/keel_r3/r3.h"

/* Motor line levels after a failed write: unknown, so the next write is
 * never skipped. */
#define LEVELS_UNKNOWN UINT32_MAX

/* ---- latch and coast ------------------------------------------------------ */

int keel__r3_coast(keel__r3 *b)
{
    int error;
    memset(&b->motor, 0, sizeof b->motor);
    b->motor.mode = KEEL_MOTOR_COAST;
    error = keel__r3_set_values(b->motor_fd, KEEL__PWM_ALL, 0);
    b->levels = error ? LEVELS_UNKNOWN : 0u;
    return error;
}

int32_t keel__r3_latch(keel__r3 *b, int32_t status, uint32_t cause, int32_t os_error,
                       uint64_t detail)
{
    if (b->fault.status == KEEL_OK) {
        b->fault.status = status;
        b->fault.cause = cause;
        b->fault.os_error = os_error;
        b->fault.detail = detail;
        b->fault.time_us = keel__r3_now_us();
        atomic_store(&b->latched, 1);
    }
    /* Self-stop. The heartbeat is the core's to stop: lowering it here would
     * make the MCU's reaction, not this cause, the first thing the core sees. */
    (void)keel__r3_set_values(b->control_fd, KEEL__R3_CTL_ENABLE | KEEL__R3_CTL_SERVO_REQUEST, 0);
    b->outputs &= ~(uint32_t)KEEL_OUT_ENABLE;
    (void)keel__r3_coast(b);
    return status;
}

/* A synchronous coast by the calling thread; its failure is MOTOR_WRITE. */
static int32_t coast_or_latch(keel__r3 *b)
{
    int error = keel__r3_coast(b);
    return error ? keel__r3_latch(b, KEEL_E_IO, KEEL_CAUSE_MOTOR_WRITE, error, 0) : KEEL_OK;
}

/* ---- the pattern ---------------------------------------------------------- */

/* HIGH time of a duty in one period: 10 us per permille. */
static uint64_t on_time_us(int32_t duty)
{
    uint64_t magnitude = duty < 0 ? (uint64_t)(-(int64_t)duty) : (uint64_t)duty;
    return magnitude * (KEEL__PWM_PERIOD_US / KEEL_DUTY_MAX);
}

/* The next instant at which a line may change: a falling edge later in this
 * period, else the next period start; never after the lease. A brake has no
 * edges and sleeps until its lease ends. */
static uint64_t next_wake(const keel__r3 *b, uint64_t now_us)
{
    const keel_port_motor_command *m = &b->motor;
    uint64_t next = m->valid_until_us;
    if (m->mode == KEEL_MOTOR_DRIVE) {
        uint64_t phase = (now_us - b->motion_start_us) % KEEL__PWM_PERIOD_US;
        uint64_t period_start = now_us - phase;
        uint64_t edge = period_start + KEEL__PWM_PERIOD_US;
        uint64_t left = on_time_us(m->left), right = on_time_us(m->right);
        if (left > phase && period_start + left < edge) {
            edge = period_start + left;
        }
        if (right > phase && period_start + right < edge) {
            edge = period_start + right;
        }
        if (edge < next) {
            next = edge;
        }
    }
    return next;
}

static void count_wake(keel__r3 *b, uint64_t lateness_us)
{
    uint32_t late = lateness_us > UINT32_MAX ? UINT32_MAX : (uint32_t)lateness_us;
    b->stats.wakes++;
    b->stats.last_lateness_us = late;
    if (late > b->stats.max_lateness_us) {
        b->stats.max_lateness_us = late;
    }
    if (lateness_us > KEEL__PWM_LATE_US) {
        b->stats.late_wakes++;
    }
    if (lateness_us > KEEL__PWM_VERY_LATE_US) {
        b->stats.very_late_wakes++;
    }
}

uint64_t keel__r3_pwm_pass(keel__r3 *b, uint64_t now_us)
{
    uint64_t lateness, detail = 0, next;
    uint32_t cause, levels;
    int error;

    if (b->motor.mode == KEEL_MOTOR_COAST) {
        return 0;
    }
    if (now_us >= b->motor.valid_until_us) {
        /* Not renewed in time: a silent coast. A missing tick is the core's
         * diagnosis; the backend only enforces. */
        b->stats.lease_expiries++;
        error = keel__r3_coast(b);
        if (error) {
            (void)keel__r3_latch(b, KEEL_E_PWM, KEEL_CAUSE_PWM_WRITE, error, 0);
        }
        return 0;
    }
    /* A signal for a new pattern can arrive before the scheduled wake. */
    lateness = now_us > b->scheduled_wake_us ? now_us - b->scheduled_wake_us : 0u;
    count_wake(b, lateness);
    cause = keel__pwm_account(&b->budget, now_us, lateness, &detail);
    if (cause != KEEL_CAUSE_NONE) {
        (void)keel__r3_latch(b, KEEL_E_PWM, cause, 0, detail);
        return 0;
    }
    levels = keel__pwm_levels(b->motor.mode, b->motor.left, b->motor.right,
                              now_us - b->motion_start_us);
    if (levels != b->levels) {
        error = keel__r3_set_values(b->motor_fd, KEEL__PWM_ALL, levels);
        if (error) {
            b->levels = LEVELS_UNKNOWN;
            (void)keel__r3_latch(b, KEEL_E_PWM, KEEL_CAUSE_PWM_WRITE, error, 0);
            return 0;
        }
        b->levels = levels;
    }
    next = next_wake(b, now_us);
    b->scheduled_wake_us = next;
    return next;
}

/* ---- write_motors ----------------------------------------------------------- */

static int command_valid(const keel_port_motor_command *command)
{
    if (command->mode == KEEL_MOTOR_BRAKE) {
        return 1;
    }
    return command->mode == KEEL_MOTOR_DRIVE && command->left >= -KEEL_DUTY_MAX &&
           command->left <= KEEL_DUTY_MAX && command->right >= -KEEL_DUTY_MAX &&
           command->right <= KEEL_DUTY_MAX;
}

/* The scheduled wake, not the last pass, because a brake has no periodic
 * wakes; an expired command is the lease's business, not a stall. */
static int worker_stalled(const keel__r3 *b, uint64_t now_us)
{
    return b->motor.mode != KEEL_MOTOR_COAST && b->motor.valid_until_us > now_us &&
           now_us > b->scheduled_wake_us && now_us - b->scheduled_wake_us > KEEL__PWM_STALL_US;
}

/* Wakes the worker only for a new pattern or motion from idle; a renewal
 * of the same pattern just moves the lease, which the worker reads when it
 * next runs. */
static void store_command(keel__r3 *b, const keel_port_motor_command *command, uint64_t now_us)
{
    int from_idle = b->motor.mode == KEEL_MOTOR_COAST;
    int changed = from_idle || command->mode != b->motor.mode || command->left != b->motor.left ||
                  command->right != b->motor.right;
    b->motor = *command;
    if (from_idle) {
        b->motion_start_us = now_us; /* a new phase origin */
        b->scheduled_wake_us = now_us;
    }
    if (changed) {
        pthread_cond_signal(&b->work);
    }
}

/* SPEC 17.5 order: COAST, latch, epoch, lease, interlock, ARMED, stall, store. */
static int32_t write_motors_locked(keel__r3 *b, const keel_port_motor_command *command,
                                   uint64_t now_us)
{
    uint64_t armed = 0;
    int error;
    if (command->mode == KEEL_MOTOR_COAST) {
        return coast_or_latch(b);
    }
    if (!command_valid(command)) {
        return keel__r3_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERNAL, 0, command->mode);
    }
    if (b->fault.status != KEEL_OK) {
        return b->fault.status; /* the latch has already coasted */
    }
    if (keel__r3_epoch_older(command->stop_epoch, b->newest_epoch)) {
        b->stats.suppressed_writes++;
        return coast_or_latch(b);
    }
    if (command->valid_until_us <= now_us) {
        b->stats.lease_expiries++;
        return coast_or_latch(b);
    }
    if (!(b->outputs & KEEL_OUT_ENABLE)) {
        /* The servo request is held LOW permanently on R3. */
        return keel__r3_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERLOCK, 0, command->mode);
    }
    /* A fresh ARMED read: the ENABLE shadow alone would allow a drive during
     * the acknowledgement's HIGH levels. ARMED = 0 can be a genuine MCU drop
     * racing the core's own read, so it is feedback, not a defect. */
    error = keel__r3_get_values(b->input_fd, KEEL__R3_IN_ARMED, &armed);
    if (error) {
        return keel__r3_latch(b, KEEL_E_IO, KEEL_CAUSE_GPIO_READ, error, 0);
    }
    if (!armed) {
        return keel__r3_latch(b, KEEL_E_FEEDBACK, KEEL_CAUSE_ARMED_LOST, 0, 0);
    }
    if (worker_stalled(b, now_us)) {
        /* The latch drives the lines LOW from this thread. */
        return keel__r3_latch(b, KEEL_E_PWM, KEEL_CAUSE_PWM_STALL, 0,
                              now_us - b->scheduled_wake_us);
    }
    store_command(b, command, now_us);
    return KEEL_OK;
}

int32_t keel__r3_write_motors(void *user, const keel_port_motor_command *command)
{
    keel__r3 *b = user;
    int32_t rc;
    pthread_mutex_lock(&b->lock);
    rc = write_motors_locked(b, command, keel__r3_now_us());
    pthread_mutex_unlock(&b->lock);
    return rc;
}

int32_t keel__r3_emergency_disable(void *user, uint32_t stop_epoch)
{
    keel__r3 *b = user;
    int32_t rc = KEEL_OK;
    int error;
    /* The lock is a priority-inheritance mutex: its kernel hand-off goes to
     * the waiter, so a thread looping on control I/O cannot keep a stop
     * waiting behind it. */
    pthread_mutex_lock(&b->lock);
    if (!keel__r3_epoch_older(stop_epoch, b->newest_epoch)) {
        b->newest_epoch = stop_epoch;
    }
    error = keel__r3_set_values(b->control_fd, KEEL__R3_CTL_ENABLE, 0);
    if (error) {
        rc = keel__r3_latch(b, KEEL_E_IO, KEEL_CAUSE_GPIO_WRITE, error, KEEL_OUT_ENABLE);
    } else {
        b->outputs &= ~(uint32_t)KEEL_OUT_ENABLE;
        rc = coast_or_latch(b);
    }
    pthread_mutex_unlock(&b->lock);
    return rc;
}

/* ---- fault record and statistics ------------------------------------------ */

int32_t keel__r3_get_fault(void *user, keel_port_fault *out)
{
    keel__r3 *b = user;
    uint32_t size = out->struct_size;
    pthread_mutex_lock(&b->lock);
    *out = b->fault;
    pthread_mutex_unlock(&b->lock);
    out->struct_size = size;
    return KEEL_OK;
}

int32_t keel__r3_reset_fault(void *user)
{
    keel__r3 *b = user;
    pthread_mutex_lock(&b->lock);
    memset(&b->fault, 0, sizeof b->fault);
    b->fault.struct_size = sizeof b->fault;
    keel__pwm_budget_reset(&b->budget);
    atomic_store(&b->latched, 0);
    pthread_mutex_unlock(&b->lock);
    return KEEL_OK;
}

int32_t keel__r3_get_stats(void *user, keel_port_stats *out)
{
    keel__r3 *b = user;
    uint32_t size = out->struct_size;
    pthread_mutex_lock(&b->lock);
    *out = b->stats;
    pthread_mutex_unlock(&b->lock);
    out->struct_size = size;
    return KEEL_OK;
}

/* ---- the worker thread ---------------------------------------------------- */

/* Pre-faults and pins the worker's stack so that a wake never page-faults.
 * mlock() populates the pages; without the privilege, touching them still
 * pre-faults the part the worker uses. */
static void pin_stack(void **out_addr, size_t *out_size)
{
    pthread_attr_t attributes;
    void *addr = NULL;
    size_t size = 0;
    *out_addr = NULL;
    *out_size = 0;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
        return;
    }
    if (pthread_attr_getstack(&attributes, &addr, &size) == 0 && mlock(addr, size) == 0) {
        *out_addr = addr;
        *out_size = size;
    } else {
        volatile unsigned char probe[KEEL__R3_STACK_BYTES / 4];
        size_t i;
        for (i = 0; i < sizeof probe; i += 256) {
            probe[i] = 0;
        }
    }
    pthread_attr_destroy(&attributes);
}

static void *worker_main(void *argument)
{
    keel__r3 *b = argument;
    void *stack;
    size_t stack_size;
    pin_stack(&stack, &stack_size);
    pthread_mutex_lock(&b->lock);
    while (!b->quit) {
        uint64_t next = keel__r3_pwm_pass(b, keel__r3_now_us());
        if (next == 0) {
            pthread_cond_wait(&b->work, &b->lock);
        } else {
            struct timespec deadline = keel__r3_timespec(next);
            (void)pthread_cond_timedwait(&b->work, &b->lock, &deadline);
        }
    }
    pthread_mutex_unlock(&b->lock);
    if (stack) {
        (void)munlock(stack, stack_size);
    }
    return NULL;
}

static size_t stack_bytes(void)
{
    long minimum = sysconf(_SC_THREAD_STACK_MIN);
    return minimum > KEEL__R3_STACK_BYTES ? (size_t)minimum : (size_t)KEEL__R3_STACK_BYTES;
}

/* The worker must never run an application's signal handler (a JVM, CLR or
 * Python host installs many); synchronous faults stay deliverable. */
static void async_signals(sigset_t *set)
{
    sigfillset(set);
    sigdelset(set, SIGSEGV);
    sigdelset(set, SIGBUS);
    sigdelset(set, SIGFPE);
    sigdelset(set, SIGILL);
    sigdelset(set, SIGTRAP);
    sigdelset(set, SIGSYS);
}

/* Returns 0 or the error of the failing step. The policy is always explicit,
 * so a best-effort worker does not inherit a real-time caller's policy. */
static int create_worker(keel__r3 *b, int realtime)
{
    pthread_attr_t attributes;
    struct sched_param param;
    sigset_t blocked, previous;
    int error = pthread_attr_init(&attributes);
    if (error) {
        return error;
    }
    memset(&param, 0, sizeof param);
    param.sched_priority = realtime ? KEEL_RT_PRIORITY : 0;
    error = pthread_attr_setstacksize(&attributes, stack_bytes());
    if (!error) {
        error = pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
    }
    if (!error) {
        error = pthread_attr_setschedpolicy(&attributes, realtime ? SCHED_FIFO : SCHED_OTHER);
    }
    if (!error) {
        error = pthread_attr_setschedparam(&attributes, &param);
    }
    if (!error) {
        async_signals(&blocked);
        pthread_sigmask(SIG_SETMASK, &blocked, &previous);
        error = pthread_create(&b->worker, &attributes, worker_main, b);
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
    }
    pthread_attr_destroy(&attributes);
    return error;
}

int32_t keel__r3_worker_start(keel__r3 *b, const keel_open_options *options,
                              keel_open_report *report)
{
    int realtime = !(options->flags & KEEL_OPEN_NO_REALTIME);
    int error = realtime ? create_worker(b, 1) : EPERM;
    if (error == 0) {
        b->timing_class = KEEL_TIMING_REALTIME;
    } else if (error == EPERM) {
        error = create_worker(b, 0);
        if (error) {
            keel__r3_report(report, KEEL_RESOURCE_PWM_WORKER, error, NULL, -1, NULL,
                            "could not start the motor PWM worker thread");
            return keel__r3_open_status(error);
        }
        b->timing_class = KEEL_TIMING_BEST_EFFORT;
        if (realtime) {
            /* Refused, not disabled: worth a warning and a remedy. */
            report->warnings |= KEEL_OPEN_WARN_BEST_EFFORT_TIMING;
            report->realtime_os_error = EPERM;
            if (report->resource == KEEL_RESOURCE_NONE) {
                keel__r3_report(report, KEEL_RESOURCE_PWM_WORKER, EPERM, NULL, -1, NULL,
                                "SCHED_FIFO was refused, so motor PWM runs best effort: allow "
                                "rtprio 40 with sudo keel-setup --user NAME, then log in again");
            }
        }
    } else {
        keel__r3_report(report, KEEL_RESOURCE_PWM_WORKER, error, NULL, -1, NULL,
                        "could not start the motor PWM worker thread");
        return keel__r3_open_status(error);
    }
    b->worker_running = 1;
    b->stats.timing_class = b->timing_class;
    (void)pthread_setname_np(b->worker, "keel-pwm");
    return KEEL_OK;
}

void keel__r3_worker_stop(keel__r3 *b)
{
    pthread_mutex_lock(&b->lock);
    b->quit = 1;
    /* Everything LOW before the worker goes, so no edge can follow. */
    if (b->control_fd >= 0) {
        (void)keel__r3_set_values(b->control_fd, KEEL__R3_CTL_ALL, 0);
        b->outputs = 0;
    }
    if (b->motor_fd >= 0) {
        (void)keel__r3_coast(b);
    }
    pthread_cond_signal(&b->work);
    pthread_mutex_unlock(&b->lock);
    if (b->worker_running) {
        pthread_join(b->worker, NULL);
        b->worker_running = 0;
    }
}
