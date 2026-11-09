/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file port.h
 * @brief C porting interface: attach the portable libkeel core to a backend
 *        (the keel_r3 Linux backend, the simulator, bare-metal or RTOS ports,
 *        test doubles). C and C++ only; not for language bindings.
 *
 * The functions declared here are not exported from libkeel.so. They are
 * available in the static archive (libkeel.a) and in source builds only, so
 * the porting interface can evolve through ::KEEL_PORT_VERSION and
 * @c struct_size without touching the shared library's ABI.
 *
 * @par Status rules for hooks
 * - Safety-path hooks (@c read_inputs, @c write_outputs, @c write_motors,
 *   @c emergency_disable, @c servo, @c sleep_until_us, @c get_fault,
 *   @c reset_fault) return ::KEEL_OK or a FAULT-class status (normally
 *   ::KEEL_E_IO, ::KEEL_E_PWM, ::KEEL_E_FEEDBACK or ::KEEL_E_INTERNAL). The
 *   core passes a FAULT-class status through unchanged; any other non-OK
 *   value latches ::KEEL_E_INTERNAL with cause ::KEEL_CAUSE_HAL_CONTRACT and
 *   the raw value in keel_fault::detail.
 * - Peripheral hooks (@c i2c_write, @c range_trigger, @c range_edges): any
 *   non-OK value becomes ::KEEL_E_PERIPHERAL and never latches.
 * - Backends report an OS failure as ::KEEL_E_IO with the errno in their
 *   fault record, never as ::KEEL_E_AGAIN or a "busy" code.
 *
 * @par The backend fault latch
 * A backend latches a fault record (::keel_port_fault) for every failure it
 * returns from a safety-path hook and for every failure its own threads
 * detect (for example a PWM worker overrun). While latched:
 * - @c read_inputs still returns ::KEEL_OK with the real levels and sets the
 *   pseudo-bit ::KEEL_IN_BACKEND_FAULT; the core checks that bit before any
 *   other evaluation and records the backend's cause.
 * - LOW levels, COAST and button-LED bits are always written; a rising
 *   ENABLE or heartbeat, DRIVE, BRAKE and a servo request are refused with
 *   the latched status.
 * - Only @c reset_fault clears it; the core calls it from keel_open() and
 *   keel_clear_fault() only.
 * A backend thread that stops by itself lowers ENABLE and the servo request
 * and coasts the motors; it never touches the heartbeat.
 *
 * @par Stop epoch
 * keel_disarm(), keel_emergency_stop() and keel_close() call
 * @c emergency_disable with a new, increasing @c stop_epoch before they take
 * any lock. Every request that could start motion (a rising ENABLE in
 * @c write_outputs, DRIVE or BRAKE in @c write_motors, REQUEST or PULSE in
 * @c servo) carries the epoch under which the core decided it. The backend
 * keeps the newest epoch it was given (serial-number comparison) and
 * suppresses a request carrying an older one: ENABLE stays LOW, the motors
 * coast, the servo stays off, the hook returns ::KEEL_OK and the suppression
 * is counted in keel_port_stats::suppressed_writes. The core notices the new
 * epoch itself and completes the stop. So a stop issued from any thread can
 * never be undone by a control section that was already running.
 *
 * @par Interlocks (required in keel_r3 and the simulator)
 * Under its own lock the backend refuses, by latching and returning
 * ::KEEL_E_INTERNAL with cause ::KEEL_CAUSE_INTERLOCK:
 * - a rising ENABLE unless the stored motor command is COAST and the servo
 *   request is LOW;
 * - a servo REQUEST unless ENABLE is HIGH and the motors have coasted for at
 *   least 3 ms;
 * - DRIVE or BRAKE unless ENABLE is HIGH and the servo request is LOW.
 * DRIVE or BRAKE also needs a fresh ARMED = 1 read under the backend lock;
 * ARMED = 0 coasts and latches ::KEEL_E_FEEDBACK with cause
 * ::KEEL_CAUSE_ARMED_LOST. DRIVE or BRAKE whose @c valid_until_us is at or
 * before now is a synchronous coast that returns ::KEEL_OK and counts in
 * keel_port_stats::lease_expiries.
 */
#ifndef KEEL_PORT_H
#define KEEL_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "keel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup keel_port Porting interface
 * @brief Backend table, its value types and the attach call.
 * @{
 */

/** @brief Version of the backend table accepted by keel_port_attach(). */
enum keel_port_version {
    /** Tables with this @c port_version and @c struct_size at least the 1.0 size. */
    KEEL_PORT_VERSION = 1
};

/** @brief Logical outputs for @c write_outputs (never BCM numbers). */
enum keel_port_output {
    /** MOTOR_EN request to the MCU and the AND gate (BCM26 on R3). */
    KEEL_OUT_ENABLE = 1,
    /** Heartbeat to the MCU (BCM18 on R3). */
    KEEL_OUT_HEARTBEAT = 2,
    /** Button LEDs A/B/X/Y start at this bit (BCM23/22/17/27 on R3). */
    KEEL_OUT_BUTTON_LED_SHIFT = 8,
    /** Button LED bits. */
    KEEL_OUT_BUTTON_LEDS = 0xF00,
    /** Every output @c write_outputs may touch. */
    KEEL_OUT_ALL = 0xF03
};

/** @brief Logical inputs returned by @c read_inputs. */
enum keel_port_input {
    /** READY from the MCU (BCM20 on R3). */
    KEEL_IN_READY = 1,
    /** ARMED, the motor-enable gate output (BCM21 on R3). */
    KEEL_IN_ARMED = 2,
    /** Buttons start at this bit, as logical "pressed" (active-low pins). */
    KEEL_IN_BUTTON_SHIFT = 8,
    /** Button bits A/B/X/Y. */
    KEEL_IN_BUTTONS = 0xF00,
    /** Pseudo-bit: the backend fault latch is set; read it with @c get_fault. */
    KEEL_IN_BACKEND_FAULT = 0x10000
};

/** @brief Modes of keel_port_motor_command::mode. */
enum keel_port_motor_mode {
    /** All four bridge inputs LOW; synchronous, always permitted. */
    KEEL_MOTOR_COAST = 0,
    /** Signed duty at 100 Hz, the opposite input of each bridge LOW. */
    KEEL_MOTOR_DRIVE = 1,
    /** All four bridge inputs HIGH. */
    KEEL_MOTOR_BRAKE = 2
};

/** @brief Modes of keel_port_servo_command::mode. */
enum keel_port_servo_mode {
    /** Stop the pulse, then lower the servo request; synchronous, always permitted. */
    KEEL_PORT_SERVO_OFF = 0,
    /** Raise the servo request with no pulse (the grant is confirmed first). */
    KEEL_PORT_SERVO_REQUEST = 1,
    /** Request HIGH and a 50 Hz pulse of @c pulse_us. */
    KEEL_PORT_SERVO_PULSE = 2
};

/** @brief Lock domains for the @c lock and @c unlock hooks. The core never
 *  holds two domains at once and never sleeps while holding CONTROL. */
enum keel_port_lock {
    /** Safety state, heartbeat, arming, motion, servo, buttons, button LEDs,
     *  fault record and snapshot. */
    KEEL_LOCK_CONTROL = 0,
    /** RGB LED frame cache and I2C transactions. */
    KEEL_LOCK_LEDS = 1,
    /** Ultrasonic measurement state. */
    KEEL_LOCK_RANGE = 2
};

/** @brief Bits of keel_port_backend::flags. */
enum keel_port_flag {
    /** No lock hooks: every call must come from one thread. With a
     *  @c thread_id hook the core returns ::KEEL_E_CONFLICT (no effect) to
     *  calls from any other thread. */
    KEEL_PORT_SINGLE_THREADED = 1
};

/** @brief Motor command passed to @c write_motors. */
typedef struct keel_port_motor_command {
    /** ::keel_port_motor_mode. */
    uint32_t mode;
    /** DRIVE: left duty, permille -1000..1000 (positive drives BCM11 on R3). */
    int32_t left;
    /** DRIVE: right duty, permille -1000..1000 (positive drives BCM10 on R3). */
    int32_t right;
    /** Stop epoch under which the core decided this command. */
    uint32_t stop_epoch;
    /** DRIVE/BRAKE: coast at or after this backend time, us. The backend
     *  never renews it; the core renews it on every armed tick. */
    uint64_t valid_until_us;
} keel_port_motor_command;

/** @brief Servo command passed to @c servo. */
typedef struct keel_port_servo_command {
    /** ::keel_port_servo_mode. */
    uint32_t mode;
    /** PULSE: pulse width, us (500..2500). */
    uint32_t pulse_us;
    /** Stop epoch under which the core decided this command. */
    uint32_t stop_epoch;
    /** Reserved; zero. */
    uint32_t reserved0;
    /** REQUEST/PULSE: lease end, backend time in us. At or after it the
     *  backend stops the pulse and lowers the request by itself, without
     *  waiting for a tick. */
    uint64_t valid_until_us;
} keel_port_servo_command;

/** @brief Backend fault record, read through @c get_fault. */
typedef struct keel_port_fault {
    /** Caller (the core) sets sizeof(keel_port_fault). */
    uint32_t struct_size;
    /** FAULT-class ::keel_status, or ::KEEL_OK when nothing is latched. */
    int32_t status;
    /** ::keel_fault_cause. */
    uint32_t cause;
    /** errno, 0 if none. */
    int32_t os_error;
    /** Cause-specific value (see ::keel_fault_cause). */
    uint64_t detail;
    /** Backend time of detection, us. */
    uint64_t time_us;
} keel_port_fault;

/** @brief Backend timing statistics, read through @c get_stats. */
typedef struct keel_port_stats {
    /** Caller (the core) sets sizeof(keel_port_stats). */
    uint32_t struct_size;
    /** ::keel_timing_class of the PWM generator. */
    uint32_t timing_class;
    /** Total PWM generator wakes. */
    uint64_t wakes;
    /** Wakes more than 1 ms late. */
    uint32_t late_wakes;
    /** Wakes more than 5 ms late. */
    uint32_t very_late_wakes;
    /** Largest lateness, us. */
    uint32_t max_lateness_us;
    /** Lateness of the most recent wake, us. */
    uint32_t last_lateness_us;
    /** Leases that ran out and were coasted (not a fault). */
    uint32_t lease_expiries;
    /** Requests suppressed because they carried an old stop epoch. */
    uint32_t suppressed_writes;
} keel_port_stats;

/** @brief One ECHO edge with its kernel timestamp. */
typedef struct keel_port_edge {
    /** Event time on the backend clock, us. */
    uint64_t time_us;
    /** 1 = rising, 0 = falling. */
    uint32_t rising;
    /** Reserved; zero. */
    uint32_t reserved0;
} keel_port_edge;

/**
 * @brief Backend table. Zero-initialise, set @c struct_size and
 *        @c port_version, then fill the hooks; an optional hook left NULL
 *        means the feature is absent.
 *
 * Every hook receives @c user. Unless stated otherwise the core calls a hook
 * with the relevant domain lock held, never two hooks at once for one domain,
 * and never from inside another hook.
 */
typedef struct keel_port_backend {
    /** sizeof(keel_port_backend) the port was compiled with. */
    uint32_t struct_size;
    /** ::KEEL_PORT_VERSION. */
    uint32_t port_version;
    /** ::keel_capability bits this backend provides. A capability declared
     *  without its hooks (SERVO: @c servo; RANGE: @c range_trigger and
     *  @c range_edges; RGB: @c i2c_write) is refused by keel_port_attach(). */
    uint32_t capabilities;
    /** Bitwise OR of ::keel_port_flag. */
    uint32_t flags;
    /** Passed unchanged to every hook. */
    void *user;

    /** @brief Required. Monotonic time in us; never decreases. */
    uint64_t (*now_us)(void *user);
    /** @brief Required. Sleep until the absolute time @c deadline_us; return
     *  early after @c cancel_sleep. Never called with a core lock held. */
    int32_t (*sleep_until_us)(void *user, uint64_t deadline_us);
    /** @brief Required. Read the logical inputs (::keel_port_input) into
     *  @c *out_inputs, setting ::KEEL_IN_BACKEND_FAULT while latched. */
    int32_t (*read_inputs)(void *user, uint32_t *out_inputs);
    /** @brief Required. Set the outputs selected by @c mask
     *  (::keel_port_output) to @c values. A rising ENABLE carrying an old
     *  @c stop_epoch is written LOW instead (see the file overview). */
    int32_t (*write_outputs)(void *user, uint32_t mask, uint32_t values, uint32_t stop_epoch);
    /** @brief Required. Apply @c *command. COAST is synchronous: all four
     *  lines are LOW when it returns. DRIVE/BRAKE may be generated
     *  asynchronously but end at @c valid_until_us without renewal. */
    int32_t (*write_motors)(void *user, const keel_port_motor_command *command);
    /** @brief Required. Drive ENABLE LOW and coast the motors now, taking only
     *  the backend's own lock, and record @c stop_epoch as the newest epoch.
     *  Callable from any thread at any time, concurrently with every other
     *  hook, including while another thread holds a core lock. */
    int32_t (*emergency_disable)(void *user, uint32_t stop_epoch);
    /** @brief Required. Copy the latched fault into @c *out (status ::KEEL_OK
     *  when nothing is latched). */
    int32_t (*get_fault)(void *user, keel_port_fault *out);
    /** @brief Required. Clear the fault latch; return a FAULT-class status if
     *  the condition persists. */
    int32_t (*reset_fault)(void *user);

    /** @brief Optional. Release the backend; called once by keel_close() (or
     *  by a failed keel_port_attach()) after every other hook has returned. */
    void (*destroy)(void *user);
    /** @brief Optional (with @c unlock). Take the ::keel_port_lock @c domain;
     *  priority inheritance is recommended. NULL requires
     *  ::KEEL_PORT_SINGLE_THREADED. */
    void (*lock)(void *user, uint32_t domain);
    /** @brief Optional (with @c lock). Release the ::keel_port_lock @c domain. */
    void (*unlock)(void *user, uint32_t domain);
    /** @brief Optional. Non-zero id of the calling OS thread, stable for its
     *  lifetime. NULL disables control-owner checks. */
    uint64_t (*thread_id)(void *user);
    /** @brief Optional. Wake every thread blocked in @c sleep_until_us; later
     *  sleeps return at once. Called once, when keel_close() starts. */
    void (*cancel_sleep)(void *user);
    /** @brief Optional. Copy the timing statistics into @c *out. */
    int32_t (*get_stats)(void *user, keel_port_stats *out);
    /** @brief Optional (::KEEL_CAP_RGB). One I2C write transaction of @c count
     *  bytes from @c data to register @c reg of 7-bit @c address. */
    int32_t (*i2c_write)(void *user, uint8_t address, uint8_t reg, const uint8_t *data,
                         size_t count);
    /** @brief Optional (::KEEL_CAP_SERVO). Apply @c *command; OFF is synchronous
     *  (pulse stopped, then request LOW when it returns). Backends without a
     *  servo hook keep any servo request line LOW at all times. */
    int32_t (*servo)(void *user, const keel_port_servo_command *command);
    /** @brief Optional (::KEEL_CAP_RANGE). Emit a TRIG pulse of at least 10 us,
     *  unless ECHO is HIGH: then set @c *out_echo_high to 1 and do nothing. */
    int32_t (*range_trigger)(void *user, uint32_t *out_echo_high);
    /** @brief Optional (::KEEL_CAP_RANGE). Drain up to @c capacity ECHO edges
     *  into @c edges without blocking; @c *out_count receives the number. */
    int32_t (*range_edges)(void *user, keel_port_edge *edges, uint32_t capacity,
                           uint32_t *out_count);
    /** @brief Optional (simulator). Apply a ::keel_sim_item stimulus. */
    int32_t (*sim_set)(void *user, uint32_t item, int32_t value);
    /** @brief Optional (simulator). Fill @c *out with the ground truth. */
    int32_t (*sim_get_state)(void *user, keel_sim_state *out);
} keel_port_backend;

/**
 * @brief Bytes of caller storage keel_port_attach() needs for one device.
 * @return Storage size in bytes.
 * @since 1.0
 */
size_t keel_port_storage_size(void);

/**
 * @brief Required alignment of the storage passed to keel_port_attach().
 * @return Alignment in bytes (a power of two).
 * @since 1.0
 */
size_t keel_port_storage_align(void);

/**
 * @brief Attach the core to caller storage and a backend; returns a normal
 *        handle.
 *
 * Validates the table (@c struct_size, @c port_version, required hooks,
 * capability hooks, lock hooks or ::KEEL_PORT_SINGLE_THREADED), resets the
 * backend fault latch, drives every output to its safe level and initialises
 * the LED driver when ::KEEL_CAP_RGB is declared (a failure clears it, as in
 * keel_open()). keel_close() later stops the device, calls @c destroy and
 * never frees @p storage. Not exported from libkeel.so.
 *
 * @param[in]  storage      Caller storage, at least keel_port_storage_size()
 *                          bytes, aligned to keel_port_storage_align().
 * @param[in]  storage_size Size of @p storage in bytes.
 * @param[in]  backend      Backend table; copied during the call.
 * @param[in]  options      Options or NULL; @c backend must be 0 and the
 *                          path fields empty.
 * @param[out] out_device   Receives the handle; NULL on failure.
 * @param[out] report       Optional outcome, as for keel_open().
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] Bad storage, size, alignment,
 *         table, options or report.
 * @retval KEEL_E_NO_MEMORY        [SETUP] No free handle slot.
 * @retval KEEL_E_SETUP_IO         [SETUP] Driving the safe output levels failed.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
int32_t keel_port_attach(void *storage, size_t storage_size, const keel_port_backend *backend,
                         const keel_open_options *options, keel_device **out_device,
                         keel_open_report *report);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* KEEL_PORT_H */
