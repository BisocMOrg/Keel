/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Error propagation (SPEC 12.2, 12.3 and 23.1): every safety-path hook, every
 * status a backend could return and every entry point that calls the hook.
 *
 * For each entry point a dry run first records which hooks the call uses;
 * the matrix then fails the first call of each of those hooks with every
 * status and checks:
 *   - a defined FAULT-class status passes through unchanged, anything else
 *     becomes KEEL_E_INTERNAL with the sticky cause HAL_CONTRACT and the raw
 *     value in the detail;
 *   - the device is FAULTED with ENABLE, heartbeat, motors and servo safe;
 *   - the record holds the primary cause (the backend's own, when it latched
 *     one) and a cleanup status that is OK or FAULT-class.
 * Peripheral hooks map every failure to KEEL_E_PERIPHERAL without a latch;
 * the fault-latch hooks (get_fault, reset_fault) have their own case.
 * The fixture plays the backend: a FAULT-class injection latches a backend
 * record first, as port.h requires; any other status comes back bare.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <errno.h>
#include <string.h>

#include "check.h"
#include "fixture.h"

enum hook { H_READ, H_OUTPUTS, H_MOTORS, H_DISABLE, H_SERVO, H_SLEEP, HOOK_COUNT };

static const char *const hook_names[HOOK_COUNT] = {"read_inputs", "write_outputs",
                                                   "write_motors", "emergency_disable",
                                                   "servo", "sleep_until_us"};

/* Cause a backend record carries for each hook's injected failure (sleep has
 * no record, so the core's default cause CLOCK applies). */
static const uint32_t hook_causes[HOOK_COUNT] = {
    KEEL_CAUSE_GPIO_READ, KEEL_CAUSE_GPIO_WRITE, KEEL_CAUSE_MOTOR_WRITE,
    KEEL_CAUSE_GPIO_WRITE, KEEL_CAUSE_GPIO_WRITE, KEEL_CAUSE_CLOCK};

/* Every defined non-OK status, a positive value and an undefined code. */
static const int32_t statuses[] = {
    KEEL_E_INVALID_ARGUMENT, KEEL_E_IO,         KEEL_E_STATE,        KEEL_E_HEALTH,
    KEEL_E_DEADLINE,         KEEL_E_FEEDBACK,   KEEL_E_AGAIN,        KEEL_E_NO_MEMORY,
    KEEL_E_PWM,              KEEL_E_FAULTED,    KEEL_E_IN_USE,       KEEL_E_PERMISSION,
    KEEL_E_NOT_FOUND,        KEEL_E_UNSUPPORTED, KEEL_E_CONFLICT,    KEEL_E_PERIPHERAL,
    KEEL_E_ACK_TIMEOUT,      KEEL_E_INVALID_HANDLE, KEEL_E_NOT_OWNER, KEEL_E_INTERNAL,
    KEEL_E_SETUP_IO,         7,                 -99};

static unsigned *hook_calls(fixture *f, unsigned hook)
{
    unsigned *counters[HOOK_COUNT] = {&f->read_calls,    &f->output_calls, &f->motor_calls,
                                      &f->disable_calls, &f->servo_calls,  &f->sleep_calls};
    return counters[hook];
}

static unsigned *hook_fail(fixture *f, unsigned hook)
{
    unsigned *counters[HOOK_COUNT] = {&f->fail_read,    &f->fail_output, &f->fail_motor,
                                      &f->fail_disable, &f->fail_servo,  &f->fail_sleep};
    return counters[hook];
}

static int fault_status(int32_t status)
{
    return status < 0 && status >= KEEL_E_SETUP_IO &&
           keel_status_class(status) == KEEL_CLASS_FAULT;
}

/* ---- device states ---------------------------------------------------------- */

static void ticks(fixture *f, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        fx_tick_after(f, 10000);
    }
}

/* Moves time so the next tick emits a heartbeat pulse. */
static void pulse_due(fixture *f)
{
    uint64_t due = f->last_fall_us + 30000, next = fx_now(f) + 10000;
    fx_advance(f, due > next ? due : next);
}

static void disarmed(fixture *f)
{
    fx_init_options(f, FX_SERVO | FX_RANGE);
    ticks(f, 30);
}

static void driving(fixture *f)
{
    disarmed(f);
    fx_arm(f);
    CHECK_RC(keel_set_motors(f->device, 300, 300), KEEL_OK);
    pulse_due(f);
}

static void faulted(fixture *f)
{
    driving(f);
    CHECK_RC(keel_emergency_stop(f->device), KEEL_OK);
}

/* ARMED after the 2.2 s servo cooldown, motors coasting. */
static void cooled(fixture *f)
{
    disarmed(f);
    ticks(f, 240);
    fx_arm(f);
    ticks(f, 3);
}

static void servo_requested(fixture *f)
{
    cooled(f);
    CHECK_RC(keel_servo_start(f->device, 1500, 300), KEEL_OK);
    pulse_due(f);
}

static void servo_granted(fixture *f)
{
    servo_requested(f);
    CHECK_RC(fx_tick(f), KEEL_OK);
    CHECK(f->board.servo_pulse_us == 1500);
}

/* ---- entry points ------------------------------------------------------------ */

static int32_t call_tick(fixture *f)
{
    return fx_tick(f);
}

static int32_t call_cycle(fixture *f)
{
    keel_cycle_input in;
    keel_snapshot out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.struct_size = sizeof in;
    in.health = KEEL_HEALTH_ALL;
    in.cycle_id = ++f->cycle;
    in.command = KEEL_CYCLE_DRIVE;
    in.left = 500;
    in.right = 500;
    out.struct_size = sizeof out;
    return keel_cycle(f->device, &in, &out);
}

static int32_t call_arm(fixture *f)
{
    return keel_request_arm(f->device);
}

static int32_t call_motors(fixture *f)
{
    return keel_set_motors(f->device, 500, 500);
}

static int32_t call_motors_zero(fixture *f)
{
    return keel_set_motors(f->device, 0, 0);
}

static int32_t call_motors_invalid(fixture *f)
{
    return keel_set_motors(f->device, 2000, 0);
}

static int32_t call_brake(fixture *f)
{
    return keel_brake(f->device);
}

static int32_t call_disarm(fixture *f)
{
    return keel_disarm(f->device);
}

static int32_t call_emergency_stop(fixture *f)
{
    return keel_emergency_stop(f->device);
}

static int32_t call_clear_fault(fixture *f)
{
    return keel_clear_fault(f->device);
}

static int32_t call_take_buttons(fixture *f)
{
    keel_buttons buttons;
    memset(&buttons, 0xA5, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    return keel_take_buttons(f->device, &buttons);
}

static int32_t call_read_inputs(fixture *f)
{
    uint32_t inputs;
    return keel_read_safety_inputs(f->device, &inputs);
}

static int32_t call_button_leds(fixture *f)
{
    return keel_set_button_leds(f->device, KEEL_BUTTON_ALL, KEEL_BUTTON_A);
}

static int32_t call_servo_start(fixture *f)
{
    return keel_servo_start(f->device, 1500, 300);
}

static int32_t call_servo_pulse(fixture *f)
{
    return keel_servo_set_pulse(f->device, 1200);
}

static int32_t call_servo_end(fixture *f)
{
    return keel_servo_end(f->device);
}

static int32_t call_sleep(fixture *f)
{
    return keel_sleep_until(f->device, fx_now(f) + 5000);
}

/* Nothing in range: the measurement waits for its 30 ms on the sleep hook. */
static int32_t call_range_measure(fixture *f)
{
    keel_range range;
    memset(&range, 0, sizeof range);
    range.struct_size = sizeof range;
    return keel_range_measure(f->device, &range);
}

/* keel_close() hands back the final record; a failure there reads E_IO. */
static keel_fault final_record;

static int32_t call_close(fixture *f)
{
    int32_t rc;
    memset(&final_record, 0, sizeof final_record);
    final_record.struct_size = sizeof final_record;
    rc = keel_close(f->device, &final_record);
    f->device = NULL;
    return rc;
}

typedef struct entry {
    const char *name;
    void (*setup)(fixture *f);
    int32_t (*call)(fixture *f);
    unsigned hooks;          /* hooks the call uses (bit per ::hook) */
    unsigned cleanup_hooks;  /* hooks whose first call is the cleanup of a latch the
                                call makes on purpose (the user stop) */
} entry;

#define BIT(h) (1u << (h))

static const entry entries[] = {
    {"tick", driving, call_tick, BIT(H_READ) | BIT(H_MOTORS) | BIT(H_OUTPUTS) | BIT(H_SLEEP), 0},
    {"cycle", driving, call_cycle, BIT(H_READ) | BIT(H_MOTORS) | BIT(H_OUTPUTS) | BIT(H_SLEEP), 0},
    {"request_arm", disarmed, call_arm, BIT(H_READ) | BIT(H_OUTPUTS) | BIT(H_MOTORS), 0},
    {"set_motors", driving, call_motors, BIT(H_READ) | BIT(H_MOTORS), 0},
    {"set_motors_zero", driving, call_motors_zero, BIT(H_MOTORS) | BIT(H_READ), 0},
    {"set_motors_invalid", driving, call_motors_invalid, BIT(H_MOTORS) | BIT(H_READ), 0},
    {"brake", driving, call_brake, BIT(H_READ) | BIT(H_MOTORS), 0},
    {"disarm", driving, call_disarm,
     BIT(H_DISABLE) | BIT(H_READ) | BIT(H_MOTORS) | BIT(H_SERVO), 0},
    {"emergency_stop", driving, call_emergency_stop,
     BIT(H_DISABLE) | BIT(H_READ) | BIT(H_MOTORS) | BIT(H_SERVO) | BIT(H_OUTPUTS),
     BIT(H_MOTORS) | BIT(H_SERVO) | BIT(H_OUTPUTS)},
    {"clear_fault", faulted, call_clear_fault,
     BIT(H_DISABLE) | BIT(H_MOTORS) | BIT(H_SERVO) | BIT(H_OUTPUTS) | BIT(H_READ), 0},
    {"take_buttons", disarmed, call_take_buttons, BIT(H_READ), 0},
    {"read_safety_inputs", disarmed, call_read_inputs, BIT(H_READ), 0},
    {"set_button_leds", disarmed, call_button_leds, BIT(H_READ) | BIT(H_OUTPUTS), 0},
    {"servo_start", cooled, call_servo_start, BIT(H_READ) | BIT(H_SERVO), 0},
    {"servo_set_pulse", servo_granted, call_servo_pulse, BIT(H_READ) | BIT(H_SERVO), 0},
    {"servo_end", servo_granted, call_servo_end, BIT(H_SERVO) | BIT(H_OUTPUTS) | BIT(H_READ), 0},
    {"servo_grant_tick", servo_requested, call_tick,
     BIT(H_READ) | BIT(H_SERVO) | BIT(H_OUTPUTS) | BIT(H_SLEEP), 0},
    {"sleep_until", disarmed, call_sleep, BIT(H_SLEEP), 0},
    {"range_measure", disarmed, call_range_measure, BIT(H_SLEEP) | BIT(H_READ), 0},
    {"close", driving, call_close,
     BIT(H_DISABLE) | BIT(H_READ) | BIT(H_MOTORS) | BIT(H_SERVO) | BIT(H_OUTPUTS), 0},
};

/* Hooks the call uses, from a run without injection. */
static unsigned dry_run(const entry *e)
{
    fixture f;
    unsigned before[HOOK_COUNT], used = 0, h;
    e->setup(&f);
    for (h = 0; h < HOOK_COUNT; ++h) {
        before[h] = *hook_calls(&f, h);
    }
    (void)e->call(&f);
    for (h = 0; h < HOOK_COUNT; ++h) {
        if (*hook_calls(&f, h) != before[h]) {
            used |= BIT(h);
        }
    }
    fx_destroy(&f);
    return used;
}

static void check_record(const keel_fault *record, unsigned hook, int32_t status)
{
    if (fault_status(status)) {
        CHECK_RC(record->status, status);
        CHECK_EQ(record->cause, hook_causes[hook]);
        CHECK_EQ(record->os_error, hook == H_SLEEP ? 0 : EIO);
    } else {
        CHECK_RC(record->status, KEEL_E_INTERNAL);
        CHECK_EQ(record->cause, KEEL_CAUSE_HAL_CONTRACT);
        CHECK_EQ(record->detail, (uint64_t)(int64_t)status);
        CHECK(record->flags & KEEL_FAULT_STICKY);
    }
}

static void inject(const entry *e, unsigned hook, int32_t status)
{
    int32_t expected = fault_status(status) ? status : KEEL_E_INTERNAL;
    int primary, was_faulted;
    uint32_t count_before;
    keel_fault record;
    fixture f;
    int32_t rc;

    e->setup(&f);
    was_faulted = fx_state(&f) == KEEL_STATE_FAULTED;
    count_before = fx_snapshot(&f).fault_count;
    primary = !was_faulted && !(e->cleanup_hooks & BIT(hook));
    *hook_fail(&f, hook) = *hook_calls(&f, hook) + 1u;
    f.fail_status = status;
    rc = e->call(&f);
    CHECK(*hook_calls(&f, hook) >= *hook_fail(&f, hook)); /* the injection fired */

    if (e->call == call_close) {
        /* close is diagnostic: E_IO when a stop action failed; the final
         * record names the failure. A failed read is not a stop action. */
        CHECK_RC(rc, hook == H_READ ? KEEL_OK : KEEL_E_IO);
        check_record(&final_record, hook, status);
        CHECK(final_record.cleanup_status == KEEL_OK ||
              keel_status_class(final_record.cleanup_status) == KEEL_CLASS_FAULT);
        fx_safe(&f);
        fx_destroy(&f);
        return;
    }
    CHECK_RC(rc, expected);
    CHECK_EQ(fx_state(&f), KEEL_STATE_FAULTED);
    fx_safe(&f);
    record = fx_fault(&f);
    CHECK(record.cleanup_status == KEEL_OK ||
          keel_status_class(record.cleanup_status) == KEEL_CLASS_FAULT);
    if (primary) {
        CHECK_EQ(record.fault_count, count_before + 1u);
        check_record(&record, hook, status);
    } else if (was_faulted) {
        CHECK_EQ(record.fault_count, count_before); /* the first fault keeps the record */
    } else {
        CHECK_EQ(record.cause, KEEL_CAUSE_USER_STOP); /* failed while stopping on purpose */
    }
    /* No heartbeat follows: the MCU removes every permission by itself. */
    fx_no_more_heartbeat(&f);
    fx_destroy(&f);
}

static void run_entry(const char *name)
{
    size_t i, s;
    unsigned h;
    for (i = 0; i < sizeof entries / sizeof entries[0]; ++i) {
        const entry *e = &entries[i];
        if (strcmp(e->name, name) != 0) {
            continue;
        }
        CHECK_EQ(dry_run(e), e->hooks);
        for (h = 0; h < HOOK_COUNT; ++h) {
            if (!(e->hooks & BIT(h))) {
                continue;
            }
            for (s = 0; s < sizeof statuses / sizeof statuses[0]; ++s) {
                fprintf(stderr, "%s: %s fails with %d\n", name, hook_names[h], statuses[s]);
                inject(e, h, statuses[s]);
            }
        }
        return;
    }
    CHECK(!"unknown entry");
}

/* ---- peripheral hooks ------------------------------------------------------ */

static void peripheral_hooks(void)
{
    size_t s;
    for (s = 0; s < sizeof statuses / sizeof statuses[0]; ++s) {
        fixture f;
        keel_range range;
        uint32_t falls;
        driving(&f);
        f.fail_status = statuses[s];
        f.fail_trigger = f.trigger_calls + 1u;
        CHECK_RC(keel_range_start(f.device), KEEL_E_PERIPHERAL);
        CHECK_RC(keel_range_start(f.device), KEEL_OK);
        f.fail_edges = f.edges_calls + 1u;
        memset(&range, 0, sizeof range);
        range.struct_size = sizeof range;
        CHECK_RC(keel_range_poll(f.device, &range), KEEL_E_PERIPHERAL);
        CHECK_EQ(range.status, KEEL_RANGE_INVALID); /* edge events were lost */
        f.fail_i2c = (unsigned)f.nwrites + 1u;
        CHECK_RC(keel_set_rgb(f.device, 0, 0xFFFFFFu), KEEL_E_PERIPHERAL);
        /* Nothing latched; the device keeps driving and the heartbeat runs. */
        CHECK_EQ(fx_state(&f), KEEL_STATE_ARMED);
        CHECK_EQ(fx_snapshot(&f).fault_count, 0);
        CHECK(fx_left(&f) == 300);
        falls = (uint32_t)f.fall_count;
        ticks(&f, 8);
        CHECK(f.fall_count >= falls + 2u);
        fx_destroy(&f);
    }
}

/* ---- the fault-latch hooks ------------------------------------------------- */

/* reset_fault failing keeps the device FAULTED with the status class
 * preserved and the record not cleared; get_fault failing while the backend
 * flags its latch leaves no usable record, which is HAL_CONTRACT. */
static void fault_latch_hooks(void)
{
    size_t s;
    for (s = 0; s < sizeof statuses / sizeof statuses[0]; ++s) {
        int32_t status = statuses[s];
        keel_buttons buttons;
        keel_fault record;
        fixture f;
        fprintf(stderr, "fault-latch hooks fail with %d\n", status);

        faulted(&f);
        f.fail_status = status;
        f.fail_reset = f.reset_calls + 1u;
        CHECK_RC(keel_clear_fault(f.device), fault_status(status) ? status : KEEL_E_INTERNAL);
        CHECK_EQ(fx_state(&f), KEEL_STATE_FAULTED);
        record = fx_fault(&f);
        CHECK(record.cause == KEEL_CAUSE_USER_STOP && !(record.flags & KEEL_FAULT_CLEARED));
        fx_safe(&f);
        /* A later attempt succeeds, unless the backend broke the port
         * contract: HAL_CONTRACT is sticky even while already FAULTED. */
        CHECK_RC(keel_clear_fault(f.device), fault_status(status) ? KEEL_OK : KEEL_E_FAULTED);
        fx_destroy(&f);

        driving(&f);
        (void)keel__board_latch(&f.board, KEEL_E_PWM, KEEL_CAUSE_PWM_OVERRUN, 0, 31000);
        f.fail_status = status;
        f.fail_get_fault = f.get_fault_calls + 1u;
        memset(&buttons, 0, sizeof buttons);
        buttons.struct_size = sizeof buttons;
        CHECK_RC(keel_take_buttons(f.device, &buttons), KEEL_E_INTERNAL);
        record = fx_fault(&f);
        CHECK(record.cause == KEEL_CAUSE_HAL_CONTRACT && (record.flags & KEEL_FAULT_STICKY));
        fx_safe(&f);
        fx_destroy(&f);
    }
}

/* ---- named regressions -------------------------------------------------------- */

/* SPEC 23.1: a backend answering a safety write with KEEL_E_AGAIN must not
 * reach the application as a retryable status. */
static void hal_contract_again(void)
{
    fixture f;
    keel_fault record;
    driving(&f);
    f.fail_status = KEEL_E_AGAIN;
    f.fail_motor = f.motor_calls + 1u;
    CHECK_RC(keel_set_motors(f.device, 500, 500), KEEL_E_INTERNAL);
    record = fx_fault(&f);
    CHECK_RC(record.status, KEEL_E_INTERNAL);
    CHECK_EQ(record.cause, KEEL_CAUSE_HAL_CONTRACT);
    CHECK_EQ(record.detail, (uint64_t)(int64_t)KEEL_E_AGAIN);
    CHECK(record.flags & KEEL_FAULT_STICKY);
    CHECK(fx_snapshot(&f).flags & KEEL_SNAP_FAULT_STICKY);
    fx_safe(&f);
    CHECK_RC(keel_clear_fault(f.device), KEEL_E_FAULTED); /* sticky until close */
    fx_destroy(&f);
}

/* SPEC 12.3: a PWM worker OVERRUN between two ticks, with no pulse due, is
 * reported by the next entry point as itself, not as ARMED_LOST or E_IO. */
static void overrun_between_ticks(void)
{
    fixture f;
    keel_buttons buttons;
    keel_fault record;
    uint32_t heartbeat;
    driving(&f);
    CHECK_RC(fx_tick(&f), KEEL_OK); /* the pulse just fell; none is due for 30 ms */
    heartbeat = f.board.outputs & KEEL_OUT_HEARTBEAT;
    fx_advance(&f, fx_now(&f) + 5000);
    (void)keel__board_latch(&f.board, KEEL_E_PWM, KEEL_CAUSE_PWM_OVERRUN, 0, 31000);
    /* The worker's self-stop lowered ENABLE and coasted, never the heartbeat. */
    CHECK(!(f.board.outputs & KEEL_OUT_ENABLE) && f.board.levels == 0);
    CHECK_EQ(f.board.outputs & KEEL_OUT_HEARTBEAT, heartbeat);
    fx_advance(&f, fx_now(&f) + 2000);
    memset(&buttons, 0, sizeof buttons);
    buttons.struct_size = sizeof buttons;
    CHECK_RC(keel_take_buttons(f.device, &buttons), KEEL_E_PWM);
    record = fx_fault(&f);
    CHECK_RC(record.status, KEEL_E_PWM);
    CHECK_EQ(record.cause, KEEL_CAUSE_PWM_OVERRUN);
    CHECK_EQ(record.detail, 31000);
    CHECK_EQ(record.state, KEEL_STATE_ARMED);
    fx_destroy(&f);
}

#define ENTRY_CASE(entry_name)               \
    static void case_##entry_name(void)      \
    {                                        \
        run_entry(#entry_name);              \
    }
ENTRY_CASE(tick)
ENTRY_CASE(cycle)
ENTRY_CASE(request_arm)
ENTRY_CASE(set_motors)
ENTRY_CASE(set_motors_zero)
ENTRY_CASE(set_motors_invalid)
ENTRY_CASE(brake)
ENTRY_CASE(disarm)
ENTRY_CASE(emergency_stop)
ENTRY_CASE(clear_fault)
ENTRY_CASE(take_buttons)
ENTRY_CASE(read_safety_inputs)
ENTRY_CASE(set_button_leds)
ENTRY_CASE(servo_start)
ENTRY_CASE(servo_set_pulse)
ENTRY_CASE(servo_end)
ENTRY_CASE(servo_grant_tick)
ENTRY_CASE(sleep_until)
ENTRY_CASE(range_measure)
ENTRY_CASE(close)

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"tick", case_tick},
        {"cycle", case_cycle},
        {"request_arm", case_request_arm},
        {"set_motors", case_set_motors},
        {"set_motors_zero", case_set_motors_zero},
        {"set_motors_invalid", case_set_motors_invalid},
        {"brake", case_brake},
        {"disarm", case_disarm},
        {"emergency_stop", case_emergency_stop},
        {"clear_fault", case_clear_fault},
        {"take_buttons", case_take_buttons},
        {"read_safety_inputs", case_read_safety_inputs},
        {"set_button_leds", case_set_button_leds},
        {"servo_start", case_servo_start},
        {"servo_set_pulse", case_servo_set_pulse},
        {"servo_end", case_servo_end},
        {"servo_grant_tick", case_servo_grant_tick},
        {"sleep_until", case_sleep_until},
        {"range_measure", case_range_measure},
        {"close", case_close},
        {"peripheral_hooks", peripheral_hooks},
        {"fault_latch_hooks", fault_latch_hooks},
        {"hal_contract_again", hal_contract_again},
        {"overrun_between_ticks", overrun_between_ticks},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
