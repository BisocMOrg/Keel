/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/* Pure PWM pattern and lateness policy (see pwm_policy.h). */
#include <string.h>

#include "backend/pwm_policy.h"

/* HIGH time of a duty within one period: 10 us per permille. */
static uint64_t on_time_us(int32_t duty)
{
    uint64_t magnitude = duty < 0 ? (uint64_t)(-(int64_t)duty) : (uint64_t)duty;
    return magnitude * (KEEL__PWM_PERIOD_US / KEEL_DUTY_MAX);
}

uint32_t keel__pwm_levels(uint32_t mode, int32_t left, int32_t right, uint64_t phase_us)
{
    uint64_t phase = phase_us % KEEL__PWM_PERIOD_US;
    uint32_t levels = 0;
    if (mode == KEEL_MOTOR_BRAKE) {
        return KEEL__PWM_ALL;
    }
    if (mode != KEEL_MOTOR_DRIVE) {
        return 0;
    }
    /* Each bridge drives one input; the opposite input stays LOW. */
    if (left && phase < on_time_us(left)) {
        levels |= left > 0 ? (uint32_t)KEEL__PWM_LN : (uint32_t)KEEL__PWM_LP;
    }
    if (right && phase < on_time_us(right)) {
        levels |= right > 0 ? (uint32_t)KEEL__PWM_RP : (uint32_t)KEEL__PWM_RN;
    }
    return levels;
}

void keel__pwm_budget_reset(keel__pwm_budget *budget)
{
    memset(budget, 0, sizeof *budget);
}

static void drop_oldest(keel__pwm_budget *budget)
{
    budget->sum_us -= budget->excess_us[budget->first];
    budget->first = (budget->first + 1u) % KEEL__PWM_HISTORY;
    budget->count--;
}

uint32_t keel__pwm_account(keel__pwm_budget *budget, uint64_t now_us, uint64_t lateness_us,
                           uint64_t *detail)
{
    uint32_t index;
    while (budget->count && now_us - budget->time_us[budget->first] >= KEEL__PWM_WINDOW_US) {
        drop_oldest(budget);
    }
    if (lateness_us > KEEL__PWM_OVERRUN_US) {
        *detail = lateness_us;
        return KEEL_CAUSE_PWM_OVERRUN;
    }
    if (lateness_us <= KEEL__PWM_LATE_US) {
        return KEEL_CAUSE_NONE;
    }
    if (budget->count == KEEL__PWM_HISTORY) {
        drop_oldest(budget);
    }
    index = (budget->first + budget->count) % KEEL__PWM_HISTORY;
    budget->time_us[index] = now_us;
    budget->excess_us[index] = (uint32_t)(lateness_us - KEEL__PWM_LATE_US);
    budget->count++;
    budget->sum_us += budget->excess_us[index];
    if (budget->sum_us > KEEL__PWM_BUDGET_US) {
        *detail = budget->sum_us;
        return KEEL_CAUSE_PWM_DEGRADED;
    }
    return KEEL_CAUSE_NONE;
}
