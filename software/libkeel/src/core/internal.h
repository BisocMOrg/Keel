/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file internal.h
 * @brief Private declarations shared by the portable core modules.
 *
 * The core is portable C11: it makes no system calls, allocates nothing and
 * creates no threads. Clocks, sleeps, locks and thread ids come from the
 * backend table (port.h); the handle registry's lock comes from src/os.
 *
 * Lock domains (SPEC section 8): fields marked CONTROL, LEDS or RANGE are
 * only touched with that domain held. No path holds two domains at once and
 * nothing sleeps while holding CONTROL; the few values one domain publishes
 * to another (READY drops, the latest echo rise) are atomics.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#ifndef KEEL_CORE_INTERNAL_H
#define KEEL_CORE_INTERNAL_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "keel/port.h"

/** @brief Converts milliseconds to microseconds as uint64_t. */
#define KEEL__MS(ms) ((uint64_t)(ms) * UINT64_C(1000))

/** @brief Internal timing and protocol constants not in the public header. */
enum keel__constant {
    /** PWM bytes in one LED driver frame (six LEDs, three channels each). */
    KEEL__RGB_BYTES = 3 * KEEL_LED_COUNT,
    /** The MCU's heartbeat window: no fall for this long means it stopped. */
    KEEL__MCU_HEARTBEAT_MS = 80,
    /** READY LOW for this long while the heartbeat runs: the MCU is latched. */
    KEEL__READY_LOW_OFF_MS = 150,
    /** ARMED HIGH this long into HIGH1 is a stale completion (MCU ACK level
     *  100 ms minus the MCU clock tolerance). */
    KEEL__STALE_ACK_MS = 90,
    /** A stored command whose expiry is closer than this is coasted. */
    KEEL__EXPIRY_MARGIN_US = 1000,
    /** A raw button level must hold this long to be accepted. */
    KEEL__DEBOUNCE_MS = 20,
    /** No echo rise within this time of the trigger: the sensor is unpowered,
     *  locked out or unplugged (UNAVAILABLE). */
    KEEL__ECHO_RISE_MS = 5,
    /** Shortest valid echo pulse (20 mm at 343 m/s). */
    KEEL__ECHO_MIN_US = 117,
    /** Longest valid echo pulse (4 m at 343 m/s); longer is NO_ECHO. */
    KEEL__ECHO_MAX_US = 23300,
    /** An echo rise this recent makes sensor power CONFIRMED. */
    KEEL__ECHO_CONFIRM_MS = 500,
    /** ECHO edges drained from the backend per hook call. */
    KEEL__RANGE_EDGE_BATCH = 8
};

/** @brief Stored motion command and per-wheel reversal bookkeeping (CONTROL). */
typedef struct keel__motion {
    /** ::keel_port_motor_mode of the stored command. */
    uint32_t mode;
    /** Stored duties while driving; zero for COAST and BRAKE. */
    int32_t command[2];
    /** Backend time at which the stored DRIVE or BRAKE ends. */
    uint64_t expiry_us;
    /** Non-zero while a reversal waits for its dwell. */
    int pending;
    /** Duties of the pending reversal. */
    int32_t pending_command[2];
    /** Expiry of the pending reversal, from its acceptance. */
    uint64_t pending_expiry_us;
    /** Last driven direction per wheel: -1, 0 (never driven) or +1. */
    int32_t direction[2];
    /** Non-zero while the wheel is idle (coasting or braking). */
    int idle[2];
    /** Completion time of the write that made the wheel idle. */
    uint64_t idle_since_us[2];
    /** KEEL_SNAP_COMMAND_EXPIRED: the last command outlived its timeout. */
    int expired;
    /** Non-zero while both bridges coast. A brake is idle time for a
     *  reversal but PWM activity for the MCU, so the servo uses this. */
    int coasting;
    /** Completion time of the write that started the current coast. */
    uint64_t coast_since_us;
} keel__motion;

/** @brief Servo burst bookkeeping (CONTROL); a burst is active in KEEL_STATE_SERVO. */
typedef struct keel__servo {
    /** Earliest start of the next burst: 2200 ms after open, after the last
     *  READY-LOW sample and after the completion of the last burst's OFF. */
    uint64_t cooldown_until_us;
    /** Completion time of the REQUEST write; the grant is due within 10 ms. */
    uint64_t requested_us;
    /** Lease end passed to the backend; never extended. */
    uint64_t lease_end_us;
    /** Pulse width driven once the grant is confirmed. */
    uint32_t pulse_us;
    /** ARMED = 0 was seen after the request and the pulse was started. */
    int granted;
    /** A REQUEST was written and no OFF since: the next OFF ends a burst. */
    int active;
} keel__servo;

/** @brief Ultrasonic measurement state (RANGE). */
typedef struct keel__range {
    /** Latest result, full size, as returned by keel_range_poll(). */
    keel_range result;
    /** A measurement is in progress (result.status is PENDING). */
    int pending;
    /** Backend time just before the trigger of the pending measurement. */
    uint64_t trigger_us;
    /** The pending measurement's echo has risen. */
    int risen;
    /** Time of that rise. */
    uint64_t rise_us;
    /** CONTROL's READY-drop count at the trigger. */
    uint32_t ready_drops;
    /** A trigger was issued since open (spacing applies). */
    int have_trigger;
    /** Time of the last trigger. */
    uint64_t last_trigger_us;
} keel__range;

/** @brief Debounce state (CONTROL). */
typedef struct keel__buttons {
    /** Time at which each button's raw level last changed. */
    uint64_t since_us[KEEL_BUTTON_COUNT];
    /** Debounced levels (::keel_button bits). */
    uint32_t held;
    /** Raw levels seen at the last sample. */
    uint32_t candidate;
    /** Press edges not yet taken. */
    uint32_t pressed;
    /** Release edges not yet taken. */
    uint32_t released;
} keel__buttons;

/** @brief One device: the caller's storage for a port, the backend's for a hosted backend. */
typedef struct keel__device {
    /* Immutable after attach. */
    keel_port_backend be;          /**< Copy of the backend table. */
    uint32_t backend_kind;         /**< ::keel_backend reported in the open report. */
    uint32_t caps;                 /**< ::keel_capability bits offered. */
    uint64_t command_timeout_us;   /**< Lifetime of an accepted drive or brake. */
    int single_threaded;           /**< ::KEEL_PORT_SINGLE_THREADED with a thread_id hook. */
    uint64_t attach_thread;        /**< Thread id of the attaching thread. */

    /* Shared across threads without a domain lock. */
    _Atomic uint32_t stop_epoch;   /**< Bumped by every stop call before it locks. */
    _Atomic int closing;           /**< Set when keel_close() starts. */
    _Atomic int32_t last_rejection; /**< Status of the most recent REJECTED call. */
    _Atomic uint32_t ready_drops;  /**< READY HIGH-to-LOW samples (CONTROL writes, RANGE reads). */
    _Atomic int echo_seen;         /**< An echo rose since open (RANGE writes, CONTROL reads). */
    _Atomic uint64_t echo_rise_us; /**< Time of the latest echo rise, valid once echo_seen. */

    /* CONTROL domain. */
    uint32_t handled_epoch;        /**< Newest stop epoch whose effect was applied. */
    uint32_t stopped_state;        /**< ::keel_state in which that epoch was applied. */
    uint32_t stopped_ack;          /**< ::keel_ack_phase in which that epoch was applied. */
    int in_stop_call;              /**< A stop call's own CONTROL section is running. */
    uint32_t state;                /**< ::keel_state. */
    uint32_t ack_phase;            /**< ::keel_ack_phase. */
    uint32_t outputs;              /**< Levels last written (KEEL_OUT_*). */
    uint32_t inputs;               /**< Last sample: KEEL_IN_READY, ARMED, buttons. */
    uint64_t inputs_sampled_us;    /**< Time of that sample. */
    uint64_t observed_us;          /**< Latest clock value, for the monotonicity check. */
    int have_tick;                 /**< A tick was accepted since open or clear. */
    uint64_t last_tick_us;         /**< Time of the last accepted tick. */
    int have_cycle;                /**< A cycle id was accepted since open. */
    uint64_t last_cycle_id;        /**< Last accepted cycle id. */
    int have_fall;                 /**< A heartbeat fall happened since the last (re)start. */
    uint64_t last_fall_us;         /**< Time of the last heartbeat fall. */
    uint64_t hb_not_before_us;     /**< Earliest first pulse after open or clear. */
    int hb_high;                   /**< The heartbeat line is HIGH (pulse in progress). */
    int hb_strict;                 /**< The previous tick ran while ARMING, ARMED or SERVO. */
    int hb_restarted;              /**< The most recent tick restarted the heartbeat. */
    uint32_t hb_falls;             /**< Falls since the heartbeat (re)started. */
    uint32_t heartbeat_restarts;   /**< Restarts after gaps while DISARMED, since open. */
    uint64_t ack_started_us;       /**< Start of the arming acknowledgement. */
    uint64_t phase_at_us;          /**< Completion of the current level's write. */
    int stale_used;                /**< The one stale-ACK restart of this arm was used. */
    uint32_t stale_ack_restarts;   /**< Stale-ACK restarts since open. */
    uint64_t enable_low_since_us;  /**< Completion of the last ENABLE LOW write. */
    int ready_low;                 /**< READY was LOW at the last sample. */
    uint64_t ready_low_since_us;   /**< First sample of the current READY LOW run. */
    int have_owner;                /**< A control owner is recorded. */
    uint64_t owner_thread;         /**< Its thread id. */
    keel_fault record;             /**< First fault since the last clear (or the final record). */
    uint32_t fault_count;          /**< Latches since open. */
    int have_latch;                /**< A fault latched since open (re-arm hold-off). */
    uint64_t latched_at_us;        /**< Time of the last latch. */
    keel__motion motion;           /**< Stored command and reversal state. */
    keel__servo servo;             /**< Servo burst state. */
    keel__buttons buttons;         /**< Debounce state. */
    uint32_t button_leds;          /**< Lit button LEDs (::keel_button bits). */

    /* LEDS domain. */
    uint8_t rgb[KEEL__RGB_BYTES];  /**< Frame last written successfully. */

    /* RANGE domain. */
    keel__range range;             /**< Ultrasonic measurement state. */
} keel__device;

/** @brief A counted reference to a registry entry, held for one call. */
typedef struct keel__ref {
    keel__device *device; /**< The device. */
    uint32_t slot;        /**< Registry slot. */
} keel__ref;

/* ---- registry.c: generation-checked handles ---------------------------- */

/** @brief Publishes @p device and returns its token; KEEL_E_NO_MEMORY when full. */
int32_t keel__registry_add(keel__device *device, keel_device **out_handle);
/** @brief Validates @p handle (owner pid first, without a lock) and takes a
 *  reference. With @p for_close the entry is marked closing, so new calls
 *  get KEEL_E_INVALID_HANDLE; a second close does too. */
int32_t keel__acquire(const keel_device *handle, keel__ref *ref, int for_close);
/** @brief Drops a reference taken by keel__acquire(). */
void keel__release(const keel__ref *ref);
/** @brief Close only: waits until no other call holds a reference, then
 *  frees the slot and bumps its generation. */
void keel__registry_retire(const keel__ref *ref);

/* ---- abi.c: struct versioning rules ------------------------------------ */

/** @brief Input struct rule: NULL, below @p known or above
 *  KEEL_STRUCT_SIZE_MAX is INVALID_ARGUMENT; a
 *  larger struct is accepted only with a zero tail, else UNSUPPORTED. */
int32_t keel__in_check(const void *in, size_t known);
/** @brief Output struct rule: non-NULL with struct_size from @p known to
 *  KEEL_STRUCT_SIZE_MAX. */
int keel__out_ok(const void *out, size_t known);
/** @brief Writes min(struct_size, @p known) bytes of @p src (never the
 *  struct_size field) and zeroes the rest of @p out. */
void keel__out_write(void *out, const void *src, size_t known);

/* ---- attach.c: options and open reports -------------------------------- */

/** @brief Zeroes a full-size report and sets its struct_size and line (-1). */
void keel__report_init(keel_open_report *report);
/** @brief Validates @p in (NULL means defaults) into the full-size @p out;
 *  KEEL_E_INVALID_ARGUMENT (with the report's resource and hint) on error. */
int32_t keel__options_check(const keel_open_options *in, keel_open_options *out,
                            keel_open_report *report);

/* ---- fault.c: status tables ------------------------------------------- */

/** @brief Non-zero for a cause that stays latched until close. */
int keel__cause_sticky(uint32_t cause);
/** @brief Non-zero for a defined FAULT-class status: the only non-OK values
 *  a safety-path hook may pass through (SPEC 12.2). */
int keel__is_fault_status(int32_t status);

/* ---- safety.c: locks, faults, heartbeat, arming, stops ---------------- */

/** @brief Takes a domain lock through the backend (no-op without lock hooks). */
void keel__lock(keel__device *dev, uint32_t domain);
/** @brief Releases a domain lock. */
void keel__unlock(keel__device *dev, uint32_t domain);
/** @brief Opens a CONTROL section: locks, refuses when close has started
 *  (KEEL_E_INVALID_HANDLE, lock not held) and applies a pending stop. */
int32_t keel__control_enter(keel__device *dev);
/** @brief Closes a CONTROL section. */
void keel__control_leave(keel__device *dev);
/** @brief Records a REJECTED status in the snapshot and returns it. */
int32_t keel__reject(keel__device *dev, int32_t status);
/** @brief Non-zero when the calling thread may issue go calls. */
int keel__is_owner(const keel__device *dev);
/** @brief Non-zero unless a single-threaded port is called from a foreign thread. */
int keel__thread_allowed(const keel__device *dev);
/** @brief Clock read with the monotonicity check (step back latches CLOCK). */
int32_t keel__clock(keel__device *dev, uint64_t *now);
/** @brief Clock read for timestamps, clamped to the last observed value. */
uint64_t keel__now(const keel__device *dev);
/** @brief read_inputs plus the backend-latch check (the latch has priority). */
int32_t keel__sample(keel__device *dev);
/** @brief Stamps the last sample and tracks READY LOW runs (READY drops for
 *  ranging, the servo cooldown). */
void keel__note_inputs(keel__device *dev, uint64_t now);
/** @brief keel__sample(), the checked clock and keel__note_inputs() in one:
 *  the start of every control-I/O section. */
int32_t keel__fresh_inputs(keel__device *dev, uint64_t *now);
/** @brief Latches FAULTED (first fault wins) after a Category-0 stop. */
int32_t keel__latch(keel__device *dev, int32_t status, uint32_t cause, int32_t os_error,
                    uint64_t detail);
/** @brief Latches a failed safety-path hook by the pass-through rule. */
int32_t keel__hook_failed(keel__device *dev, int32_t rc, uint32_t cause, uint64_t detail);
/** @brief Latches with the backend's own fault record. */
int32_t keel__latch_backend(keel__device *dev);
/** @brief Fails with TICK_LATE when the 70 ms tick window has closed. */
int32_t keel__window(keel__device *dev, uint64_t now);
/** @brief Writes ENABLE LOW (current epoch) and updates its shadow; a
 *  failure latches by the pass-through rule. */
int32_t keel__enable_low(keel__device *dev);
/** @brief Enters DISARMED: no acknowledgement, stored motion dropped. */
void keel__enter_disarmed(keel__device *dev);
/** @brief Bumps the stop epoch and lowers ENABLE through the backend, before any lock. */
int32_t keel__emergency_disable(keel__device *dev);
/** @brief The safe-stop sequence; every step is attempted. */
int32_t keel__safe_stop(keel__device *dev, int stop_heartbeat, int32_t *os_error);
/** @brief keel_tick() / keel_cycle() body (takes and releases CONTROL itself). */
int32_t keel__tick(keel__device *dev, uint64_t cycle_id, uint32_t health,
                   const keel_cycle_input *in, keel_snapshot *out);
/** @brief keel_request_arm() body (CONTROL held). */
int32_t keel__request_arm(keel__device *dev);
/** @brief keel_disarm() body (CONTROL held, ENABLE already lowered). */
int32_t keel__disarm(keel__device *dev, int32_t disable_rc);
/** @brief keel_emergency_stop() body (CONTROL held, ENABLE already lowered). */
int32_t keel__emergency_stop(keel__device *dev, int32_t disable_rc);
/** @brief keel_clear_fault() body (CONTROL held). */
int32_t keel__clear_fault(keel__device *dev);
/** @brief keel_close() stop (CONTROL held, ENABLE already lowered). */
int32_t keel__close_stop(keel__device *dev, int32_t disable_rc);

/* ---- motion.c: drive, coast, brake, lease ----------------------------- */

/** @brief Starts motion bookkeeping at open: coasting and idle since @p now. */
void keel__motion_init(keel__device *dev, uint64_t now);
/** @brief Coasts both motors synchronously and marks the wheels idle; returns the hook status. */
int32_t keel__coast_write(keel__device *dev);
/** @brief Non-zero while a motor is driven or braked or a reversal is pending. */
int keel__motion_active(const keel__device *dev);
/** @brief Fresh control I/O before motion or a servo request (SPEC 13.1
 *  step 11): inputs and backend latch, clock, tick window, READY, ARMED. */
int32_t keel__armed_io(keel__device *dev, uint64_t *now);
/** @brief Forgets the stored command, pending reversal and brake. */
void keel__motion_drop(keel__device *dev);
/** @brief Refused motion request: while ARMED coast and drop the command. */
int32_t keel__motion_refuse(keel__device *dev, int32_t status);
/** @brief set_motors(0, 0): coast and drop while ARMED, otherwise nothing. */
int32_t keel__motion_zero(keel__device *dev);
/** @brief Non-zero set_motors; @p tick_now is the cycle's tick time or NULL for a fresh read. */
int32_t keel__motion_set(keel__device *dev, int32_t left, int32_t right, const uint64_t *tick_now);
/** @brief keel_brake(); @p tick_now as for keel__motion_set(). */
int32_t keel__motion_brake(keel__device *dev, const uint64_t *tick_now);
/** @brief Armed tick: pending reversal, expiry and lease renewal. */
int32_t keel__motion_tick(keel__device *dev, uint64_t now);
/** @brief Duty in range: -KEEL_DUTY_MAX..KEEL_DUTY_MAX. */
int keel__duty_valid(int32_t duty);

/* ---- servo.c: J5 bursts (CONTROL held) --------------------------------- */

/** @brief Starts the cooldown that runs from open. */
void keel__servo_init(keel__device *dev, uint64_t now);
/** @brief keel_servo_start() after its argument and capability checks. */
int32_t keel__servo_start(keel__device *dev, uint32_t pulse_us, uint32_t lease_ms);
/** @brief keel_servo_set_pulse() after its argument and capability checks. */
int32_t keel__servo_set_pulse(keel__device *dev, uint32_t pulse_us);
/** @brief keel_servo_end(): ends an active burst; a no-op otherwise. */
int32_t keel__servo_end(keel__device *dev);
/** @brief SERVO tick after the READY check: lease end, grant, dual permit. */
int32_t keel__servo_tick(keel__device *dev, uint64_t now);
/** @brief Servo OFF (pulse, then request LOW) where a servo hook exists.
 *  An OFF that ends a burst restarts the cooldown from its completion.
 *  @return The raw hook status. */
int32_t keel__servo_off(keel__device *dev);
/** @brief A READY-LOW sample restarts the cooldown (the MCU may have reset). */
void keel__servo_ready_low(keel__device *dev, uint64_t now);

/* ---- range.c: HC-SR04 classification (RANGE held) ---------------------- */

/** @brief Starts with an IDLE result. */
void keel__range_init(keel__device *dev);
/** @brief Drains ECHO edges and classifies a pending measurement.
 *  @return KEEL_OK or KEEL_E_PERIPHERAL (the measurement becomes INVALID). */
int32_t keel__range_update(keel__device *dev);
/**
 * @brief keel_range_start() after the capability check.
 * @param[in] dev   Device.
 * @param[in] power ::keel_sensor_power inferred under CONTROL just before.
 * @return KEEL_OK (triggered, or an immediate UNAVAILABLE), KEEL_E_AGAIN or
 *         KEEL_E_PERIPHERAL.
 */
int32_t keel__range_start(keel__device *dev, uint32_t power);
/** @brief Copies the latest result (full size). */
void keel__range_result(const keel__device *dev, keel_range *out);
/** @brief Ends a pending measurement whose wait is over (keel_range_measure()
 *  at its 35 ms cap, keel_close()): NO_ECHO if the echo rose, else UNAVAILABLE. */
void keel__range_expire(keel__device *dev);

/* ---- buttons.c ---------------------------------------------------------- */

/** @brief Starts debouncing from the levels in dev->inputs. */
void keel__buttons_init(keel__device *dev, uint64_t now);
/** @brief Debounces dev->inputs at @p now (20 ms). */
void keel__buttons_debounce(keel__device *dev, uint64_t now);

/* ---- leds.c: IS31FL3218-compatible protocol (LEDS held) ------------- */

/** @brief Initialises the driver dark; KEEL_E_PERIPHERAL on failure. */
int32_t keel__leds_init(keel__device *dev);
/** @brief Writes one frame and latches it; updates the cache on success. */
int32_t keel__leds_frame(keel__device *dev, const uint8_t frame[KEEL__RGB_BYTES]);
/** @brief Puts the driver into shutdown (all LEDs off). */
int32_t keel__leds_shutdown(keel__device *dev);

/* ---- snapshot.c ---------------------------------------------------------- */

/** @brief Fills a full-size snapshot from cached state (CONTROL held). */
void keel__snapshot(const keel__device *dev, keel_snapshot *snapshot);
/** @brief SPEC 9.6 sensor-power inference from cached state (CONTROL held). */
uint32_t keel__sensor_power(const keel__device *dev, uint64_t now);

#endif /* KEEL_CORE_INTERNAL_H */
