/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file pwm_policy.h
 * @brief Pure motor-PWM policy shared by the keel_r3 PWM worker and the
 *        simulator: the 100 Hz line pattern and the lateness budget
 *        (SPEC section 17.4).
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#ifndef KEEL_BACKEND_PWM_POLICY_H
#define KEEL_BACKEND_PWM_POLICY_H

#include <stdint.h>

#include "keel/port.h"

/** @brief Motor line bits returned by keel__pwm_levels(). */
enum keel__pwm_line {
    KEEL__PWM_LP = 1,  /**< BCM8: left bridge, reverse input. */
    KEEL__PWM_LN = 2,  /**< BCM11: left bridge, forward input. */
    KEEL__PWM_RP = 4,  /**< BCM10: right bridge, forward input. */
    KEEL__PWM_RN = 8,  /**< BCM9: right bridge, reverse input. */
    KEEL__PWM_ALL = 15 /**< All four inputs (BRAKE). */
};

/** @brief Timing of the PWM generator and its fault thresholds. */
enum keel__pwm_limit {
    KEEL__PWM_PERIOD_US = 10000,    /**< One 100 Hz period. */
    KEEL__PWM_LATE_US = 1000,       /**< A wake later than this counts as late. */
    KEEL__PWM_VERY_LATE_US = 5000,  /**< A wake later than this counts as very late. */
    KEEL__PWM_OVERRUN_US = 30000,   /**< One wake later than this latches PWM_OVERRUN. */
    KEEL__PWM_WINDOW_US = 100000,   /**< Sliding window of the lateness budget. */
    KEEL__PWM_BUDGET_US = 20000,    /**< Excess lateness in the window that latches PWM_DEGRADED. */
    KEEL__PWM_STALL_US = 25000,     /**< Scheduled wake this far in the past is a stall. */
    KEEL__PWM_HISTORY = 128         /**< Late wakes remembered for the window. */
};

/** @brief Sliding-window record of late wakes (excess over 1 ms). */
typedef struct keel__pwm_budget {
    uint64_t time_us[KEEL__PWM_HISTORY];   /**< Time of each late wake. */
    uint32_t excess_us[KEEL__PWM_HISTORY]; /**< Its lateness beyond 1 ms. */
    uint32_t first;                        /**< Oldest entry. */
    uint32_t count;                        /**< Entries in the window. */
    uint64_t sum_us;                       /**< Sum of their excess. */
} keel__pwm_budget;

/**
 * @brief Line levels of a motor command at a point of its period.
 * @param[in] mode     ::keel_port_motor_mode.
 * @param[in] left     Left duty, permille (positive drives BCM11).
 * @param[in] right    Right duty, permille (positive drives BCM10).
 * @param[in] phase_us Time since the motion-start epoch.
 * @return ::keel__pwm_line bits that are HIGH.
 */
uint32_t keel__pwm_levels(uint32_t mode, int32_t left, int32_t right, uint64_t phase_us);

/**
 * @brief Empties the lateness budget.
 * @param[out] budget Budget to reset.
 */
void keel__pwm_budget_reset(keel__pwm_budget *budget);

/**
 * @brief Accounts one generator wake. Late wakes are never fatal one by one:
 *        only a single wake over 30 ms or more than 20 ms of excess
 *        lateness within 100 ms latch.
 * @param[in,out] budget      Lateness budget.
 * @param[in]     now_us      Time of the wake.
 * @param[in]     lateness_us How late the wake came.
 * @param[out]    detail      Lateness (OVERRUN) or excess sum (DEGRADED), us.
 * @return ::KEEL_CAUSE_NONE, ::KEEL_CAUSE_PWM_OVERRUN or ::KEEL_CAUSE_PWM_DEGRADED.
 */
uint32_t keel__pwm_account(keel__pwm_budget *budget, uint64_t now_us, uint64_t lateness_us,
                           uint64_t *detail);

#endif /* KEEL_BACKEND_PWM_POLICY_H */
