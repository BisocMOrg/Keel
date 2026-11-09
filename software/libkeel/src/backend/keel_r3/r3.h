/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file r3.h
 * @brief Internal contract of the keel_r3 backend (SPEC section 17): the R3
 *        baseboard on a Raspberry Pi 4 (BCM2711), driven through the GPIO
 *        character device (v2 line requests), i2c-dev and a userspace 100 Hz
 *        PWM worker.
 *
 * The backend is split by concern:
 * - r3_open.c         open and close, the hook table, domain locks, sleeps
 *                     and the fork handler;
 * - r3_gpio.c         chip discovery, line preflight and requests, the
 *                     control outputs and the inputs;
 * - r3_motor.c        the fault latch, the motor path with its interlocks and
 *                     stop epoch, and the PWM worker;
 * - r3_peripherals.c  the LED driver's I2C bus and the HC-SR04 lines.
 *
 * Locking. The backend lock (@c lock, priority inheritance) guards every
 * shadow below that is marked "lock" and every write to the control and motor
 * requests, so a check and the write it allows are atomic with respect to
 * emergency_disable(). It is a leaf: nothing else is taken while it is held.
 * The fault latch's flag is an atomic so that read_inputs() needs no lock.
 * The ranging fields belong to the core's RANGE domain and the I2C fd to its
 * LEDS domain. File descriptors never change between open and destroy.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#ifndef KEEL_BACKEND_KEEL_R3_H
#define KEEL_BACKEND_KEEL_R3_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "backend/pwm_policy.h"
#include "keel/port.h"

/** @brief BCM line numbers of the R3 pin map (SPEC 2.1). */
enum keel__r3_bcm {
    KEEL__R3_BCM_ENABLE = 26,        /**< MOTOR_EN to the MCU and the AND gate. */
    KEEL__R3_BCM_HEARTBEAT = 18,     /**< Heartbeat to the MCU. */
    KEEL__R3_BCM_SERVO_REQUEST = 19, /**< Servo request, held LOW (J5 not fitted). */
    KEEL__R3_BCM_LED_A = 23,         /**< Button LED A. */
    KEEL__R3_BCM_LED_B = 22,         /**< Button LED B. */
    KEEL__R3_BCM_LED_X = 17,         /**< Button LED X. */
    KEEL__R3_BCM_LED_Y = 27,         /**< Button LED Y. */
    KEEL__R3_BCM_LP = 8,             /**< Left bridge, reverse input. */
    KEEL__R3_BCM_LN = 11,            /**< Left bridge, forward input. */
    KEEL__R3_BCM_RP = 10,            /**< Right bridge, forward input. */
    KEEL__R3_BCM_RN = 9,             /**< Right bridge, reverse input. */
    KEEL__R3_BCM_READY = 20,         /**< READY from the MCU. */
    KEEL__R3_BCM_ARMED = 21,         /**< ARMED, the motor-enable gate output. */
    KEEL__R3_BCM_BUTTON_A = 5,       /**< Button A, active low. */
    KEEL__R3_BCM_BUTTON_B = 6,       /**< Button B, active low. */
    KEEL__R3_BCM_BUTTON_X = 16,      /**< Button X, active low. */
    KEEL__R3_BCM_BUTTON_Y = 24,      /**< Button Y, active low. */
    KEEL__R3_BCM_TRIG = 13,          /**< HC-SR04 trigger. */
    KEEL__R3_BCM_ECHO = 25           /**< HC-SR04 echo. */
};

/** @brief Bits of the control request, in its line order (26, 18, 19, 23,
 *  22, 17, 27). */
enum keel__r3_control_bit {
    KEEL__R3_CTL_ENABLE = 1,         /**< BCM26. */
    KEEL__R3_CTL_HEARTBEAT = 2,      /**< BCM18. */
    KEEL__R3_CTL_SERVO_REQUEST = 4,  /**< BCM19. */
    KEEL__R3_CTL_LED_SHIFT = 3,      /**< Button LEDs A/B/X/Y start at this bit. */
    KEEL__R3_CTL_ALL = 0x7F          /**< Every control line. */
};

/** @brief Bits of the input request, in its line order (20, 21, 5, 6, 16, 24). */
enum keel__r3_input_bit {
    KEEL__R3_IN_READY = 1,           /**< BCM20. */
    KEEL__R3_IN_ARMED = 2,           /**< BCM21. */
    KEEL__R3_IN_BUTTON_SHIFT = 2,    /**< Buttons A/B/X/Y start at this bit. */
    KEEL__R3_IN_ALL = 0x3F           /**< Every input line. */
};

/** @brief Bits of the range request, in its line order (13, 25). */
enum keel__r3_range_bit {
    KEEL__R3_RANGE_TRIG = 1,         /**< BCM13, output. */
    KEEL__R3_RANGE_ECHO = 2          /**< BCM25, input with both edge events. */
};

/** @brief Fixed parameters of the backend. */
enum keel__r3_limit {
    KEEL__R3_CONTROL_LINES = 7,      /**< Lines in the control request. */
    KEEL__R3_MOTOR_LINES = 4,        /**< Lines in the motor request. */
    KEEL__R3_INPUT_LINES = 6,        /**< Lines in the input request. */
    KEEL__R3_RANGE_LINES = 2,        /**< Lines in the range request. */
    KEEL__R3_CHIP_LINES_MIN = 28,    /**< The BCM2711 bank 0 has 58 lines; 28 reach the header. */
    KEEL__R3_CHIP_SCAN = 64,         /**< /dev/gpiochip0..63 are probed. */
    KEEL__R3_STACK_BYTES = 65536,    /**< PWM worker stack (raised to the platform minimum). */
    KEEL__R3_TRIG_PULSE_US = 10,     /**< Minimum HC-SR04 trigger pulse. */
    KEEL__R3_EDGE_BUFFER = 32,       /**< Kernel event buffer of the ECHO line. */
    KEEL__R3_EDGE_BATCH = 16,        /**< Edge events read per range_edges() call. */
    KEEL__R3_I2C_TIMEOUT_10MS = 2,   /**< I2C_TIMEOUT, in units of 10 ms (20 ms). */
    KEEL__R3_I2C_MAX_DATA = 32       /**< Largest I2C payload after the register byte. */
};

/** @brief Domains of the core (CONTROL, LEDS, RANGE). */
#define KEEL__R3_DOMAINS 3

/** @brief keel_r3 device state; one allocation also holds the core's device
 *  storage, which follows it. */
typedef struct keel__r3 {
    pthread_mutex_t domains[KEEL__R3_DOMAINS]; /**< Core lock domains (priority inheritance). */
    pthread_mutex_t lock;          /**< The backend lock (priority inheritance). */
    pthread_cond_t work;           /**< Wakes the PWM worker; CLOCK_MONOTONIC. */
    pthread_mutex_t sleep_lock;    /**< Guards @c sleep_cancelled. */
    pthread_cond_t sleep_cond;     /**< Sleepers in sleep_until_us(); CLOCK_MONOTONIC. */
    unsigned sync_ready;           /**< Synchronisation objects initialised so far. */
    int sleep_cancelled;           /**< cancel_sleep() was called (sleep_lock). */

    pthread_t worker;              /**< The PWM worker. */
    int worker_running;            /**< @c worker exists and must be joined. */
    int quit;                      /**< Worker exit request (lock). */
    uint32_t timing_class;         /**< ::keel_timing_class of the worker. */

    int chip_fd;                   /**< GPIO chip, open only during open. */
    int control_fd;                /**< Control request: ENABLE, heartbeat, servo request, LEDs. */
    int motor_fd;                  /**< Motor request: BCM8, 11, 10, 9. */
    int input_fd;                  /**< Input request: READY, ARMED, buttons. */
    int range_fd;                  /**< Range request (TRIG, ECHO), or -1. */
    int i2c_fd;                    /**< LED driver's I2C adapter, or -1. */

    uint32_t outputs;              /**< ::keel_port_output levels last written (lock). */
    keel_port_motor_command motor; /**< Stored motor command; COAST when idle (lock). */
    uint32_t levels;               /**< ::keel__pwm_line levels on the motor lines (lock). */
    uint32_t newest_epoch;         /**< Newest stop epoch seen (lock). */
    uint64_t motion_start_us;      /**< Phase origin of the running pattern (lock). */
    uint64_t scheduled_wake_us;    /**< When the worker is due to run next (lock). */
    keel__pwm_budget budget;       /**< Lateness budget (lock). */
    keel_port_stats stats;         /**< Timing statistics (lock). */
    keel_port_fault fault;         /**< The latched record; status OK when clear (lock). */
    atomic_int latched;            /**< Non-zero while @c fault holds a record. */

    uint32_t echo_seqno;           /**< Last ECHO line_seqno seen, 0 = none (RANGE). */

    struct keel__r3 *next_open;    /**< Next open device, for the fork handler. */
    size_t block_size;             /**< Size of the allocation, for munlock(). */
    int memory_locked;             /**< mlock() of the allocation succeeded. */
} keel__r3;

/**
 * @brief Serial-number comparison of stop epochs, which may wrap.
 * @param[in] epoch  Epoch carried by a request.
 * @param[in] newest Newest epoch the backend has seen.
 * @return Non-zero when @p epoch is older than @p newest.
 */
static inline int keel__r3_epoch_older(uint32_t epoch, uint32_t newest)
{
    return (int32_t)(epoch - newest) < 0;
}

/**
 * @brief Absolute CLOCK_MONOTONIC deadline for the condition variables.
 * @param[in] us Time in microseconds.
 * @return The same time as a timespec.
 */
static inline struct timespec keel__r3_timespec(uint64_t us)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(us / UINT64_C(1000000));
    ts.tv_nsec = (long)(us % UINT64_C(1000000)) * 1000L;
    return ts;
}

/* ---- r3_open.c (keel__r3_open() is declared in backend/hosted.h) ---------- */

/**
 * @brief CLOCK_MONOTONIC in microseconds (the backend and kernel event clock).
 * @return Monotonic time, us.
 */
uint64_t keel__r3_now_us(void);

/**
 * @brief Records an open failure: clears the previous path and consumer,
 *        then fills resource, errno, path, line, consumer and hint.
 * @param[out] report   Full-size report.
 * @param[in]  resource ::keel_resource.
 * @param[in]  os_error errno, or 0.
 * @param[in]  path     Device path, or NULL.
 * @param[in]  line     BCM line, or -1.
 * @param[in]  consumer Kernel consumer label, or NULL.
 * @param[in]  hint     One-line remedy.
 */
void keel__r3_report(keel_open_report *report, uint32_t resource, int32_t os_error,
                     const char *path, int32_t line, const char *consumer, const char *hint);

/**
 * @brief Maps an errno from opening a device or requesting lines to a
 *        SETUP-class status (SPEC 17.1).
 * @param[in] error errno.
 * @return ::KEEL_E_PERMISSION, ::KEEL_E_NOT_FOUND, ::KEEL_E_IN_USE,
 *         ::KEEL_E_NO_MEMORY or ::KEEL_E_SETUP_IO.
 */
int32_t keel__r3_open_status(int error);

/* ---- r3_gpio.c ------------------------------------------------------------ */

/**
 * @brief Finds the BCM2711 chip, checks every line with
 *        GPIO_V2_GET_LINEINFO and requests the control, motor, input and
 *        (unless ::KEEL_OPEN_NO_RANGE) range lines, all outputs LOW.
 * @param[in,out] b       Backend; receives the request fds.
 * @param[in]     options Validated options.
 * @param[out]    report  Full-size report, filled on failure.
 * @return ::KEEL_OK or a SETUP-class status.
 */
int32_t keel__r3_gpio_open(keel__r3 *b, const keel_open_options *options,
                           keel_open_report *report);

/**
 * @brief GPIO_V2_LINE_SET_VALUES on one request.
 * @param[in] fd   Line request.
 * @param[in] mask Request bits to set.
 * @param[in] bits Their levels.
 * @return 0 or the errno of the failure.
 */
int keel__r3_set_values(int fd, uint64_t mask, uint64_t bits);

/**
 * @brief GPIO_V2_LINE_GET_VALUES on one request.
 * @param[in]  fd   Line request.
 * @param[in]  mask Request bits to read.
 * @param[out] bits Their levels (logical: active-low lines read 1 when LOW).
 * @return 0 or the errno of the failure.
 */
int keel__r3_get_values(int fd, uint64_t mask, uint64_t *bits);

/**
 * @brief Control-request bits of ::keel_port_output values.
 * @param[in] outputs ::keel_port_output bits.
 * @return ::keel__r3_control_bit bits.
 */
uint64_t keel__r3_control_bits(uint32_t outputs);

/** @brief Hook @c read_inputs (see port.h). @param[in] user Backend. @param[out] out_inputs Inputs. @return Status. */
int32_t keel__r3_read_inputs(void *user, uint32_t *out_inputs);

/** @brief Hook @c write_outputs (see port.h). @param[in] user Backend. @param[in] mask Outputs to set.
 *  @param[in] values Their levels. @param[in] stop_epoch Epoch of the decision. @return Status. */
int32_t keel__r3_write_outputs(void *user, uint32_t mask, uint32_t values, uint32_t stop_epoch);

/* ---- r3_motor.c ----------------------------------------------------------- */

/**
 * @brief Latches a backend fault (first record wins) and stops by itself:
 *        ENABLE and the servo request LOW, motors LOW, COAST stored. The
 *        heartbeat is never touched. Call with the backend lock held.
 * @param[in,out] b        Backend.
 * @param[in]     status   FAULT-class status.
 * @param[in]     cause    ::keel_fault_cause.
 * @param[in]     os_error errno, or 0.
 * @param[in]     detail   Cause-specific detail.
 * @return @p status.
 */
int32_t keel__r3_latch(keel__r3 *b, int32_t status, uint32_t cause, int32_t os_error,
                       uint64_t detail);

/**
 * @brief Stores COAST and drives the motor lines LOW. Backend lock held.
 * @param[in,out] b Backend.
 * @return 0 or the errno of the failed write.
 */
int keel__r3_coast(keel__r3 *b);

/**
 * @brief One pass of the PWM worker at @p now_us: lease expiry, lateness
 *        accounting, then the line levels of the current phase. Backend lock
 *        held.
 * @param[in,out] b      Backend.
 * @param[in]     now_us Time of the pass.
 * @return The absolute time of the next pass, or 0 when the motors coast.
 */
uint64_t keel__r3_pwm_pass(keel__r3 *b, uint64_t now_us);

/**
 * @brief Starts the PWM worker: SCHED_FIFO ::KEEL_RT_PRIORITY unless
 *        ::KEEL_OPEN_NO_REALTIME, recreated as SCHED_OTHER on EPERM, async
 *        signals blocked, named "keel-pwm". Records the outcome in @p report.
 * @param[in,out] b       Backend.
 * @param[in]     options Validated options.
 * @param[out]    report  Full-size report.
 * @return ::KEEL_OK, ::KEEL_E_NO_MEMORY or ::KEEL_E_SETUP_IO.
 */
int32_t keel__r3_worker_start(keel__r3 *b, const keel_open_options *options,
                              keel_open_report *report);

/**
 * @brief Stops the worker (motors and control lines LOW first) and joins it.
 * @param[in,out] b Backend.
 */
void keel__r3_worker_stop(keel__r3 *b);

/** @brief Hook @c write_motors (see port.h). @param[in] user Backend. @param[in] command Command. @return Status. */
int32_t keel__r3_write_motors(void *user, const keel_port_motor_command *command);

/** @brief Hook @c emergency_disable (see port.h). @param[in] user Backend. @param[in] stop_epoch Epoch. @return Status. */
int32_t keel__r3_emergency_disable(void *user, uint32_t stop_epoch);

/** @brief Hook @c get_fault (see port.h). @param[in] user Backend. @param[out] out Record. @return Status. */
int32_t keel__r3_get_fault(void *user, keel_port_fault *out);

/** @brief Hook @c reset_fault (see port.h). @param[in] user Backend. @return Status. */
int32_t keel__r3_reset_fault(void *user);

/** @brief Hook @c get_stats (see port.h). @param[in] user Backend. @param[out] out Statistics. @return Status. */
int32_t keel__r3_get_stats(void *user, keel_port_stats *out);

/* ---- r3_peripherals.c ----------------------------------------------------- */

/**
 * @brief Opens the LED driver's I2C adapter and sets its 20 ms timeout. A
 *        failure is a warning: @c i2c_fd stays -1 and the report says why.
 * @param[in,out] b       Backend.
 * @param[in]     options Validated options.
 * @param[out]    report  Full-size report.
 */
void keel__r3_i2c_open(keel__r3 *b, const keel_open_options *options, keel_open_report *report);

/** @brief Hook @c i2c_write (see port.h). @param[in] user Backend. @param[in] address 7-bit address.
 *  @param[in] reg Register. @param[in] data Bytes. @param[in] count Byte count. @return Status. */
int32_t keel__r3_i2c_write(void *user, uint8_t address, uint8_t reg, const uint8_t *data,
                           size_t count);

/** @brief Hook @c range_trigger (see port.h). @param[in] user Backend. @param[out] out_echo_high ECHO HIGH. @return Status. */
int32_t keel__r3_range_trigger(void *user, uint32_t *out_echo_high);

/** @brief Hook @c range_edges (see port.h). @param[in] user Backend. @param[out] edges Edges.
 *  @param[in] capacity Capacity. @param[out] out_count Count. @return Status. */
int32_t keel__r3_range_edges(void *user, keel_port_edge *edges, uint32_t capacity,
                             uint32_t *out_count);

#endif /* KEEL_BACKEND_KEEL_R3_H */
