/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Public entry points (keel.h). Each call follows the evaluation order of
 * SPEC 13.1: process and handle first (the owner pid atomically, before any
 * lock), stop calls lower ENABLE before they lock, then arguments,
 * capability, thread, state and I/O. Arguments and capabilities are decided
 * from immutable or cached data, so those refusals are I/O-free.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "backend/hosted.h"
#include "core/internal.h"
#include "keel_build_config.h"
#include "os/os.h"

/* Handle and process check, then the single-threaded port rule. */
static int32_t enter(const keel_device *handle, keel__ref *ref)
{
    int32_t rc = keel__acquire(handle, ref, 0);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!keel__thread_allowed(ref->device)) {
        rc = keel__reject(ref->device, KEEL_E_CONFLICT);
        keel__release(ref);
    }
    return rc;
}

/* Runs a CONTROL-section body for a validated handle. */
typedef int32_t (*control_body)(keel__device *dev, void *context);

static int32_t with_control(keel__device *dev, control_body body, void *context)
{
    int32_t rc = keel__control_enter(dev);
    if (rc == KEEL_OK) {
        rc = body(dev, context);
        keel__control_leave(dev);
    }
    return rc;
}

static int32_t get_snapshot_body(keel__device *dev, void *context)
{
    keel_snapshot snapshot;
    keel__snapshot(dev, &snapshot);
    keel__out_write(context, &snapshot, sizeof snapshot);
    return KEEL_OK;
}

/* ---- lifetime ------------------------------------------------------------ */

/* Resolves AUTO through KEEL_BACKEND; never falls back to the simulator. */
static int32_t open_backend(keel_open_options *options, keel_device **out_device,
                            keel_open_report *report)
{
    uint32_t backend = options->backend;
    if (backend == KEEL_BACKEND_AUTO) {
        backend = keel__os_backend_env();
        if (backend == UINT32_MAX) {
            report->backend = KEEL_BACKEND_AUTO;
            keel__report_fail(report, KEEL_RESOURCE_BACKEND, 0, NULL,
                              "KEEL_BACKEND must be \"r3\" or \"simulator\"");
            return KEEL_E_NOT_FOUND;
        }
        if (backend == KEEL_BACKEND_AUTO) {
            backend = KEEL_BACKEND_R3;
        }
    }
    options->backend = backend;
    report->backend = backend;
    if ((options->flags & KEEL_OPEN_VIRTUAL_CLOCK) && backend != KEEL_BACKEND_SIMULATOR) {
        keel__report_fail(report, KEEL_RESOURCE_OPTIONS, 0, NULL,
                          "KEEL_OPEN_VIRTUAL_CLOCK needs the simulator backend");
        return KEEL_E_INVALID_ARGUMENT;
    }
    if (backend == KEEL_BACKEND_SIMULATOR) {
        if (options->gpiochip_path[0] || options->i2c_path[0]) {
            keel__report_fail(report, KEEL_RESOURCE_OPTIONS, 0, NULL,
                              "the simulator takes no device paths");
            return KEEL_E_INVALID_ARGUMENT;
        }
#if KEEL_HAVE_BACKEND_SIM
        return keel__sim_open(options, out_device, report);
#else
        keel__report_fail(report, KEEL_RESOURCE_BACKEND, 0, NULL,
                          "this libkeel was built without the simulator backend");
        return KEEL_E_NOT_FOUND;
#endif
    }
    /* The "built without" text exists only in a library that lacks keel_r3,
     * so tests/packaging/check_debs.sh can prove that the robot's library
     * reaches it. */
#if KEEL_HAVE_BACKEND_R3
    return keel__r3_open(options, out_device, report);
#else
    (void)out_device;
    keel__report_fail(report, KEEL_RESOURCE_BACKEND, 0, NULL,
                      "this libkeel was built without the keel_r3 backend");
    return KEEL_E_NOT_FOUND;
#endif
}

KEEL_API int32_t keel_open(const keel_open_options *options, keel_device **out_device,
                           keel_open_report *report)
{
    keel_open_report local;
    keel_open_options resolved;
    int32_t rc;
    /* Every failure, including a bad report, leaves no device behind. */
    if (out_device) {
        *out_device = NULL;
    }
    if (report && !keel__out_ok(report, sizeof *report)) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    keel__report_init(&local);
    if (!out_device) {
        keel__report_fail(&local, KEEL_RESOURCE_NONE, 0, NULL, "out_device must not be NULL");
        rc = KEEL_E_INVALID_ARGUMENT;
    } else {
        rc = keel__options_check(options, &resolved, &local);
        if (rc == KEEL_OK) {
            rc = open_backend(&resolved, out_device, &local);
        }
    }
    local.status = rc;
    if (report) {
        keel__out_write(report, &local, sizeof local);
    }
    return rc;
}

KEEL_API int32_t keel_close(keel_device *device, keel_fault *out_final_fault)
{
    keel__ref ref;
    keel__device *dev;
    keel_port_backend be;
    keel_fault final_record;
    int32_t rc, disable_rc;
    if (!device) {
        return KEEL_OK;
    }
    rc = keel__acquire(device, &ref, 1);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev = ref.device;
    /* From here every new call is refused and CONTROL sections of calls in
     * flight end at once; ENABLE falls before any lock is taken. */
    atomic_store(&dev->closing, 1);
    disable_rc = keel__emergency_disable(dev);
    keel__lock(dev, KEEL_LOCK_CONTROL);
    dev->in_stop_call = 1;
    rc = keel__close_stop(dev, disable_rc);
    dev->in_stop_call = 0;
    final_record = dev->record;
    keel__unlock(dev, KEEL_LOCK_CONTROL);
    if (dev->caps & KEEL_CAP_RGB) {
        keel__lock(dev, KEEL_LOCK_LEDS);
        (void)keel__leds_shutdown(dev);
        keel__unlock(dev, KEEL_LOCK_LEDS);
    }
    if (dev->caps & KEEL_CAP_RANGE) {
        keel__lock(dev, KEEL_LOCK_RANGE);
        keel__range_expire(dev);
        keel__unlock(dev, KEEL_LOCK_RANGE);
    }
    /* Wakes keel_sleep_until() and keel_range_measure(); both see closing. */
    if (dev->be.cancel_sleep) {
        dev->be.cancel_sleep(dev->be.user);
    }
    keel__registry_retire(&ref);
    /* destroy may free the device storage itself (hosted backends). */
    be = dev->be;
    if (be.destroy) {
        be.destroy(be.user);
    }
    if (out_final_fault) {
        if (!keel__out_ok(out_final_fault, sizeof *out_final_fault)) {
            return KEEL_E_INVALID_ARGUMENT;
        }
        keel__out_write(out_final_fault, &final_record, sizeof final_record);
    }
    return rc;
}

KEEL_API int32_t keel_get_capabilities(const keel_device *device, uint32_t *out_capabilities)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!out_capabilities) {
        rc = keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    } else {
        *out_capabilities = ref.device->caps;
    }
    keel__release(&ref);
    return rc;
}

/* ---- clock --------------------------------------------------------------- */

KEEL_API int32_t keel_now_us(const keel_device *device, uint64_t *out_now_us)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!out_now_us) {
        rc = keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    } else {
        *out_now_us = ref.device->be.now_us(ref.device->be.user);
    }
    keel__release(&ref);
    return rc;
}

static int32_t sleep_failed(keel__device *dev, void *context)
{
    return keel__hook_failed(dev, *(const int32_t *)context, KEEL_CAUSE_CLOCK, 0);
}

KEEL_API int32_t keel_sleep_until(keel_device *device, uint64_t deadline_us)
{
    keel__ref ref;
    keel__device *dev;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev = ref.device;
    if (deadline_us > dev->be.now_us(dev->be.user) + KEEL__MS(KEEL_SLEEP_AHEAD_MAX_MS)) {
        rc = keel__reject(dev, KEEL_E_INVALID_ARGUMENT);
    } else {
        /* No lock is held while sleeping; close wakes the sleeper. */
        rc = atomic_load(&dev->closing) ? KEEL_OK : dev->be.sleep_until_us(dev->be.user, deadline_us);
        if (atomic_load(&dev->closing)) {
            rc = KEEL_E_INVALID_HANDLE;
        } else if (rc != KEEL_OK) {
            rc = with_control(dev, sleep_failed, &rc);
        }
    }
    keel__release(&ref);
    return rc;
}

/* ---- safety and loop ------------------------------------------------------ */

KEEL_API int32_t keel_tick(keel_device *device, uint64_t cycle_id, uint32_t health)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = keel__tick(ref.device, cycle_id, health, NULL, NULL);
    keel__release(&ref);
    return rc;
}

KEEL_API int32_t keel_cycle(keel_device *device, const keel_cycle_input *in, keel_snapshot *out)
{
    keel__ref ref;
    keel_cycle_input input;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    /* A usable input always ticks; anything wrong with the command or the
     * output is decided after the tick (SPEC 13.1 step 3). */
    rc = keel__in_check(in, sizeof *in);
    if (rc == KEEL_E_UNSUPPORTED && keel__out_ok(out, sizeof *out)) {
        /* No tick, but the output rule still holds: only INVALID_ARGUMENT and
         * the HANDLE class leave out untouched. The snapshot already shows
         * this refusal. */
        (void)keel__reject(ref.device, KEEL_E_UNSUPPORTED);
        rc = with_control(ref.device, get_snapshot_body, out);
        rc = rc == KEEL_OK ? KEEL_E_UNSUPPORTED : rc;
    } else if (rc != KEEL_OK) {
        rc = keel__reject(ref.device, rc);
    } else {
        memcpy(&input, in, sizeof input);
        rc = keel__tick(ref.device, input.cycle_id, input.health, &input, out);
    }
    keel__release(&ref);
    return rc;
}

static int32_t request_arm_body(keel__device *dev, void *context)
{
    (void)context;
    return keel__request_arm(dev);
}

KEEL_API int32_t keel_request_arm(keel_device *device)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = with_control(ref.device, request_arm_body, NULL);
    keel__release(&ref);
    return rc;
}

/* A stop verb's CONTROL section, after its epoch already lowered ENABLE. */
typedef int32_t (*stop_verb)(keel__device *dev, int32_t disable_rc);

typedef struct stop_request {
    stop_verb verb;
    int32_t disable_rc; /* status of the emergency_disable that preceded the lock */
} stop_request;

static int32_t stop_body(keel__device *dev, void *context)
{
    const stop_request *request = context;
    int32_t rc;
    /* A fault this section records names the state the stop ended. */
    dev->in_stop_call = 1;
    rc = request->verb(dev, request->disable_rc);
    dev->in_stop_call = 0;
    return rc;
}

/* Stop first, lock second (SPEC 8 rule 2). */
static int32_t stop_call(keel_device *device, stop_verb verb)
{
    keel__ref ref;
    stop_request request;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    request.verb = verb;
    request.disable_rc = keel__emergency_disable(ref.device);
    rc = with_control(ref.device, stop_body, &request);
    keel__release(&ref);
    return rc;
}

KEEL_API int32_t keel_disarm(keel_device *device)
{
    return stop_call(device, keel__disarm);
}

KEEL_API int32_t keel_emergency_stop(keel_device *device)
{
    return stop_call(device, keel__emergency_stop);
}

static int32_t clear_fault_body(keel__device *dev, void *context)
{
    (void)context;
    return keel__clear_fault(dev);
}

KEEL_API int32_t keel_clear_fault(keel_device *device)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = with_control(ref.device, clear_fault_body, NULL);
    keel__release(&ref);
    return rc;
}

static int32_t get_state_body(keel__device *dev, void *context)
{
    *(uint32_t *)context = dev->state;
    return KEEL_OK;
}

KEEL_API int32_t keel_get_state(const keel_device *device, uint32_t *out_state)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = out_state ? with_control(ref.device, get_state_body, out_state)
                   : keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    keel__release(&ref);
    return rc;
}

KEEL_API int32_t keel_get_snapshot(const keel_device *device, keel_snapshot *out)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = keel__out_ok(out, sizeof *out) ? with_control(ref.device, get_snapshot_body, out)
                                        : keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    keel__release(&ref);
    return rc;
}

static int32_t get_fault_body(keel__device *dev, void *context)
{
    keel__out_write(context, &dev->record, sizeof dev->record);
    return KEEL_OK;
}

KEEL_API int32_t keel_get_fault(const keel_device *device, keel_fault *out)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = keel__out_ok(out, sizeof *out) ? with_control(ref.device, get_fault_body, out)
                                        : keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    keel__release(&ref);
    return rc;
}

static int32_t read_safety_body(keel__device *dev, void *context)
{
    uint64_t now;
    int32_t rc = keel__fresh_inputs(dev, &now);
    if (rc == KEEL_OK) {
        *(uint32_t *)context = dev->inputs & (KEEL_INPUT_READY | KEEL_INPUT_ARMED);
    }
    return rc;
}

KEEL_API int32_t keel_read_safety_inputs(keel_device *device, uint32_t *out_inputs)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = out_inputs ? with_control(ref.device, read_safety_body, out_inputs)
                    : keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    keel__release(&ref);
    return rc;
}

/* ---- motion ---------------------------------------------------------------- */

typedef struct motor_request {
    int32_t left;
    int32_t right;
} motor_request;

static int32_t set_motors_body(keel__device *dev, void *context)
{
    const motor_request *request = context;
    if (!keel__duty_valid(request->left) || !keel__duty_valid(request->right)) {
        return keel__motion_refuse(dev, KEEL_E_INVALID_ARGUMENT);
    }
    if (request->left == 0 && request->right == 0) {
        return keel__motion_zero(dev); /* a stop: any thread, any state */
    }
    return keel__motion_set(dev, request->left, request->right, NULL);
}

KEEL_API int32_t keel_set_motors(keel_device *device, int32_t left, int32_t right)
{
    keel__ref ref;
    motor_request request;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    request.left = left;
    request.right = right;
    rc = with_control(ref.device, set_motors_body, &request);
    keel__release(&ref);
    return rc;
}

static int32_t brake_body(keel__device *dev, void *context)
{
    (void)context;
    return keel__motion_brake(dev, NULL);
}

KEEL_API int32_t keel_brake(keel_device *device)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = with_control(ref.device, brake_body, NULL);
    keel__release(&ref);
    return rc;
}

/* ---- servo (J5) ------------------------------------------------------------ */

typedef struct servo_request {
    uint32_t pulse_us;
    uint32_t lease_ms;
} servo_request;

static int pulse_valid(uint32_t pulse_us)
{
    return pulse_us >= KEEL_SERVO_PULSE_MIN_US && pulse_us <= KEEL_SERVO_PULSE_MAX_US;
}

static int32_t servo_start_body(keel__device *dev, void *context)
{
    const servo_request *request = context;
    return keel__servo_start(dev, request->pulse_us, request->lease_ms);
}

static int32_t servo_pulse_body(keel__device *dev, void *context)
{
    return keel__servo_set_pulse(dev, ((const servo_request *)context)->pulse_us);
}

/* Arguments, then capability (SPEC 13.1 steps 3 and 4); the thread, state
 * and timing checks follow under CONTROL. */
static int32_t servo_call(keel_device *device, int valid, control_body body,
                          servo_request *request)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!valid) {
        rc = keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    } else if (!(ref.device->caps & KEEL_CAP_SERVO)) {
        rc = keel__reject(ref.device, KEEL_E_UNSUPPORTED);
    } else {
        rc = with_control(ref.device, body, request);
    }
    keel__release(&ref);
    return rc;
}

KEEL_API int32_t keel_servo_start(keel_device *device, uint32_t pulse_us, uint32_t lease_ms)
{
    servo_request request;
    request.pulse_us = pulse_us;
    request.lease_ms = lease_ms;
    return servo_call(device,
                      pulse_valid(pulse_us) && lease_ms >= KEEL_SERVO_LEASE_MIN_MS &&
                          lease_ms <= KEEL_SERVO_LEASE_MAX_MS,
                      servo_start_body, &request);
}

KEEL_API int32_t keel_servo_set_pulse(keel_device *device, uint32_t pulse_us)
{
    servo_request request;
    request.pulse_us = pulse_us;
    request.lease_ms = 0;
    return servo_call(device, pulse_valid(pulse_us), servo_pulse_body, &request);
}

static int32_t servo_end_body(keel__device *dev, void *context)
{
    (void)context;
    return keel__servo_end(dev);
}

KEEL_API int32_t keel_servo_end(keel_device *device)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    /* A stop: any thread, any state. Without the capability there is never
     * a burst to end. */
    rc = (ref.device->caps & KEEL_CAP_SERVO) ? with_control(ref.device, servo_end_body, NULL)
                                             : KEEL_OK;
    keel__release(&ref);
    return rc;
}

/* ---- ranging (J1) ------------------------------------------------------------ */

/* What ranging needs from the safety side, read under CONTROL before RANGE
 * is taken: the two domains are never held together. */
typedef struct range_gate {
    int measure;      /* keel_range_measure(): the owner rule applies */
    uint32_t power;   /* ::keel_sensor_power */
    int loop_blocked; /* the caller owns a loop with a heartbeat deadline armed */
} range_gate;

static int32_t range_gate_body(keel__device *dev, void *context)
{
    range_gate *gate = context;
    uint64_t now = keel__now(dev);
    gate->power = keel__sensor_power(dev, now);
    gate->loop_blocked = keel__is_owner(dev) && (dev->state == KEEL_STATE_ARMING ||
                                                 dev->state == KEEL_STATE_ARMED ||
                                                 dev->state == KEEL_STATE_SERVO);
    if (gate->measure && gate->loop_blocked) {
        return KEEL_OK; /* refused from cached state, without I/O */
    }
    if (gate->power == KEEL_SENSOR_PERMITTED || gate->power == KEEL_SENSOR_CONFIRMED) {
        /* The cached READY can be a whole tick old, and an MCU that reset or
         * latched since then has already switched the sensor off: confirm
         * READY with a fresh read right before TRIG may be driven. The read
         * is control I/O, so the backend latch has priority as everywhere. */
        int32_t rc = keel__fresh_inputs(dev, &now);
        if (rc != KEEL_OK) {
            return rc;
        }
        gate->power = keel__sensor_power(dev, now);
    }
    return KEEL_OK;
}

static int32_t range_start_locked(keel__device *dev, const range_gate *gate, keel_range *result)
{
    int32_t rc;
    keel__lock(dev, KEEL_LOCK_RANGE);
    rc = keel__range_start(dev, gate->power);
    keel__range_result(dev, result);
    keel__unlock(dev, KEEL_LOCK_RANGE);
    return rc;
}

/* The result an output receives when there is nothing to report. */
static void idle_result(keel_range *result)
{
    memset(result, 0, sizeof *result);
    result->struct_size = sizeof *result;
}

KEEL_API int32_t keel_range_start(keel_device *device)
{
    keel__ref ref;
    range_gate gate;
    keel_range result;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    gate.measure = 0;
    if (!(ref.device->caps & KEEL_CAP_RANGE)) {
        rc = keel__reject(ref.device, KEEL_E_UNSUPPORTED);
    } else {
        rc = with_control(ref.device, range_gate_body, &gate);
        if (rc == KEEL_OK) {
            rc = range_start_locked(ref.device, &gate, &result);
        }
    }
    keel__release(&ref);
    return rc;
}

KEEL_API int32_t keel_range_poll(keel_device *device, keel_range *out)
{
    keel__ref ref;
    keel__device *dev;
    keel_range result;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev = ref.device;
    if (!keel__out_ok(out, sizeof *out)) {
        rc = keel__reject(dev, KEEL_E_INVALID_ARGUMENT);
    } else {
        idle_result(&result);
        if (!(dev->caps & KEEL_CAP_RANGE)) {
            rc = keel__reject(dev, KEEL_E_UNSUPPORTED);
        } else {
            keel__lock(dev, KEEL_LOCK_RANGE);
            rc = keel__range_update(dev);
            keel__range_result(dev, &result);
            keel__unlock(dev, KEEL_LOCK_RANGE);
        }
        keel__out_write(out, &result, sizeof result);
    }
    keel__release(&ref);
    return rc;
}

/* Waits outside every lock, 1 ms at a time, until the measurement completes,
 * the 35 ms cap passes or close starts. */
static int32_t range_wait(keel__device *dev, uint64_t deadline, keel_range *result)
{
    for (;;) {
        uint64_t now, wake;
        int32_t rc;
        keel__lock(dev, KEEL_LOCK_RANGE);
        rc = keel__range_update(dev);
        now = dev->be.now_us(dev->be.user);
        if (rc == KEEL_OK && now >= deadline) {
            keel__range_expire(dev);
        }
        keel__range_result(dev, result);
        keel__unlock(dev, KEEL_LOCK_RANGE);
        if (rc != KEEL_OK || result->status != KEEL_RANGE_PENDING) {
            return rc;
        }
        wake = now + KEEL__MS(1) < deadline ? now + KEEL__MS(1) : deadline;
        rc = dev->be.sleep_until_us(dev->be.user, wake);
        if (atomic_load(&dev->closing)) {
            return KEEL_E_INVALID_HANDLE;
        }
        if (rc != KEEL_OK) {
            return with_control(dev, sleep_failed, &rc);
        }
    }
}

KEEL_API int32_t keel_range_measure(keel_device *device, keel_range *out)
{
    keel__ref ref;
    keel__device *dev;
    range_gate gate;
    keel_range result;
    uint64_t deadline;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev = ref.device;
    if (!keel__out_ok(out, sizeof *out)) {
        rc = keel__reject(dev, KEEL_E_INVALID_ARGUMENT);
        keel__release(&ref);
        return rc;
    }
    idle_result(&result);
    deadline = dev->be.now_us(dev->be.user) + KEEL__MS(KEEL_RANGE_MEASURE_MAX_MS);
    gate.measure = 1;
    if (!(dev->caps & KEEL_CAP_RANGE)) {
        rc = keel__reject(dev, KEEL_E_UNSUPPORTED);
    } else {
        rc = with_control(dev, range_gate_body, &gate);
        if (rc == KEEL_OK && gate.loop_blocked) {
            /* Blocking here would stall the heartbeat this thread owes the MCU. */
            rc = keel__reject(dev, KEEL_E_CONFLICT);
            keel__lock(dev, KEEL_LOCK_RANGE);
            keel__range_result(dev, &result);
            keel__unlock(dev, KEEL_LOCK_RANGE);
        } else if (rc == KEEL_OK) {
            rc = range_start_locked(dev, &gate, &result);
            if (rc == KEEL_OK && result.status == KEEL_RANGE_PENDING) {
                rc = range_wait(dev, deadline, &result);
            }
        }
    }
    if (rc != KEEL_E_INVALID_HANDLE) {
        keel__out_write(out, &result, sizeof result);
    }
    keel__release(&ref);
    return rc;
}

/* ---- LEDs ------------------------------------------------------------------- */

static void put_rgb(uint8_t frame[KEEL__RGB_BYTES], uint32_t index, uint32_t rgb24)
{
    frame[index * 3u] = (uint8_t)(rgb24 >> 16);
    frame[index * 3u + 1u] = (uint8_t)(rgb24 >> 8);
    frame[index * 3u + 2u] = (uint8_t)rgb24;
}

/* Builds a frame from the cache under LEDS and writes it; never touches CONTROL. */
static int32_t write_frame(keel__device *dev, const uint32_t *rgb24, uint32_t first,
                           uint32_t count, int same_colour)
{
    uint8_t frame[KEEL__RGB_BYTES];
    uint32_t i;
    int32_t rc;
    keel__lock(dev, KEEL_LOCK_LEDS);
    if (atomic_load(&dev->closing)) {
        keel__unlock(dev, KEEL_LOCK_LEDS); /* close is waiting for this lock */
        return KEEL_E_INVALID_HANDLE;
    }
    memcpy(frame, dev->rgb, sizeof frame);
    for (i = 0; i < count; ++i) {
        put_rgb(frame, first + i, same_colour ? rgb24[0] : rgb24[i]);
    }
    rc = keel__leds_frame(dev, frame);
    keel__unlock(dev, KEEL_LOCK_LEDS);
    return rc;
}

static int32_t led_call(keel_device *device, int valid, const uint32_t *rgb24, uint32_t first,
                        uint32_t count, int same_colour)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!valid) {
        rc = keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    } else if (!(ref.device->caps & KEEL_CAP_RGB)) {
        rc = keel__reject(ref.device, KEEL_E_UNSUPPORTED);
    } else {
        rc = write_frame(ref.device, rgb24, first, count, same_colour);
    }
    keel__release(&ref);
    return rc;
}

KEEL_API int32_t keel_set_rgb(keel_device *device, uint32_t index, uint32_t rgb24)
{
    return led_call(device, index < KEEL_LED_COUNT, &rgb24, index, 1, 1);
}

KEEL_API int32_t keel_set_rgb_all(keel_device *device, uint32_t rgb24)
{
    return led_call(device, 1, &rgb24, 0, KEEL_LED_COUNT, 1);
}

KEEL_API int32_t keel_set_rgb_frame(keel_device *device, const uint32_t *rgb24, uint32_t count)
{
    return led_call(device, rgb24 && count >= 1 && count <= KEEL_LED_COUNT, rgb24, 0, count, 0);
}

KEEL_API int32_t keel_get_rgb(const keel_device *device, uint32_t index, uint32_t *out_rgb24)
{
    keel__ref ref;
    keel__device *dev;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev = ref.device;
    if (index >= KEEL_LED_COUNT || !out_rgb24) {
        rc = keel__reject(dev, KEEL_E_INVALID_ARGUMENT);
    } else {
        keel__lock(dev, KEEL_LOCK_LEDS);
        *out_rgb24 = (uint32_t)dev->rgb[index * 3u] << 16 | (uint32_t)dev->rgb[index * 3u + 1u] << 8 |
                     (uint32_t)dev->rgb[index * 3u + 2u];
        keel__unlock(dev, KEEL_LOCK_LEDS);
    }
    keel__release(&ref);
    return rc;
}

typedef struct button_leds_request {
    uint32_t mask;
    uint32_t values;
} button_leds_request;

static int32_t button_leds_body(keel__device *dev, void *context)
{
    const button_leds_request *request = context;
    uint32_t lit;
    int32_t rc = keel__sample(dev);
    if (rc != KEEL_OK) {
        return rc;
    }
    lit = (dev->button_leds & ~request->mask) | (request->values & request->mask);
    /* These lines share the control request with ENABLE and the heartbeat,
     * so a failure is a control-path failure. */
    rc = dev->be.write_outputs(dev->be.user, request->mask << KEEL_OUT_BUTTON_LED_SHIFT,
                               lit << KEEL_OUT_BUTTON_LED_SHIFT, dev->handled_epoch);
    if (rc != KEEL_OK) {
        return keel__hook_failed(dev, rc, KEEL_CAUSE_GPIO_WRITE,
                                 request->mask << KEEL_OUT_BUTTON_LED_SHIFT);
    }
    dev->button_leds = lit;
    return KEEL_OK;
}

KEEL_API int32_t keel_set_button_leds(keel_device *device, uint32_t mask, uint32_t values)
{
    keel__ref ref;
    button_leds_request request;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    request.mask = mask;
    request.values = values;
    rc = (mask & ~(uint32_t)KEEL_BUTTON_ALL)
             ? keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT)
             : with_control(ref.device, button_leds_body, &request);
    keel__release(&ref);
    return rc;
}

/* ---- buttons ---------------------------------------------------------------- */

static int32_t take_buttons_body(keel__device *dev, void *context)
{
    keel_buttons result;
    uint64_t now;
    int32_t rc = keel__fresh_inputs(dev, &now);
    if (rc == KEEL_OK) {
        keel__buttons_debounce(dev, now);
    }
    /* The output is always fully written; edges are consumed only on success. */
    memset(&result, 0, sizeof result);
    result.struct_size = sizeof result;
    result.held = dev->buttons.held;
    result.pressed = dev->buttons.pressed;
    result.released = dev->buttons.released;
    keel__out_write(context, &result, sizeof result);
    if (rc == KEEL_OK) {
        dev->buttons.pressed = 0;
        dev->buttons.released = 0;
    }
    return rc;
}

KEEL_API int32_t keel_take_buttons(keel_device *device, keel_buttons *out)
{
    keel__ref ref;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    rc = keel__out_ok(out, sizeof *out) ? with_control(ref.device, take_buttons_body, out)
                                        : keel__reject(ref.device, KEEL_E_INVALID_ARGUMENT);
    keel__release(&ref);
    return rc;
}

/* ---- simulator ----------------------------------------------------------- */

KEEL_API int32_t keel_sim_set(keel_device *device, uint32_t item, int32_t value)
{
    keel__ref ref;
    keel__device *dev;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev = ref.device;
    if (item < KEEL_SIM_BUTTONS || item > KEEL_SIM_PWM_STALL) {
        rc = keel__reject(dev, KEEL_E_INVALID_ARGUMENT);
    } else if (!dev->be.sim_set) {
        rc = keel__reject(dev, KEEL_E_UNSUPPORTED);
    } else {
        /* The stimulus touches only the backend's model, under its own lock. */
        rc = dev->be.sim_set(dev->be.user, item, value);
        if (rc != KEEL_OK) {
            rc = keel__reject(dev, KEEL_E_INVALID_ARGUMENT);
        }
    }
    keel__release(&ref);
    return rc;
}

KEEL_API int32_t keel_sim_get_state(const keel_device *device, keel_sim_state *out)
{
    keel__ref ref;
    keel__device *dev;
    keel_sim_state state;
    int32_t rc = enter(device, &ref);
    if (rc != KEEL_OK) {
        return rc;
    }
    dev = ref.device;
    memset(&state, 0, sizeof state);
    state.struct_size = sizeof state;
    if (!keel__out_ok(out, sizeof *out)) {
        rc = keel__reject(dev, KEEL_E_INVALID_ARGUMENT);
    } else if (!dev->be.sim_get_state) {
        keel__out_write(out, &state, sizeof state); /* fully written, as every non-argument status */
        rc = keel__reject(dev, KEEL_E_UNSUPPORTED);
    } else {
        rc = dev->be.sim_get_state(dev->be.user, &state);
        if (rc != KEEL_OK) {
            /* The simulator's ground truth cannot fail; anything else is a
             * port defect, reported without touching the safety state. */
            memset(&state, 0, sizeof state);
            state.struct_size = sizeof state;
            rc = keel__reject(dev, KEEL_E_UNSUPPORTED);
        }
        keel__out_write(out, &state, sizeof state);
    }
    keel__release(&ref);
    return rc;
}
