/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Board model of the simulator (see sim_board.h). The MCU input mapping
 * qualifies the host protocol against the production policy: event-mode
 * heartbeat falls, ENABLE edge counts, PWM
 * levels and edges, and feedback equal to the gate output; the servo request
 * is the J5 model's line. The HC-SR04 answers only while the MCU powers it,
 * and its edges carry exact timestamps, as kernel edge events do.
 */
#include <errno.h>
#include <string.h>

#include "backend/sim/sim_board.h"

enum {
    LED_REG_PWM = 0x01,
    LED_REG_UPDATE = 0x16,
    LED_REG_RESET = 0x17
};

/* BCM numbers of the board pins reported by keel_sim_get_state(). */
enum {
    PIN_LP = 8, PIN_RN = 9, PIN_RP = 10, PIN_LN = 11,
    PIN_BUTTON_A = 5, PIN_BUTTON_B = 6, PIN_BUTTON_X = 16, PIN_BUTTON_Y = 24,
    PIN_LED_A = 23, PIN_LED_B = 22, PIN_LED_X = 17, PIN_LED_Y = 27,
    PIN_HEARTBEAT = 18, PIN_SERVO_REQUEST = 19, PIN_READY = 20, PIN_ARMED = 21,
    PIN_ECHO = 25, PIN_ENABLE = 26
};

static const uint8_t button_pins[KEEL_BUTTON_COUNT] = {PIN_BUTTON_A, PIN_BUTTON_B, PIN_BUTTON_X,
                                                       PIN_BUTTON_Y};
static const uint8_t button_led_pins[KEEL_BUTTON_COUNT] = {PIN_LED_A, PIN_LED_B, PIN_LED_X,
                                                           PIN_LED_Y};

/* Serial-number comparison: epochs may wrap. */
static int epoch_older(uint32_t epoch, uint32_t newest)
{
    return (int32_t)(epoch - newest) < 0;
}

static uint32_t mcu_now_ms(const keel__sim_board *b)
{
    return (uint32_t)((b->now_us - b->origin_us) / 1000u);
}

static void set_levels(keel__sim_board *b, uint32_t levels)
{
    if (levels != b->levels) {
        b->levels = levels;
        b->pwm_edge = 1;
    }
}

static void coast(keel__sim_board *b)
{
    if (b->motor.mode != KEEL_MOTOR_COAST) {
        b->coast_since_us = b->now_us;
    }
    memset(&b->motor, 0, sizeof b->motor);
    b->motor.mode = KEEL_MOTOR_COAST;
    set_levels(b, 0);
}

/* Signal off first, then the request: the servo never sees a pulse
 * without power. */
static void servo_off(keel__sim_board *b)
{
    b->servo_pulse_us = 0;
    b->servo_request = 0;
}

/* Counts the edges the MCU sees (it latches F_ACK on two ENABLE edges
 * within one millisecond). */
static void set_outputs(keel__sim_board *b, uint32_t outputs)
{
    if ((b->outputs ^ outputs) & KEEL_OUT_ENABLE) {
        b->enable_edges++;
    }
    if ((b->outputs & KEEL_OUT_HEARTBEAT) && !(outputs & KEEL_OUT_HEARTBEAT)) {
        b->heartbeat_falls++;
        b->falls_total++;
    }
    b->outputs = outputs;
}

void keel__board_init(keel__sim_board *b, uint64_t now_us)
{
    memset(b, 0, sizeof *b);
    b->now_us = now_us;
    b->origin_us = now_us;
    b->next_step_us = now_us + KEEL__BOARD_STEP_US;
    b->motor.mode = KEEL_MOTOR_COAST;
    b->range_mm = -1;
    b->coast_since_us = now_us;
    b->scheduled_wake_us = now_us;
    b->fault.struct_size = sizeof b->fault;
    b->stats.struct_size = sizeof b->stats;
    b->stats.timing_class = KEEL_TIMING_NONE;
    keel__pwm_budget_reset(&b->budget);
    safety_init(&b->mcu, 0);
}

int keel__board_armed(const keel__sim_board *b)
{
    return b->armed_stuck ||
           (b->permit.motor_permit && (b->outputs & KEEL_OUT_ENABLE) && !b->fault_low);
}

int32_t keel__board_latch(keel__sim_board *b, int32_t status, uint32_t cause, int32_t os_error,
                          uint64_t detail)
{
    if (b->fault.status == KEEL_OK) {
        b->fault.status = status;
        b->fault.cause = cause;
        b->fault.os_error = os_error;
        b->fault.detail = detail;
        b->fault.time_us = b->now_us;
    }
    /* The self-stop never touches the heartbeat: the core decides that. */
    set_outputs(b, b->outputs & ~(uint32_t)KEEL_OUT_ENABLE);
    servo_off(b);
    coast(b);
    return status;
}

/* One pass of the simulated PWM worker: lease expiry, lateness budget and
 * the line pattern. A stalled generator holds its lines and its lease. */
static void generator_step(keel__sim_board *b)
{
    uint64_t lateness, detail = 0;
    uint32_t cause, levels;
    if (b->motor.mode == KEEL_MOTOR_COAST || b->pwm_stall) {
        return;
    }
    if (b->now_us >= b->motor.valid_until_us) {
        coast(b); /* the lease ran out without renewal: silent coast */
        b->stats.lease_expiries++;
        return;
    }
    lateness = b->pending_lateness_us;
    b->pending_lateness_us = 0;
    b->scheduled_wake_us = b->now_us;
    b->stats.wakes++;
    b->stats.last_lateness_us = lateness > UINT32_MAX ? UINT32_MAX : (uint32_t)lateness;
    if (b->stats.last_lateness_us > b->stats.max_lateness_us) {
        b->stats.max_lateness_us = b->stats.last_lateness_us;
    }
    if (lateness > KEEL__PWM_LATE_US) {
        b->stats.late_wakes++;
    }
    if (lateness > KEEL__PWM_VERY_LATE_US) {
        b->stats.very_late_wakes++;
    }
    cause = keel__pwm_account(&b->budget, b->now_us, lateness, &detail);
    if (cause != KEEL_CAUSE_NONE) {
        (void)keel__board_latch(b, KEEL_E_PWM, cause, 0, detail);
        return;
    }
    levels = keel__pwm_levels(b->motor.mode, b->motor.left, b->motor.right,
                              b->now_us - b->motion_start_us);
    if (levels != b->levels && b->fail_pwm_writes) {
        b->fail_pwm_writes--;
        (void)keel__board_latch(b, KEEL_E_PWM, KEEL_CAUSE_PWM_WRITE, EIO, 0);
        return;
    }
    set_levels(b, levels);
}

static void mcu_step(keel__sim_board *b)
{
    safety_input input;
    memset(&input, 0, sizeof input);
    input.now_ms = mcu_now_ms(b);
    input.enable = (b->outputs & KEEL_OUT_ENABLE) != 0;
    input.heartbeat = (b->outputs & KEEL_OUT_HEARTBEAT) != 0;
    input.servo_request = b->servo_request != 0;
    input.hardware_fault_high = !b->fault_low;
    input.hardware_fault_edge = b->fault_edge != 0;
    input.host_rail_ok = true;
    input.internal_ok = true;
    input.pwm_levels = (uint8_t)b->levels;
    input.pwm_edge_seen = b->pwm_edge != 0;
    input.enable_edges = b->enable_edges;
    input.heartbeat_falls = b->heartbeat_falls;
    input.heartbeat_event_mode = true;
    input.motor_feedback = keel__board_armed(b) != 0;
    input.servo_feedback = b->permit.servo_permit && input.enable && input.hardware_fault_high;
    input.sensor_strike_lockout = b->sensor_lockout != 0;
    b->permit = safety_step(&b->mcu, &input);
    b->pwm_edge = 0;
    b->fault_edge = 0;
    b->enable_edges = 0;
    b->heartbeat_falls = 0;
}

static void push_edge(keel__sim_board *b, uint64_t time_us, uint32_t rising)
{
    if (b->edge_count < KEEL__BOARD_EDGES) {
        keel_port_edge *edge = &b->edges[b->edge_count++];
        memset(edge, 0, sizeof *edge);
        edge->time_us = time_us;
        edge->rising = rising;
    }
}

/* Emits the echo edges due by now, stamped with their exact times as a
 * kernel edge event would be. */
static void echo_run(keel__sim_board *b)
{
    if (b->echo_phase == KEEL__ECHO_SCHEDULED && b->echo_rise_us <= b->now_us) {
        push_edge(b, b->echo_rise_us, 1);
        b->echo_phase = KEEL__ECHO_HIGH;
    }
    if (b->echo_phase == KEEL__ECHO_HIGH && b->echo_fall_us <= b->now_us) {
        push_edge(b, b->echo_fall_us, 0);
        b->echo_phase = KEEL__ECHO_IDLE;
    }
}

/* The MCU switched the sensor off: an echo in progress collapses. */
static void echo_power(keel__sim_board *b)
{
    if (b->permit.sensor_permit) {
        return;
    }
    if (b->echo_phase == KEEL__ECHO_HIGH) {
        push_edge(b, b->now_us, 0);
    }
    b->echo_phase = KEEL__ECHO_IDLE;
}

void keel__board_advance(keel__sim_board *b, uint64_t target_us)
{
    while (b->next_step_us <= target_us) {
        b->now_us = b->next_step_us;
        b->next_step_us += KEEL__BOARD_STEP_US;
        echo_run(b);
        if (b->servo_request && b->now_us >= b->servo_until_us) {
            servo_off(b); /* the backend ends a burst at its lease, without ticks */
        }
        generator_step(b);
        mcu_step(b);
        echo_power(b);
    }
    if (target_us > b->now_us) {
        b->now_us = target_us;
    }
    echo_run(b);
}

int32_t keel__board_read(keel__sim_board *b, uint32_t *inputs)
{
    if (b->fail_gpio_reads) {
        b->fail_gpio_reads--;
        return keel__board_latch(b, KEEL_E_IO, KEEL_CAUSE_GPIO_READ, EIO, 0);
    }
    *inputs = (b->permit.ready ? (uint32_t)KEEL_IN_READY : 0u) |
              (keel__board_armed(b) ? (uint32_t)KEEL_IN_ARMED : 0u) |
              (b->buttons << KEEL_IN_BUTTON_SHIFT) |
              (b->fault.status != KEEL_OK ? (uint32_t)KEEL_IN_BACKEND_FAULT : 0u);
    return KEEL_OK;
}

int32_t keel__board_write_outputs(keel__sim_board *b, uint32_t mask, uint32_t values,
                                  uint32_t stop_epoch)
{
    uint32_t next, rising;
    int32_t rc = KEEL_OK;
    if (mask & ~(uint32_t)KEEL_OUT_ALL) {
        return keel__board_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERNAL, 0, mask);
    }
    if (b->fail_gpio_writes) {
        b->fail_gpio_writes--;
        return keel__board_latch(b, KEEL_E_IO, KEEL_CAUSE_GPIO_WRITE, EIO, mask);
    }
    next = (b->outputs & ~mask) | (values & mask);
    rising = next & ~b->outputs & (uint32_t)(KEEL_OUT_ENABLE | KEEL_OUT_HEARTBEAT);
    if (rising && b->fault.status != KEEL_OK) {
        /* While latched, LOW levels and LEDs are written; rises are refused. */
        next &= ~rising;
        rc = b->fault.status;
    } else if ((rising & KEEL_OUT_ENABLE) && epoch_older(stop_epoch, b->newest_epoch)) {
        next &= ~(uint32_t)KEEL_OUT_ENABLE; /* decided before a newer stop */
        b->stats.suppressed_writes++;
    } else if ((rising & KEEL_OUT_ENABLE) &&
               (b->motor.mode != KEEL_MOTOR_COAST || b->servo_request)) {
        set_outputs(b, next & ~(uint32_t)KEEL_OUT_ENABLE);
        return keel__board_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERLOCK, 0, mask);
    }
    set_outputs(b, next);
    return rc;
}

int32_t keel__board_write_motors(keel__sim_board *b, const keel_port_motor_command *command)
{
    if (command->mode == KEEL_MOTOR_COAST) {
        coast(b); /* always applied */
        return KEEL_OK;
    }
    if ((command->mode != KEEL_MOTOR_DRIVE && command->mode != KEEL_MOTOR_BRAKE) ||
        (command->mode == KEEL_MOTOR_DRIVE &&
         (command->left < -KEEL_DUTY_MAX || command->left > KEEL_DUTY_MAX ||
          command->right < -KEEL_DUTY_MAX || command->right > KEEL_DUTY_MAX))) {
        return keel__board_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERNAL, 0, command->mode);
    }
    if (b->fault.status != KEEL_OK) {
        return b->fault.status;
    }
    if (epoch_older(command->stop_epoch, b->newest_epoch)) {
        coast(b);
        b->stats.suppressed_writes++;
        return KEEL_OK;
    }
    if (command->valid_until_us <= b->now_us) {
        coast(b); /* the lease ended before the command arrived */
        b->stats.lease_expiries++;
        return KEEL_OK;
    }
    if (!(b->outputs & KEEL_OUT_ENABLE) || b->servo_request) {
        return keel__board_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERLOCK, 0, command->mode);
    }
    /* Fresh ARMED read under the model lock: catches a drive during the
     * acknowledgement's HIGH levels, which the ENABLE check alone allows. */
    if (!keel__board_armed(b)) {
        return keel__board_latch(b, KEEL_E_FEEDBACK, KEEL_CAUSE_ARMED_LOST, 0, 0);
    }
    if (b->motor.mode != KEEL_MOTOR_COAST && b->motor.valid_until_us > b->now_us &&
        b->now_us - b->scheduled_wake_us > KEEL__PWM_STALL_US) {
        return keel__board_latch(b, KEEL_E_PWM, KEEL_CAUSE_PWM_STALL, 0,
                                 b->now_us - b->scheduled_wake_us);
    }
    if (b->motor.mode == KEEL_MOTOR_COAST) {
        /* Motion from idle: a new phase epoch and an immediate wake. */
        b->motion_start_us = b->now_us;
        b->scheduled_wake_us = b->now_us;
    }
    b->motor = *command;
    set_levels(b, keel__pwm_levels(command->mode, command->left, command->right,
                                   b->now_us - b->motion_start_us));
    return KEEL_OK;
}

int32_t keel__board_emergency_disable(keel__sim_board *b, uint32_t stop_epoch)
{
    if (!epoch_older(stop_epoch, b->newest_epoch)) {
        b->newest_epoch = stop_epoch;
    }
    set_outputs(b, b->outputs & ~(uint32_t)KEEL_OUT_ENABLE);
    servo_off(b);
    coast(b);
    return KEEL_OK;
}

int32_t keel__board_servo(keel__sim_board *b, const keel_port_servo_command *command)
{
    if (command->mode == KEEL_PORT_SERVO_OFF) {
        servo_off(b); /* always applied */
        return KEEL_OK;
    }
    if ((command->mode != KEEL_PORT_SERVO_REQUEST && command->mode != KEEL_PORT_SERVO_PULSE) ||
        (command->mode == KEEL_PORT_SERVO_PULSE &&
         (command->pulse_us < KEEL_SERVO_PULSE_MIN_US ||
          command->pulse_us > KEEL_SERVO_PULSE_MAX_US))) {
        return keel__board_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERNAL, 0, command->mode);
    }
    if (b->fault.status != KEEL_OK) {
        return b->fault.status;
    }
    if (epoch_older(command->stop_epoch, b->newest_epoch)) {
        servo_off(b);
        b->stats.suppressed_writes++;
        return KEEL_OK;
    }
    if (command->valid_until_us <= b->now_us) {
        servo_off(b); /* the lease ended before the command arrived */
        return KEEL_OK;
    }
    if (!b->servo_request) {
        /* A new burst needs ENABLE HIGH and motors quiet for longer than one
         * MCU sample, or the MCU would see PWM activity at the rise. */
        if (!(b->outputs & KEEL_OUT_ENABLE) || b->motor.mode != KEEL_MOTOR_COAST ||
            b->now_us - b->coast_since_us < KEEL__BOARD_SERVO_IDLE_US) {
            return keel__board_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERLOCK, 0, command->mode);
        }
        b->servo_request = 1;
        b->servo_until_us = command->valid_until_us;
    } else if (command->valid_until_us < b->servo_until_us) {
        b->servo_until_us = command->valid_until_us; /* shortened, never extended */
    }
    b->servo_pulse_us = command->mode == KEEL_PORT_SERVO_PULSE ? command->pulse_us : 0u;
    return KEEL_OK;
}

int32_t keel__board_range_trigger(keel__sim_board *b, uint32_t *out_echo_high)
{
    uint64_t width;
    *out_echo_high = b->echo_phase == KEEL__ECHO_HIGH;
    /* An unpowered sensor stays silent; a busy one ignores the trigger. */
    if (*out_echo_high || b->echo_phase != KEEL__ECHO_IDLE || !b->permit.sensor_permit) {
        return KEEL_OK;
    }
    width = b->range_mm >= 0 ? (uint64_t)b->range_mm * 2000u / 343u
                             : (uint64_t)KEEL__BOARD_ECHO_NOTHING_US;
    b->echo_phase = KEEL__ECHO_SCHEDULED;
    b->echo_rise_us = b->now_us + KEEL__BOARD_ECHO_DELAY_US;
    b->echo_fall_us = b->echo_rise_us + width;
    return KEEL_OK;
}

int32_t keel__board_range_edges(keel__sim_board *b, keel_port_edge *edges, uint32_t capacity,
                                uint32_t *out_count)
{
    uint32_t n = capacity < b->edge_count ? capacity : b->edge_count;
    memcpy(edges, b->edges, n * sizeof *edges);
    memmove(b->edges, b->edges + n, (b->edge_count - n) * sizeof *edges);
    b->edge_count -= n;
    *out_count = n;
    return KEEL_OK;
}

int32_t keel__board_get_fault(const keel__sim_board *b, keel_port_fault *out)
{
    uint32_t size = out->struct_size;
    *out = b->fault;
    out->struct_size = size;
    return KEEL_OK;
}

int32_t keel__board_reset_fault(keel__sim_board *b)
{
    memset(&b->fault, 0, sizeof b->fault);
    b->fault.struct_size = sizeof b->fault;
    keel__pwm_budget_reset(&b->budget);
    return KEEL_OK;
}

int32_t keel__board_i2c(keel__sim_board *b, uint8_t address, uint8_t reg, const uint8_t *data,
                        size_t count)
{
    size_t end = (size_t)reg + count;
    if (b->fail_i2c) {
        b->fail_i2c--;
        return KEEL_E_IO;
    }
    if (address != KEEL__BOARD_LED_ADDRESS || count == 0 || end > KEEL__BOARD_LED_REGISTERS) {
        return KEEL_E_IO; /* no acknowledge */
    }
    memcpy(&b->led_registers[reg], data, count);
    if (reg <= LED_REG_RESET && LED_REG_RESET < end) {
        memset(b->led_registers, 0, sizeof b->led_registers);
    }
    if (reg <= LED_REG_UPDATE && LED_REG_UPDATE < end) {
        memcpy(b->led_output, &b->led_registers[LED_REG_PWM], sizeof b->led_output);
    }
    return KEEL_OK;
}

int32_t keel__board_stats(const keel__sim_board *b, keel_port_stats *out)
{
    uint32_t size = out->struct_size;
    *out = b->stats;
    out->struct_size = size;
    return KEEL_OK;
}

/* Non-negative counter stimulus. */
static int32_t set_count(uint32_t *counter, int32_t value)
{
    if (value < 0) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    *counter = (uint32_t)value;
    return KEEL_OK;
}

int32_t keel__board_set(keel__sim_board *b, uint32_t item, int32_t value)
{
    switch (item) {
    case KEEL_SIM_BUTTONS:
        if (value < 0 || ((uint32_t)value & ~(uint32_t)KEEL_BUTTON_ALL)) {
            return KEEL_E_INVALID_ARGUMENT;
        }
        b->buttons = (uint32_t)value;
        return KEEL_OK;
    case KEEL_SIM_RANGE_MM:
        if (value < -1) {
            return KEEL_E_INVALID_ARGUMENT;
        }
        b->range_mm = value;
        return KEEL_OK;
    case KEEL_SIM_HARDWARE_FAULT:
        b->fault_edge = b->fault_edge || value != 0;
        return KEEL_OK;
    case KEEL_SIM_MCU_RESET:
        if (value) {
            safety_init(&b->mcu, mcu_now_ms(b));
            memset(&b->permit, 0, sizeof b->permit);
        }
        return KEEL_OK;
    case KEEL_SIM_FAIL_GPIO_WRITES:
        return set_count(&b->fail_gpio_writes, value);
    case KEEL_SIM_FAIL_GPIO_READS:
        return set_count(&b->fail_gpio_reads, value);
    case KEEL_SIM_FAIL_I2C:
        return set_count(&b->fail_i2c, value);
    case KEEL_SIM_SENSOR_LOCKOUT:
        b->sensor_lockout = value != 0;
        return KEEL_OK;
    case KEEL_SIM_PWM_LATENESS_US:
        if (value < 0) {
            return KEEL_E_INVALID_ARGUMENT;
        }
        b->pending_lateness_us = (uint64_t)value;
        return KEEL_OK;
    case KEEL_SIM_ARMED_STUCK_HIGH:
        b->armed_stuck = value != 0;
        return KEEL_OK;
    case KEEL_SIM_FAIL_PWM_WRITES:
        return set_count(&b->fail_pwm_writes, value);
    case KEEL_SIM_PWM_STALL:
        b->pwm_stall = value != 0;
        return KEEL_OK;
    default:
        return KEEL_E_INVALID_ARGUMENT;
    }
}

static uint32_t pin(unsigned bcm, int level)
{
    return level ? UINT32_C(1) << bcm : 0u;
}

int32_t keel__board_state(const keel__sim_board *b, keel_sim_state *out)
{
    uint32_t size = out->struct_size;
    int armed = keel__board_armed(b);
    unsigned i;
    memset(out, 0, sizeof *out);
    out->struct_size = size;
    out->pins = pin(PIN_ENABLE, (b->outputs & KEEL_OUT_ENABLE) != 0) |
                pin(PIN_HEARTBEAT, (b->outputs & KEEL_OUT_HEARTBEAT) != 0) |
                pin(PIN_LP, (b->levels & KEEL__PWM_LP) != 0) |
                pin(PIN_LN, (b->levels & KEEL__PWM_LN) != 0) |
                pin(PIN_RP, (b->levels & KEEL__PWM_RP) != 0) |
                pin(PIN_RN, (b->levels & KEEL__PWM_RN) != 0) | pin(PIN_READY, b->permit.ready) |
                pin(PIN_ARMED, armed) | pin(PIN_SERVO_REQUEST, b->servo_request) |
                pin(PIN_ECHO, b->echo_phase == KEEL__ECHO_HIGH);
    for (i = 0; i < KEEL_BUTTON_COUNT; ++i) {
        out->pins |= pin(button_pins[i], !(b->buttons & (1u << i))); /* active low */
        out->pins |= pin(button_led_pins[i],
                         (b->outputs & (1u << (KEEL_OUT_BUTTON_LED_SHIFT + i))) != 0);
    }
    out->mcu_stage = (uint32_t)b->mcu.stage;
    out->mcu_faults = b->mcu.last_faults;
    out->flags = (b->permit.motor_permit ? (uint32_t)KEEL_SIM_MOTOR_PERMIT : 0u) |
                 (b->permit.servo_permit ? (uint32_t)KEEL_SIM_SERVO_PERMIT : 0u) |
                 (b->permit.sensor_permit ? (uint32_t)KEEL_SIM_SENSOR_PERMIT : 0u) |
                 (b->permit.ready ? (uint32_t)KEEL_SIM_READY : 0u) |
                 ((b->levels & (KEEL__PWM_LP | KEEL__PWM_LN)) == (KEEL__PWM_LP | KEEL__PWM_LN)
                      ? (uint32_t)KEEL_SIM_LEFT_BRAKE
                      : 0u) |
                 ((b->levels & (KEEL__PWM_RP | KEEL__PWM_RN)) == (KEEL__PWM_RP | KEEL__PWM_RN)
                      ? (uint32_t)KEEL_SIM_RIGHT_BRAKE
                      : 0u);
    if (armed && b->motor.mode == KEEL_MOTOR_DRIVE) {
        out->bridge_left = b->motor.left;
        out->bridge_right = b->motor.right;
    }
    if (b->permit.servo_permit && b->servo_request) {
        out->servo_pulse_us = b->servo_pulse_us; /* J5 is powered only during a granted burst */
    }
    out->now_us = b->now_us;
    out->heartbeat_falls = b->falls_total;
    return KEEL_OK;
}
