/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file fixture.h
 * @brief Reference-model fixture: the core attached through port.h to the
 *        simulator's board model (production MCU policy stepped every
 *        simulated millisecond) on a virtual clock, plus fault and delay
 *        injection.
 *
 * Injections count hook calls: fail_x = n makes the n-th call of hook x fail
 * with fail_status (KEEL_E_IO when zero). A FAULT-class failure latches a
 * backend record first, as a real backend must; any other status is
 * returned bare, as a backend that breaks the port contract would.
 * delay_x blocks inside the next matching call by advancing the virtual
 * clock.
 *
 * Options (set before fx_attach()) add the optional hooks: FX_THREADS gives
 * lock hooks and a thread_id hook that reports @c thread, so tests can play
 * several threads deterministically from one; FX_SERVO and FX_RANGE add the
 * servo and HC-SR04 hooks of the board model; FX_INVARIANTS checks the
 * random-walk invariants on every simulated millisecond.
 */
#ifndef KEEL_TEST_FIXTURE_H
#define KEEL_TEST_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#include "backend/sim/sim_board.h"
#include "keel/port.h"

/** @brief Bits of fixture::options. */
enum fx_option {
    FX_THREADS = 1,   /**< Lock hooks and a thread_id hook returning fixture::thread. */
    FX_SERVO = 2,     /**< KEEL_CAP_SERVO with the board's servo hook. */
    FX_RANGE = 4,     /**< KEEL_CAP_RANGE with the board's HC-SR04 hooks. */
    FX_INVARIANTS = 8 /**< Per-millisecond board invariants (fx_advance()). */
};

/** @brief One logged I2C transaction. */
typedef struct transaction {
    uint8_t address;  /**< 7-bit address. */
    uint8_t reg;      /**< Register. */
    uint8_t data[18]; /**< Payload (up to one frame). */
    size_t count;     /**< Payload length. */
} transaction;

/** @brief The fixture; zero-initialised by fx_prepare(). */
typedef struct fixture {
    keel__sim_board board;       /**< Board model and MCU. */
    keel_device *device;         /**< Handle while attached. */
    void *storage;               /**< Core storage. */
    unsigned options;            /**< ::fx_option bits used by fx_attach(). */
    uint64_t thread;             /**< Id reported by the thread_id hook (FX_THREADS). */
    uint64_t cycle;              /**< Last cycle id used by fx_tick(). */
    uint32_t force_low;          /**< READY/ARMED bits forced LOW in reads. */
    uint32_t force_high;         /**< READY/ARMED bits forced HIGH in reads. */
    uint64_t falls[1024];        /**< Heartbeat fall times. */
    size_t nfalls;               /**< Entries in falls; long FX_INVARIANTS runs stop logging when full. */
    size_t fall_count;           /**< Heartbeat falls since attach. */
    uint64_t last_fall_us;       /**< Time of the latest fall. */
    uint64_t enable_changes[64]; /**< ENABLE edge times. */
    size_t nenable_changes;      /**< Entries in enable_changes (logged like falls). */
    unsigned enable_rises;       /**< ENABLE rising edges. */
    transaction writes[128];     /**< I2C log (logged like falls). */
    size_t nwrites;              /**< I2C transactions seen (failed ones too); fail_i2c counts these. */
    unsigned motor_calls, output_calls, read_calls, sleep_calls, disable_calls; /**< Hook calls. */
    unsigned servo_calls, trigger_calls, edges_calls; /**< Optional hook calls. */
    unsigned get_fault_calls, reset_calls; /**< Fault-latch hook calls. */
    unsigned fail_motor, fail_output, fail_read, fail_i2c, fail_sleep, fail_disable; /**< Call number to fail. */
    unsigned fail_servo, fail_trigger, fail_edges; /**< Call number to fail (optional hooks). */
    unsigned fail_get_fault, fail_reset; /**< Call number to fail (fault-latch hooks). */
    int32_t fail_status;         /**< Status of an injected failure; 0 means KEEL_E_IO. */
    unsigned latch_after_read;   /**< Read number after which a PWM overrun latches. */
    uint32_t extra_sleep;        /**< Added to every sleep, us. */
    uint32_t delay_enable;       /**< Delay inside the next ENABLE write, us. */
    uint32_t delay_motor_zero;   /**< Delay before the next COAST, us. */
    uint32_t delay_motor_return; /**< Delay after the next motor write, us. */
    uint32_t delay_read;         /**< Delay inside the next read, us. */
    uint32_t mcu_trips;          /**< MCU fault bits of every latch since the last fx_take_trips(). */
    uint64_t mcu_trip_us;        /**< Time of the latest MCU latch. */
} fixture;

/** @brief Resets @p f with buttons @p held and fresh MCU state; allocates storage. */
void fx_prepare(fixture *f, uint32_t held);
/** @brief keel_port_attach() with the fixture backend. */
int32_t fx_attach(fixture *f, keel_open_report *report);
/** @brief fx_prepare() + successful attach, DISARMED, outputs safe. */
void fx_init_held(fixture *f, uint32_t held);
/** @brief fx_init_held() with no button held. */
void fx_init(fixture *f);
/** @brief fx_prepare() with @p options + successful attach. */
void fx_init_options(fixture *f, unsigned options);
/** @brief keel_close() (must succeed) and release the storage. */
void fx_destroy(fixture *f);
/** @brief Advances the virtual clock to @p target_us, checking the MCU every ms. */
void fx_advance(fixture *f, uint64_t target_us);
/** @brief Current virtual time. */
uint64_t fx_now(const fixture *f);
/** @brief keel_tick() with the next cycle id and full health. */
int32_t fx_tick(fixture *f);
/** @brief Advances by @p gap_us, then a tick that must succeed. */
void fx_tick_after(fixture *f, uint64_t gap_us);
/** @brief keel_get_state(). */
uint32_t fx_state(fixture *f);
/** @brief keel_get_snapshot(). */
keel_snapshot fx_snapshot(fixture *f);
/** @brief keel_get_fault(). */
keel_fault fx_fault(fixture *f);
/** @brief request_arm, then 10 ms ticks until ARMED (at most 1.6 s). */
void fx_arm(fixture *f);
/** @brief clear_fault, 1 s of ticks (re-arm hold-off), then fx_arm(). */
void fx_recover(fixture *f);
/** @brief ENABLE and heartbeat LOW and both bridges coasting. */
void fx_safe(const fixture *f);
/** @brief Ten ticks give FAULTED and no fall; disarm re-runs the stop. */
void fx_no_more_heartbeat(fixture *f);
/** @brief Left duty being generated (0 unless DRIVE). */
int32_t fx_left(const fixture *f);
/** @brief Right duty being generated (0 unless DRIVE). */
int32_t fx_right(const fixture *f);
/** @brief Returns and clears fixture::mcu_trips. */
uint32_t fx_take_trips(fixture *f);

#endif /* KEEL_TEST_FIXTURE_H */
