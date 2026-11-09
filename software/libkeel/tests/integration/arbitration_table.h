/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file arbitration_table.h
 * @brief The libkeel 1.0 request-versus-state arbitration table as C data.
 *
 * One row per public call, and per precondition that changes the outcome;
 * one cell per (state when the call starts) x (calling thread). The OWNER
 * column is the control owner, or any thread while no owner is recorded; the
 * OTHER column is any other thread of the same process while an owner is
 * recorded. Each cell gives the status, the state after the call, the side
 * effects and whether the heartbeat keeps running afterwards (given on-time
 * ticks). Cells flagged ::KEEL_ARB_UNREACHABLE cannot occur (for example a
 * sticky fault outside FAULTED); they carry the status the evaluation order
 * would give and are not replayed.
 *
 * The cells assume nominal I/O. Injected failures are covered by the
 * error-propagation matrix, and invalid or closed handles (always
 * ::KEEL_E_INVALID_HANDLE, nothing touched) by the handle tests.
 *
 * Consumers: the arbitration integration test replays every reachable cell
 * against the simulator, and tests/docs/arbitration_render.c prints the table
 * that docs/spec.md and docs/safety.md carry, so the documents and the tests
 * share one source.
 */
#ifndef KEEL_TEST_ARBITRATION_TABLE_H
#define KEEL_TEST_ARBITRATION_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "keel/keel.h"

/** @brief Thread columns of the table. */
enum keel_arb_thread {
    /** The control owner, or any thread while no owner is recorded. */
    KEEL_ARB_OWNER = 0,
    /** Another thread of the same process while an owner is recorded. */
    KEEL_ARB_OTHER = 1,
    /** Number of thread columns. */
    KEEL_ARB_THREAD_COUNT = 2
};

/** @brief Table dimensions and the pseudo-state used after keel_close(). */
enum keel_arb_dimension {
    /** State columns: DISARMED, ARMING, ARMED, SERVO, FAULTED (::keel_state order). */
    KEEL_ARB_STATE_COUNT = 5,
    /** keel_arb_cell::state_after value meaning "handle closed". */
    KEEL_ARB_CLOSED = 5
};

/** @brief Side-effect bits of keel_arb_cell::effects. A REJECTED status
 *  additionally updates keel_snapshot::last_rejection, which is not listed. */
enum keel_arb_effect {
    /** No side effect. */
    KEEL_ARB_FX_NONE = 0,
    /** Inputs sampled and buttons debounced. */
    KEEL_ARB_FX_SAMPLE = 1 << 0,
    /** Heartbeat pulse when due (falls at least 30 ms apart). */
    KEEL_ARB_FX_PULSE = 1 << 1,
    /** Heartbeat restarted after a gap; heartbeat_restarts incremented. */
    KEEL_ARB_FX_HB_RESTART = 1 << 2,
    /** Arming acknowledgement starts (ENABLE LOW and coast re-asserted) or advances. */
    KEEL_ARB_FX_ACK = 1 << 3,
    /** Motor lease renewed, an expired command coasted, a due reversal applied. */
    KEEL_ARB_FX_LEASE = 1 << 4,
    /** Servo grant checked; pulse started once granted; burst ended at lease end. */
    KEEL_ARB_FX_GRANT = 1 << 5,
    /** Synchronous coast; stored command, timeout, pending reversal and brake dropped. */
    KEEL_ARB_FX_COAST = 1 << 6,
    /** Fresh READY/ARMED check; command stored with its timeout and driven. */
    KEEL_ARB_FX_DRIVE = 1 << 7,
    /** Fresh READY/ARMED check; both motors coast now; command pending for the dwell. */
    KEEL_ARB_FX_REVERSAL = 1 << 8,
    /** Fresh READY/ARMED check; all four bridge inputs HIGH with a timeout. */
    KEEL_ARB_FX_BRAKE = 1 << 9,
    /** Category-0 sequence: ENABLE LOW first, then coast and servo off. */
    KEEL_ARB_FX_STOP = 1 << 10,
    /** Heartbeat driven LOW and stopped. */
    KEEL_ARB_FX_HB_STOP = 1 << 11,
    /** Fault record written; state FAULTED. */
    KEEL_ARB_FX_LATCH = 1 << 12,
    /** Backend latch reset, record flagged CLEARED, control owner forgotten. */
    KEEL_ARB_FX_CLEAR = 1 << 13,
    /** Fresh READY/ARMED check; servo request raised; stored motion dropped. */
    KEEL_ARB_FX_SERVO_REQUEST = 1 << 14,
    /** Servo pulse off, request LOW, ENABLE LOW; cooldown restarts. */
    KEEL_ARB_FX_SERVO_END = 1 << 15,
    /** Servo pulse width updated (lease unchanged). */
    KEEL_ARB_FX_SERVO_PULSE = 1 << 16,
    /** TRIG pulse; result PENDING. */
    KEEL_ARB_FX_TRIGGER = 1 << 17,
    /** Immediate UNAVAILABLE result; TRIG not driven. */
    KEEL_ARB_FX_UNAVAILABLE = 1 << 18,
    /** Blocks up to 35 ms outside the control lock. */
    KEEL_ARB_FX_WAIT = 1 << 19,
    /** Latest range result copied out. */
    KEEL_ARB_FX_RESULT = 1 << 20,
    /** RGB LED I2C frame written. */
    KEEL_ARB_FX_I2C = 1 << 21,
    /** Button LED lines written. */
    KEEL_ARB_FX_BUTTON_LEDS = 1 << 22,
    /** Fresh read of READY, ARMED and the buttons. */
    KEEL_ARB_FX_READ = 1 << 23,
    /** Button edges consumed. */
    KEEL_ARB_FX_TAKE = 1 << 24,
    /** Simulator stimulus applied or ground truth read. */
    KEEL_ARB_FX_SIM = 1 << 25,
    /** LEDs off, waiters woken, handle released, backend destroyed. */
    KEEL_ARB_FX_RELEASE = 1 << 26,
    /** Sleeps until the deadline outside every lock. */
    KEEL_ARB_FX_SLEEP = 1 << 27,
    /** Snapshot flag REARM_HOLDOFF reported. */
    KEEL_ARB_FX_HOLDOFF = 1 << 28,
    /** Number of effect bits. */
    KEEL_ARB_FX_BITS = 29,
    /** Fail-stop: sample, Category-0 stop, heartbeat stopped, fault latched. */
    KEEL_ARB_FX_FAILSTOP = KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP |
                           KEEL_ARB_FX_LATCH
};

/** @brief Bits of keel_arb_cell::flags. */
enum keel_arb_cell_flag {
    /** The cell's precondition cannot hold in this state; not replayed. */
    KEEL_ARB_UNREACHABLE = 1,
    /** The precondition means the loop has stalled: the state is the cached
     *  one, the heartbeat has stopped and the next tick latches, whatever
     *  this call does. */
    KEEL_ARB_LOOP_STALLED = 2
};

/** @brief Expected outcome of one call in one state from one thread. */
typedef struct keel_arb_cell {
    /** Expected ::keel_status. */
    int32_t status;
    /** ::keel_fault_cause this call records; ::KEEL_CAUSE_NONE when the fault
     *  record is left unchanged. */
    uint32_t cause;
    /** Expected ::keel_state after the call, or ::KEEL_ARB_CLOSED. */
    uint32_t state_after;
    /** Bitwise OR of ::keel_arb_effect. */
    uint32_t effects;
    /** 1 if the heartbeat keeps running after the call, 0 if it is stopped. */
    uint32_t heartbeat;
    /** Bitwise OR of ::keel_arb_cell_flag. */
    uint32_t flags;
} keel_arb_cell;

/** @brief One request (call plus precondition) across every state and thread. */
typedef struct keel_arb_row {
    /** Public call(s) the row covers. */
    const char *call;
    /** Precondition that selects this row. */
    const char *condition;
    /** Clarification printed under the table, or NULL. */
    const char *note;
    /** Cells indexed by [::keel_state][::keel_arb_thread]. */
    keel_arb_cell cells[KEEL_ARB_STATE_COUNT][KEEL_ARB_THREAD_COUNT];
} keel_arb_row;

/** @brief Short names of the ::keel_arb_effect bits, indexed by bit number. */
static const char *const keel_arb_effect_names[KEEL_ARB_FX_BITS] = {
    "sample",        "pulse",         "hb restart",  "ack",         "lease",
    "grant",         "coast",         "drive",       "reversal",    "brake",
    "Cat-0 stop",    "hb stop",       "latch",       "clear",       "servo request",
    "servo end",     "pulse width",   "trigger",     "unavailable", "wait <= 35 ms",
    "result",        "I2C frame",     "button LEDs", "read",        "take edges",
    "sim",           "release",       "sleep",       "hold-off"};

/* Cell builders, local to this header. */
#define KEEL_ARB_C(status, state, fx, hb) {(status), KEEL_CAUSE_NONE, (state), (fx), (hb), 0u}
#define KEEL_ARB_NEVER(status, state, fx, hb) \
    {(status), KEEL_CAUSE_NONE, (state), (fx), (hb), KEEL_ARB_UNREACHABLE}
/* A call made while the loop has stalled; the heartbeat is already gone. */
#define KEEL_ARB_STALLED(status, state, fx) \
    {(status), KEEL_CAUSE_NONE, (state), (fx), 0u, KEEL_ARB_LOOP_STALLED}
/* A call that latches FAULTED with a new record. */
#define KEEL_ARB_L(status, cause, fx) {(status), (cause), KEEL_STATE_FAULTED, (fx), 0u, 0u}
#define KEEL_ARB_BOTH(cell) {cell, cell}
#define KEEL_ARB_D KEEL_STATE_DISARMED
#define KEEL_ARB_A KEEL_STATE_ARMING
#define KEEL_ARB_R KEEL_STATE_ARMED
#define KEEL_ARB_S KEEL_STATE_SERVO
#define KEEL_ARB_F KEEL_STATE_FAULTED
/* A tick or cycle from a non-owner thread fail-stops. */
#define KEEL_ARB_FOREIGN_TICK \
    KEEL_ARB_L(KEEL_E_HEALTH, KEEL_CAUSE_TICK_THREAD_CHANGED, KEEL_ARB_FX_FAILSTOP)
/* Owner ticks that report bad health or a non-increasing cycle_id fail-stop. */
#define KEEL_ARB_BAD_HEALTH \
    KEEL_ARB_L(KEEL_E_HEALTH, KEEL_CAUSE_HEALTH_FLAGS, KEEL_ARB_FX_FAILSTOP)
#define KEEL_ARB_BAD_CYCLE \
    KEEL_ARB_L(KEEL_E_HEALTH, KEEL_CAUSE_CYCLE_NOT_INCREASING, KEEL_ARB_FX_FAILSTOP)
/* Any tick or cycle while FAULTED: inputs sampled, nothing emitted. */
#define KEEL_ARB_FAULTED_TICK KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_SAMPLE, 0u)
/* A go call refused because another thread owns the loop. */
#define KEEL_ARB_CONFLICT(state, hb) KEEL_ARB_C(KEEL_E_CONFLICT, state, KEEL_ARB_FX_NONE, hb)
/* A completed keel_range_measure(): READY confirmed, triggered, waited for. */
#define KEEL_ARB_FX_MEASURE (KEEL_ARB_FX_TRIGGER | KEEL_ARB_FX_WAIT | KEEL_ARB_FX_READ)

/** @brief The arbitration table. */
static const keel_arb_row keel_arb_table[] = {
    {"keel_tick",
     "on time, health ALL, cycle_id increasing",
     "ARMING becomes ARMED when the acknowledgement completes; SERVO becomes DISARMED when the "
     "lease ends. The first successful tick records the control owner.",
     {{KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_ACK, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_LEASE,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_GRANT,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_tick",
     "more than 70 ms after the previous tick, which ran in the same state",
     "A tick whose previous tick ran while DISARMED (the first after keel_request_arm) restarts "
     "the heartbeat instead of failing.",
     {{KEEL_ARB_C(KEEL_OK, KEEL_ARB_D,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_HB_RESTART, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_L(KEEL_E_DEADLINE, KEEL_CAUSE_TICK_LATE, KEEL_ARB_FX_FAILSTOP),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_L(KEEL_E_DEADLINE, KEEL_CAUSE_TICK_LATE, KEEL_ARB_FX_FAILSTOP),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_L(KEEL_E_DEADLINE, KEEL_CAUSE_TICK_LATE, KEEL_ARB_FX_FAILSTOP),
       KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_tick",
     "heartbeat fall lands more than 74 ms after the previous fall (previous tick in the same "
     "state)",
     "A fall predicted more than 70 ms after the previous one gives the same result before pulsing.",
     {{KEEL_ARB_C(KEEL_OK, KEEL_ARB_D,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_HB_RESTART, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_L(KEEL_E_DEADLINE, KEEL_CAUSE_HEARTBEAT_LATE,
                  KEEL_ARB_FX_FAILSTOP | KEEL_ARB_FX_PULSE),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_L(KEEL_E_DEADLINE, KEEL_CAUSE_HEARTBEAT_LATE,
                  KEEL_ARB_FX_FAILSTOP | KEEL_ARB_FX_PULSE),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_L(KEEL_E_DEADLINE, KEEL_CAUSE_HEARTBEAT_LATE,
                  KEEL_ARB_FX_FAILSTOP | KEEL_ARB_FX_PULSE),
       KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_tick",
     "health is not KEEL_HEALTH_ALL",
     NULL,
     {{KEEL_ARB_BAD_HEALTH, KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_BAD_HEALTH, KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_BAD_HEALTH, KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_BAD_HEALTH, KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_tick",
     "cycle_id not greater than the last accepted one",
     NULL,
     {{KEEL_ARB_BAD_CYCLE, KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_BAD_CYCLE, KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_BAD_CYCLE, KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_BAD_CYCLE, KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_cycle",
     "in is NULL or in->struct_size is below the 1.0 size",
     "No tick runs; the heartbeat ages until the next valid tick.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},

    {"keel_cycle",
     "in is larger than this library knows and its tail is not zero",
     "No tick runs; bindings treat this as fatal for the loop.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},

    {"keel_cycle",
     "valid in, command NONE (tick on time)",
     "Identical to the on-time keel_tick row; out receives the snapshot.",
     {{KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_ACK, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_LEASE,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_GRANT,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_cycle",
     "valid in, command DRIVE with a valid non-zero duty",
     "Reversal deferral applies as for keel_set_motors.",
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_ACK,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_LEASE | KEEL_ARB_FX_DRIVE,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_GRANT, 1u),
       KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_cycle",
     "valid in, command DRIVE with (0, 0)",
     NULL,
     {{KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_ACK, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_COAST,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_GRANT,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_cycle",
     "valid in, command BRAKE",
     NULL,
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_ACK,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_LEASE | KEEL_ARB_FX_BRAKE,
                  1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_GRANT, 1u),
       KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_cycle",
     "valid in; unknown command or flag, duty out of range, or bad out",
     "Checked after the tick; while ARMED the motors coast. Bindings treat this as fatal.",
     {{KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_D, KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_A,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_ACK, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_R,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_COAST, 1u),
       KEEL_ARB_FOREIGN_TICK},
      {KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_S,
                  KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_GRANT, 1u),
       KEEL_ARB_FOREIGN_TICK},
      KEEL_ARB_BOTH(KEEL_ARB_FAULTED_TICK)}},

    {"keel_request_arm",
     "no fault latched within the last 1 s",
     NULL,
     {{KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_ACK, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_A, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_S, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_request_arm",
     "a fault latched less than 1 s ago",
     "Arming cannot be in progress within 1 s of a latch, so only DISARMED and FAULTED occur.",
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_HOLDOFF, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_NEVER(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_HOLDOFF, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_NEVER(KEEL_E_STATE, KEEL_ARB_R, KEEL_ARB_FX_HOLDOFF, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_NEVER(KEEL_E_STATE, KEEL_ARB_S, KEEL_ARB_FX_HOLDOFF, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_disarm",
     "any",
     "ENABLE falls before any lock is taken; the whole safe stop is re-run in every state.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_STOP, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_STOP, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_STOP, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_STOP | KEEL_ARB_FX_SERVO_END, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP, 0u))}},

    {"keel_emergency_stop",
     "any",
     "Record status E_FAULTED, cause USER_STOP; in FAULTED the existing record is kept.",
     {KEEL_ARB_BOTH(KEEL_ARB_L(KEEL_OK, KEEL_CAUSE_USER_STOP,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP | KEEL_ARB_FX_LATCH)),
      KEEL_ARB_BOTH(KEEL_ARB_L(KEEL_OK, KEEL_CAUSE_USER_STOP,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP | KEEL_ARB_FX_LATCH)),
      KEEL_ARB_BOTH(KEEL_ARB_L(KEEL_OK, KEEL_CAUSE_USER_STOP,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP | KEEL_ARB_FX_LATCH)),
      KEEL_ARB_BOTH(KEEL_ARB_L(KEEL_OK, KEEL_CAUSE_USER_STOP,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_SERVO_END | KEEL_ARB_FX_HB_STOP |
                                   KEEL_ARB_FX_LATCH)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP, 0u))}},

    {"keel_clear_fault",
     "recorded cause not sticky (or not FAULTED)",
     "The heartbeat resumes at the next tick, which records the new control owner.",
     {{KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_A, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_S, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_STOP | KEEL_ARB_FX_CLEAR, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_clear_fault",
     "sticky cause (ARMED_UNEXPECTED, INTERLOCK, INTERNAL, CLOCK, HAL_CONTRACT)",
     "A sticky record exists only while FAULTED; close and reopen to recover.",
     {{KEEL_ARB_NEVER(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_NEVER(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_NEVER(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_NEVER(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_set_motors",
     "(0, 0)",
     NULL,
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_COAST, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},

    {"keel_set_motors",
     "valid non-zero duty, no reversal",
     "The command lives for the command timeout (default 500 ms), then coasts without a fault.",
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_A, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_DRIVE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_S, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_set_motors",
     "valid; a wheel changes direction without 20 ms idle",
     "The first tick or command at least 20 ms after the coast applies the pending command.",
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_A, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_REVERSAL, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_S, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_set_motors",
     "a duty outside -1000..1000",
     "Arguments are checked before the thread, so any thread's invalid command coasts while ARMED.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_R, KEEL_ARB_FX_COAST, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_INVALID_ARGUMENT, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},

    {"keel_brake",
     "KEEL_CAP_BRAKE",
     "Without the capability: KEEL_E_UNSUPPORTED from any thread, coasting while ARMED.",
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_A, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_BRAKE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_S, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_servo_start",
     "KEEL_CAP_SERVO; valid arguments; motors coasting 20 ms; cooldown elapsed",
     "The pulse starts on the tick that confirms the grant (ARMED 0, READY 1 within 10 ms).",
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_A, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_SERVO_REQUEST, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_S, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_servo_start",
     "KEEL_CAP_SERVO; a motor driven or braked, or a reversal pending",
     "Motors only drive, brake or hold a pending reversal while ARMED.",
     {{KEEL_ARB_NEVER(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_NEVER(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)},
      {KEEL_ARB_NEVER(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_NEVER(KEEL_E_CONFLICT, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u)}}},

    {"keel_servo_start",
     "KEEL_CAP_SERVO; cooldown running, or motors coasting for less than 20 ms",
     NULL,
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_A, 1u)},
      {KEEL_ARB_C(KEEL_E_AGAIN, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_S, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_servo_start, keel_servo_set_pulse",
     "no KEEL_CAP_SERVO (keel_r3 on R3: J5 not fitted)",
     "The capability is checked before the thread and the state; keel_servo_end stays a no-op.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_NEVER(KEEL_E_UNSUPPORTED, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_UNSUPPORTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},

    {"keel_servo_set_pulse",
     "KEEL_CAP_SERVO; pulse in range",
     "Never extends the lease.",
     {{KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_D, 1u)},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_A, 1u)},
      {KEEL_ARB_C(KEEL_E_STATE, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u), KEEL_ARB_CONFLICT(KEEL_ARB_R, 1u)},
      {KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_SERVO_PULSE, 1u),
       KEEL_ARB_CONFLICT(KEEL_ARB_S, 1u)},
      {KEEL_ARB_C(KEEL_E_FAULTED, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u),
       KEEL_ARB_CONFLICT(KEEL_ARB_F, 0u)}}},

    {"keel_servo_end",
     "any",
     NULL,
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_SERVO_END, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},

    {"keel_range_start",
     "KEEL_CAP_RANGE; sensor power PERMITTED or CONFIRMED; idle; spacing elapsed; ECHO LOW",
     "FAULTED has no heartbeat, so the sensor is never powered there.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_TRIGGER | KEEL_ARB_FX_READ, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_TRIGGER | KEEL_ARB_FX_READ, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_TRIGGER | KEEL_ARB_FX_READ, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_TRIGGER | KEEL_ARB_FX_READ, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_NEVER(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_TRIGGER, 0u))}},

    {"keel_range_start",
     "KEEL_CAP_RANGE; sensor power OFF or UNKNOWN",
     "On-time ticks keep READY HIGH and the heartbeat qualified in ARMED and SERVO. There the "
     "sensor is unpowered only after the loop stalled past 70 ms (or the MCU dropped READY since "
     "the last tick, which the fresh READY read finds): the state shown is the cached one until "
     "the next tick latches, and the heartbeat has already stopped.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_UNAVAILABLE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_UNAVAILABLE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_STALLED(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_UNAVAILABLE)),
      KEEL_ARB_BOTH(KEEL_ARB_STALLED(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_UNAVAILABLE)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_UNAVAILABLE, 0u))}},

    {"keel_range_start",
     "KEEL_CAP_RANGE; sensor powered; measurement pending, under 60 ms since the trigger, or ECHO "
     "HIGH",
     NULL,
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_AGAIN, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_AGAIN, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_AGAIN, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_AGAIN, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_NEVER(KEEL_E_AGAIN, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},

    {"keel_range_poll",
     "KEEL_CAP_RANGE",
     NULL,
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_RESULT, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_RESULT, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_RESULT, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_RESULT, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_RESULT, 0u))}},

    {"keel_range_measure",
     "KEEL_CAP_RANGE; spacing elapsed; sensor powered unless FAULTED",
     "The control owner may not block while a heartbeat deadline is armed.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_MEASURE, 1u)),
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_MEASURE, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_MEASURE, 1u)},
      {KEEL_ARB_C(KEEL_E_CONFLICT, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u),
       KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_MEASURE, 1u)},
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_UNAVAILABLE, 0u))}},

    {"keel_set_rgb, keel_set_rgb_all, keel_set_rgb_frame",
     "KEEL_CAP_RGB; valid arguments",
     "An I2C failure returns KEEL_E_PERIPHERAL and never latches.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_I2C, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_I2C, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_I2C, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_I2C, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_I2C, 0u))}},

    {"keel_set_button_leds",
     "mask within KEEL_BUTTON_ALL",
     "A GPIO failure is KEEL_E_IO and latches (the lines share the control request).",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_BUTTON_LEDS, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_BUTTON_LEDS, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_BUTTON_LEDS, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_BUTTON_LEDS, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_BUTTON_LEDS, 0u))}},

    {"keel_take_buttons",
     "valid out",
     "A backend latch found by the read is recorded first (for example E_PWM after a worker "
     "overrun).",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_READ | KEEL_ARB_FX_TAKE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_READ | KEEL_ARB_FX_TAKE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_READ | KEEL_ARB_FX_TAKE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_READ | KEEL_ARB_FX_TAKE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_READ | KEEL_ARB_FX_TAKE, 0u))}},

    {"keel_read_safety_inputs",
     "valid out",
     NULL,
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_READ, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_READ, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_READ, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_READ, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_READ, 0u))}},

    {"keel_get_state, keel_get_snapshot, keel_get_fault, keel_get_capabilities, keel_get_rgb, "
     "keel_now_us",
     "valid out",
     "Cached reads: no I/O.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},

    {"keel_sleep_until",
     "deadline at most 10 s ahead",
     "Returns KEEL_E_INVALID_HANDLE if keel_close starts during the sleep.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_SLEEP, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_SLEEP, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_SLEEP, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_SLEEP, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_SLEEP, 0u))}},

    {"keel_sim_set, keel_sim_get_state",
     "simulator device; valid item",
     "On other devices: KEEL_E_UNSUPPORTED, nothing done.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_D, KEEL_ARB_FX_SIM, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_A, KEEL_ARB_FX_SIM, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_R, KEEL_ARB_FX_SIM, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_S, KEEL_ARB_FX_SIM, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_F, KEEL_ARB_FX_SIM, 0u))}},

    {"keel_close",
     "any",
     "The Category-0 stop runs under the control lock before the LED and range locks are taken.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_CLOSED,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP | KEEL_ARB_FX_RELEASE, 0u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_CLOSED,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP | KEEL_ARB_FX_RELEASE, 0u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_CLOSED,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP | KEEL_ARB_FX_RELEASE, 0u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_CLOSED,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_SERVO_END | KEEL_ARB_FX_HB_STOP |
                                   KEEL_ARB_FX_RELEASE,
                               0u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_OK, KEEL_ARB_CLOSED,
                               KEEL_ARB_FX_STOP | KEEL_ARB_FX_HB_STOP | KEEL_ARB_FX_RELEASE, 0u))}},

    {"any call",
     "from a process that did not open the device (forked child)",
     "Checked atomically before any lock; the parent's device is untouched.",
     {KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_NOT_OWNER, KEEL_ARB_D, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_NOT_OWNER, KEEL_ARB_A, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_NOT_OWNER, KEEL_ARB_R, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_NOT_OWNER, KEEL_ARB_S, KEEL_ARB_FX_NONE, 1u)),
      KEEL_ARB_BOTH(KEEL_ARB_C(KEEL_E_NOT_OWNER, KEEL_ARB_F, KEEL_ARB_FX_NONE, 0u))}},
};

/** @brief Number of rows in ::keel_arb_table. */
enum keel_arb_size {
    /** Row count. */
    KEEL_ARB_ROW_COUNT = (int)(sizeof(keel_arb_table) / sizeof(keel_arb_table[0]))
};

#undef KEEL_ARB_C
#undef KEEL_ARB_NEVER
#undef KEEL_ARB_L
#undef KEEL_ARB_BOTH
#undef KEEL_ARB_D
#undef KEEL_ARB_A
#undef KEEL_ARB_R
#undef KEEL_ARB_S
#undef KEEL_ARB_F
#undef KEEL_ARB_FOREIGN_TICK
#undef KEEL_ARB_BAD_HEALTH
#undef KEEL_ARB_BAD_CYCLE
#undef KEEL_ARB_FAULTED_TICK
#undef KEEL_ARB_CONFLICT

#endif /* KEEL_TEST_ARBITRATION_TABLE_H */
