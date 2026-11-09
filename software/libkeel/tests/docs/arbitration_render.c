/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Prints the arbitration table (arbitration_table.h) as Markdown on stdout.
 * docs/spec.md and docs/safety.md carry this output verbatim, so the
 * documents can never drift from the table the integration tests replay.
 * Rows whose two thread columns agree in every state are printed once with
 * the thread "any".
 *
 * Before printing, every cell is checked against the rules the table must
 * obey (section 13 of docs/spec.md); a violation is reported on stderr and
 * nothing is printed, so a malformed table cannot reach the documents.
 */
#include <stdio.h>

#include "arbitration_table.h"

/* The library's own name table is not linked here: the renderer must build
 * before the library exists, and the short form keeps table cells narrow. */
static const char *status_short_name(int32_t status) {
    switch (status) {
    case KEEL_OK:
        return "OK";
    case KEEL_E_INVALID_ARGUMENT:
        return "E_INVALID_ARGUMENT";
    case KEEL_E_IO:
        return "E_IO";
    case KEEL_E_STATE:
        return "E_STATE";
    case KEEL_E_HEALTH:
        return "E_HEALTH";
    case KEEL_E_DEADLINE:
        return "E_DEADLINE";
    case KEEL_E_FEEDBACK:
        return "E_FEEDBACK";
    case KEEL_E_AGAIN:
        return "E_AGAIN";
    case KEEL_E_PWM:
        return "E_PWM";
    case KEEL_E_FAULTED:
        return "E_FAULTED";
    case KEEL_E_UNSUPPORTED:
        return "E_UNSUPPORTED";
    case KEEL_E_CONFLICT:
        return "E_CONFLICT";
    case KEEL_E_PERIPHERAL:
        return "E_PERIPHERAL";
    case KEEL_E_INVALID_HANDLE:
        return "E_INVALID_HANDLE";
    case KEEL_E_NOT_OWNER:
        return "E_NOT_OWNER";
    case KEEL_E_INTERNAL:
        return "E_INTERNAL";
    default:
        return "E_UNKNOWN";
    }
}

/* Only causes that a request itself can latch appear in the table. */
static const char *cause_name(uint32_t cause) {
    switch (cause) {
    case KEEL_CAUSE_HEALTH_FLAGS:
        return "HEALTH_FLAGS";
    case KEEL_CAUSE_CYCLE_NOT_INCREASING:
        return "CYCLE_NOT_INCREASING";
    case KEEL_CAUSE_TICK_THREAD_CHANGED:
        return "TICK_THREAD_CHANGED";
    case KEEL_CAUSE_TICK_LATE:
        return "TICK_LATE";
    case KEEL_CAUSE_HEARTBEAT_LATE:
        return "HEARTBEAT_LATE";
    case KEEL_CAUSE_USER_STOP:
        return "USER_STOP";
    default:
        return "UNKNOWN";
    }
}

static const char *state_name(uint32_t state) {
    static const char *const names[] = {"DISARMED", "ARMING", "ARMED", "SERVO", "FAULTED",
                                        "closed"};
    return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

/* Mirrors the class column of keel.h's status documentation. */
static uint32_t status_class(int32_t status) {
    switch (status) {
    case KEEL_OK:
        return KEEL_CLASS_OK;
    case KEEL_E_INVALID_ARGUMENT:
    case KEEL_E_STATE:
    case KEEL_E_AGAIN:
    case KEEL_E_UNSUPPORTED:
    case KEEL_E_CONFLICT:
        return KEEL_CLASS_REJECTED;
    case KEEL_E_PERIPHERAL:
        return KEEL_CLASS_PERIPHERAL;
    case KEEL_E_INVALID_HANDLE:
    case KEEL_E_NOT_OWNER:
        return KEEL_CLASS_HANDLE;
    default:
        return KEEL_CLASS_FAULT;
    }
}

/* Effects a REJECTED cell may show: the tick part of keel_cycle, the
 * documented coast of a refused motion request, and the hold-off flag. */
static const uint32_t rejected_effects = KEEL_ARB_FX_SAMPLE | KEEL_ARB_FX_PULSE | KEEL_ARB_FX_ACK |
                                         KEEL_ARB_FX_GRANT | KEEL_ARB_FX_COAST |
                                         KEEL_ARB_FX_HOLDOFF;
/* Effects that need a running heartbeat or motor permission. */
static const uint32_t active_effects = KEEL_ARB_FX_PULSE | KEEL_ARB_FX_LEASE | KEEL_ARB_FX_DRIVE |
                                       KEEL_ARB_FX_REVERSAL | KEEL_ARB_FX_BRAKE |
                                       KEEL_ARB_FX_TRIGGER;

static int check_cell(const keel_arb_row *row, const keel_arb_cell *cell, uint32_t state) {
    const char *problem = NULL;
    uint32_t cls = status_class(cell->status);
    int stopped = cell->state_after == KEEL_STATE_FAULTED || cell->state_after == KEEL_ARB_CLOSED ||
                  (cell->flags & KEEL_ARB_LOOP_STALLED);
    if (cell->flags & KEEL_ARB_UNREACHABLE) {
        return 0;
    }
    if (cell->heartbeat != (stopped ? 0u : 1u)) {
        problem = "heartbeat must run exactly when the state after is neither FAULTED nor closed "
                  "and the loop has not stalled";
    } else if ((cell->flags & KEEL_ARB_LOOP_STALLED) &&
               (cell->state_after != state || cls == KEEL_CLASS_FAULT ||
                (cell->effects & active_effects))) {
        problem = "a stalled-loop cell keeps the cached state and runs nothing";
    } else if ((cell->cause != KEEL_CAUSE_NONE) != ((cell->effects & KEEL_ARB_FX_LATCH) != 0)) {
        problem = "a cause is recorded exactly when the cell latches";
    } else if (cls == KEEL_CLASS_FAULT && cell->status != KEEL_E_FAULTED &&
               (cell->state_after != KEEL_STATE_FAULTED || !(cell->effects & KEEL_ARB_FX_LATCH))) {
        problem = "a new FAULT-class status must latch FAULTED";
    } else if (cls == KEEL_CLASS_REJECTED &&
               (cell->state_after != state || (cell->effects & ~rejected_effects) ||
                ((cell->effects & KEEL_ARB_FX_COAST) && state != KEEL_STATE_ARMED))) {
        problem = "a REJECTED status changes no state and coasts only while ARMED";
    } else if (cls == KEEL_CLASS_HANDLE && (cell->effects || cell->state_after != state)) {
        problem = "a HANDLE-class status touches nothing";
    } else if (state == KEEL_STATE_FAULTED && (cell->effects & active_effects)) {
        problem = "nothing runs the heartbeat, motors or sensor while FAULTED";
    }
    if (problem) {
        fprintf(stderr, "arbitration_render: %s (%s) in %s: %s\n", row->call, row->condition,
                state_name(state), problem);
        return 1;
    }
    return 0;
}

static int check_table(void) {
    int i, failures = 0;
    uint32_t state, thread;
    for (i = 0; i < KEEL_ARB_ROW_COUNT; ++i) {
        for (state = 0; state < KEEL_ARB_STATE_COUNT; ++state) {
            for (thread = 0; thread < KEEL_ARB_THREAD_COUNT; ++thread) {
                failures += check_cell(&keel_arb_table[i], &keel_arb_table[i].cells[state][thread],
                                       state);
            }
        }
    }
    return failures;
}

static int cells_equal(const keel_arb_cell *a, const keel_arb_cell *b) {
    return a->status == b->status && a->cause == b->cause && a->state_after == b->state_after &&
           a->effects == b->effects && a->heartbeat == b->heartbeat && a->flags == b->flags;
}

static int threads_agree(const keel_arb_row *row) {
    uint32_t state;
    for (state = 0; state < KEEL_ARB_STATE_COUNT; ++state) {
        if (!cells_equal(&row->cells[state][KEEL_ARB_OWNER], &row->cells[state][KEEL_ARB_OTHER])) {
            return 0;
        }
    }
    return 1;
}

static void print_cell(const keel_arb_cell *cell, uint32_t state) {
    uint32_t bit;
    int first = 1;
    if (cell->flags & KEEL_ARB_UNREACHABLE) {
        printf(" unreachable |");
        return;
    }
    printf(" %s", status_short_name(cell->status));
    if (cell->cause != KEEL_CAUSE_NONE) {
        printf(" (%s)", cause_name(cell->cause));
    }
    if (cell->state_after != state) {
        printf(" -> %s", state_name(cell->state_after));
    }
    if (cell->flags & KEEL_ARB_LOOP_STALLED) {
        printf(" (loop stalled)");
    }
    printf(";");
    for (bit = 0; bit < KEEL_ARB_FX_BITS; ++bit) {
        if (cell->effects & (UINT32_C(1) << bit)) {
            printf("%s%s", first ? " " : ", ", keel_arb_effect_names[bit]);
            first = 0;
        }
    }
    if (first) {
        printf(" none");
    }
    printf("; HB %s |", cell->heartbeat ? "yes" : "no");
}

static void print_row(const keel_arb_row *row, int note, const char *thread, uint32_t column) {
    uint32_t state;
    printf("| `%s` | %s", row->call, row->condition);
    if (note) {
        printf(" [%d]", note);
    }
    printf(" | %s |", thread);
    for (state = 0; state < KEEL_ARB_STATE_COUNT; ++state) {
        print_cell(&row->cells[state][column], state);
    }
    printf("\n");
}

int main(void) {
    int i, note = 0;
    if (check_table() != 0) {
        return 1;
    }
    printf("| Call | Condition | Thread | DISARMED | ARMING | ARMED | SERVO | FAULTED |\n");
    printf("|---|---|---|---|---|---|---|---|\n");
    for (i = 0; i < KEEL_ARB_ROW_COUNT; ++i) {
        const keel_arb_row *row = &keel_arb_table[i];
        int this_note = row->note ? ++note : 0;
        if (threads_agree(row)) {
            print_row(row, this_note, "any", KEEL_ARB_OWNER);
        } else {
            print_row(row, this_note, "owner", KEEL_ARB_OWNER);
            print_row(row, 0, "other", KEEL_ARB_OTHER);
        }
    }
    printf("\nNotes:\n\n");
    note = 0;
    for (i = 0; i < KEEL_ARB_ROW_COUNT; ++i) {
        if (keel_arb_table[i].note) {
            printf("%d. %s\n", ++note, keel_arb_table[i].note);
        }
    }
    return 0;
}
