/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Status, class and fault-cause tables (SPEC sections 5 and 12.5). Every
 * string is static and well under KEEL_STRING_MAX bytes.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include "core/internal.h"

/* One row per status code, indexed by -status. */
typedef struct status_info {
    const char *name;
    const char *text;
    uint32_t status_class;
} status_info;

static const status_info statuses[] = {
    {"KEEL_OK", "Success.", KEEL_CLASS_OK},
    {"KEEL_E_INVALID_ARGUMENT",
     "Invalid argument: check pointers, struct_size, reserved fields and value ranges.",
     KEEL_CLASS_REJECTED},
    {"KEEL_E_IO",
     "Control-path I/O failed; the device stopped and is FAULTED. Read keel_get_fault(), "
     "check the board, then call keel_clear_fault().",
     KEEL_CLASS_FAULT},
    {"KEEL_E_STATE",
     "Not allowed in the current state: arm first, or wait for the re-arm hold-off after a fault.",
     KEEL_CLASS_REJECTED},
    {"KEEL_E_HEALTH",
     "The application reported bad health, repeated a cycle id or ticked from another thread; "
     "the device stopped.",
     KEEL_CLASS_FAULT},
    {"KEEL_E_DEADLINE",
     "A tick or heartbeat was late while arming, armed or in a servo burst; the device stopped. "
     "Keep the loop within 70 ms.",
     KEEL_CLASS_FAULT},
    {"KEEL_E_FEEDBACK",
     "READY/ARMED feedback disagreed with the commanded state; the device stopped. Check the "
     "safety MCU and the enable gate.",
     KEEL_CLASS_FAULT},
    {"KEEL_E_AGAIN", "Temporarily refused; retry later.", KEEL_CLASS_REJECTED},
    {"KEEL_E_NO_MEMORY", "Out of memory or handle slots while opening; close unused devices.",
     KEEL_CLASS_SETUP},
    {"KEEL_E_PWM",
     "The motor PWM generator failed; the device stopped. Check system load and real-time "
     "priority (keel-doctor).",
     KEEL_CLASS_FAULT},
    {"KEEL_E_FAULTED",
     "The device is latched FAULTED: read keel_get_fault(), then call keel_clear_fault() from "
     "the control loop thread.",
     KEEL_CLASS_FAULT},
    {"KEEL_E_IN_USE",
     "A GPIO line is held by another process, device or kernel function; see the open report.",
     KEEL_CLASS_SETUP},
    {"KEEL_E_PERMISSION",
     "Permission denied on a device node; run keel-setup to add the user to the right groups.",
     KEEL_CLASS_SETUP},
    {"KEEL_E_NOT_FOUND",
     "The board, a kernel interface or the requested backend was not found; see the open report.",
     KEEL_CLASS_SETUP},
    {"KEEL_E_UNSUPPORTED", "Not supported by this device or library version.",
     KEEL_CLASS_REJECTED},
    {"KEEL_E_CONFLICT",
     "Conflicts with the control owner thread or an active instruction; issue it from the "
     "control loop.",
     KEEL_CLASS_REJECTED},
    {"KEEL_E_PERIPHERAL",
     "A non-safety peripheral (RGB LEDs or ultrasonic sensor) failed; the safety state is "
     "unchanged.",
     KEEL_CLASS_PERIPHERAL},
    {"KEEL_E_ACK_TIMEOUT",
     "Arming did not complete within 2 s; check that the safety MCU is powered and READY.",
     KEEL_CLASS_FAULT},
    {"KEEL_E_INVALID_HANDLE", "The device handle is closed, stale or invalid.",
     KEEL_CLASS_HANDLE},
    {"KEEL_E_NOT_OWNER",
     "The device belongs to another process (for example the parent of a fork).",
     KEEL_CLASS_HANDLE},
    {"KEEL_E_INTERNAL",
     "Internal invariant or backend contract failure; close and reopen the device and report "
     "the fault record.",
     KEEL_CLASS_FAULT},
    {"KEEL_E_SETUP_IO", "An I/O operation failed while opening; see the open report.",
     KEEL_CLASS_SETUP},
};

static const char *const cause_names[] = {
    "NONE",           "HEALTH_FLAGS",  "CYCLE_NOT_INCREASING", "TICK_THREAD_CHANGED",
    "TICK_LATE",      "HEARTBEAT_LATE", "READY_LOST",          "ARMED_LOST",
    "ARMED_UNEXPECTED", "ACK_NO_READY", "ACK_NO_ARMED",        "ACK_FEEDBACK",
    "SERVO_NOT_GRANTED", "DUAL_PERMIT", "PWM_OVERRUN",         "PWM_DEGRADED",
    "PWM_STALL",      "PWM_WRITE",     "GPIO_WRITE",           "GPIO_READ",
    "MOTOR_WRITE",    "CLOCK",         "INTERLOCK",            "HAL_CONTRACT",
    "INTERNAL",       "USER_STOP",
};

static const status_info *lookup(int32_t status)
{
    if (status > 0 || status < -(int32_t)(sizeof statuses / sizeof statuses[0] - 1)) {
        return NULL;
    }
    return &statuses[-status];
}

KEEL_API const char *keel_status_name(int32_t status)
{
    const status_info *info = lookup(status);
    return info ? info->name : "KEEL_E_UNKNOWN";
}

KEEL_API const char *keel_status_string(int32_t status)
{
    const status_info *info = lookup(status);
    return info ? info->text : "Unknown status code.";
}

KEEL_API uint32_t keel_status_class(int32_t status)
{
    const status_info *info = lookup(status);
    /* An undefined value is read conservatively as a fault. */
    return info ? info->status_class : KEEL_CLASS_FAULT;
}

KEEL_API const char *keel_fault_cause_name(uint32_t cause)
{
    return cause < sizeof cause_names / sizeof cause_names[0] ? cause_names[cause] : "UNKNOWN";
}

int keel__is_fault_status(int32_t status)
{
    /* Unlike keel_status_class(), an undefined value is not a fault here: a
     * backend returning it has broken the port contract. */
    const status_info *info = lookup(status);
    return info && info->status_class == KEEL_CLASS_FAULT;
}

int keel__cause_sticky(uint32_t cause)
{
    switch (cause) {
    case KEEL_CAUSE_ARMED_UNEXPECTED:
    case KEEL_CAUSE_INTERLOCK:
    case KEEL_CAUSE_INTERNAL:
    case KEEL_CAUSE_CLOCK:
    case KEEL_CAUSE_HAL_CONTRACT:
        return 1;
    default:
        return 0;
    }
}
