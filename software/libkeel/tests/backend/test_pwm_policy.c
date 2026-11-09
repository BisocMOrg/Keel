/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Unit tests of the shared PWM policy (SPEC 17.4): the 100 Hz line pattern
 * and the lateness budget.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include "backend/pwm_policy.h"
#include "check.h"

static uint32_t drive(int32_t left, int32_t right, uint64_t phase_us)
{
    return keel__pwm_levels(KEEL_MOTOR_DRIVE, left, right, phase_us);
}

static void pattern(void)
{
    int32_t duty;
    uint64_t phase;
    /* Left forward drives BCM11 (LN), right forward BCM10 (RP); 10 us per permille. */
    CHECK_EQ(drive(500, 0, 0), KEEL__PWM_LN);
    CHECK_EQ(drive(500, 0, 4999), KEEL__PWM_LN);
    CHECK_EQ(drive(500, 0, 5000), 0);
    CHECK_EQ(drive(-500, 0, 100), KEEL__PWM_LP);
    CHECK_EQ(drive(0, 1000, 9999), KEEL__PWM_RP);
    CHECK_EQ(drive(0, -1000, 12345), KEEL__PWM_RN);
    CHECK_EQ(drive(1, -1, 5), KEEL__PWM_LN | KEEL__PWM_RN);
    CHECK_EQ(drive(1, 1, 10), 0);
    CHECK_EQ(drive(250, 250, 10000 + 2499), KEEL__PWM_LN | KEEL__PWM_RP); /* periodic */
    CHECK_EQ(keel__pwm_levels(KEEL_MOTOR_BRAKE, 0, 0, 1234), KEEL__PWM_ALL);
    CHECK_EQ(keel__pwm_levels(KEEL_MOTOR_COAST, 1000, 1000, 0), 0);
    /* Driving never raises both inputs of one bridge (that is brake only). */
    for (duty = -1000; duty <= 1000; duty += 50) {
        for (phase = 0; phase < KEEL__PWM_PERIOD_US; phase += 250) {
            uint32_t levels = drive(duty, -duty, phase);
            CHECK((levels & (KEEL__PWM_LP | KEEL__PWM_LN)) != (KEEL__PWM_LP | KEEL__PWM_LN));
            CHECK((levels & (KEEL__PWM_RP | KEEL__PWM_RN)) != (KEEL__PWM_RP | KEEL__PWM_RN));
        }
    }
}

/* Single late wakes of 1.9, 2.5 and 9.9 ms do not latch, and the late wake
 * recomputes the output of the phase it really runs at. */
static void late_wakes_tolerated(void)
{
    keel__pwm_budget budget;
    uint64_t detail = 0;
    keel__pwm_budget_reset(&budget);
    CHECK_EQ(keel__pwm_account(&budget, 10000, 1900, &detail), KEEL_CAUSE_NONE);
    CHECK_EQ(keel__pwm_account(&budget, 20000, 2500, &detail), KEEL_CAUSE_NONE);
    CHECK_EQ(keel__pwm_account(&budget, 30000, 9900, &detail), KEEL_CAUSE_NONE);
    CHECK_EQ(budget.sum_us, 900 + 1500 + 8900);
    /* An edge due at 3 ms (duty 500 ends its HIGH at 5 ms): */
    CHECK_EQ(drive(500, 0, 3000 + 1900), KEEL__PWM_LN);
    CHECK_EQ(drive(500, 0, 3000 + 2500), 0);
    CHECK_EQ(drive(500, 0, 3000 + 9900), KEEL__PWM_LN); /* the next period has begun */
    /* Wakes up to 1 ms late cost nothing; old excess leaves the window. */
    CHECK_EQ(keel__pwm_account(&budget, 40000, 1000, &detail), KEEL_CAUSE_NONE);
    CHECK_EQ(budget.count, 3);
    CHECK_EQ(keel__pwm_account(&budget, 140000, 0, &detail), KEEL_CAUSE_NONE);
    CHECK(budget.count == 0 && budget.sum_us == 0);
}

static void overrun(void)
{
    keel__pwm_budget budget;
    uint64_t detail = 0;
    keel__pwm_budget_reset(&budget);
    CHECK_EQ(keel__pwm_account(&budget, 5000, 31000, &detail), KEEL_CAUSE_PWM_OVERRUN);
    CHECK_EQ(detail, 31000);
}

static void degraded(void)
{
    keel__pwm_budget budget;
    uint64_t detail = 0;
    uint64_t i;
    keel__pwm_budget_reset(&budget);
    /* 8.9 ms of excess every 10 ms: the third wake crosses 20 ms in 100 ms. */
    CHECK_EQ(keel__pwm_account(&budget, 0, 9900, &detail), KEEL_CAUSE_NONE);
    CHECK_EQ(keel__pwm_account(&budget, 10000, 9900, &detail), KEEL_CAUSE_NONE);
    CHECK_EQ(keel__pwm_account(&budget, 20000, 9900, &detail), KEEL_CAUSE_PWM_DEGRADED);
    CHECK_EQ(detail, 3 * 8900);
    /* The same wakes spread wider than the window never latch. */
    keel__pwm_budget_reset(&budget);
    for (i = 0; i < 50; ++i) {
        CHECK_EQ(keel__pwm_account(&budget, i * 60000, 9900, &detail), KEEL_CAUSE_NONE);
    }
    /* A full history keeps an exact sum (oldest entries are dropped). */
    keel__pwm_budget_reset(&budget);
    for (i = 0; i < 2 * KEEL__PWM_HISTORY; ++i) {
        CHECK_EQ(keel__pwm_account(&budget, i * 100, 1001, &detail), KEEL_CAUSE_NONE);
    }
    CHECK_EQ(budget.count, KEEL__PWM_HISTORY);
    CHECK_EQ(budget.sum_us, KEEL__PWM_HISTORY);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"pattern", pattern},
        {"late_wakes_tolerated", late_wakes_tolerated},
        {"overrun", overrun},
        {"degraded", degraded},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
