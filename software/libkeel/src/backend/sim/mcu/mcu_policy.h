/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file mcu_policy.h
 * @brief The production MCU policy (safety_core.h, unmodified) with its
 *        global functions renamed into libkeel's internal namespace, so the
 *        static archive exports only keel_ and keel__ symbols.
 *
 * Include this header, never safety_core.h directly; mcu_policy.c compiles
 * safety_core.c under the same names.
 */
#ifndef KEEL_SIM_MCU_POLICY_H
#define KEEL_SIM_MCU_POLICY_H

#define safety_init keel__mcu_safety_init
#define safety_step keel__mcu_safety_step
#define sensor_guard_valid keel__mcu_sensor_guard_valid
#define sensor_guard_boot keel__mcu_sensor_guard_boot
#define sensor_guard_trip keel__mcu_sensor_guard_trip
#define sensor_guard_tick keel__mcu_sensor_guard_tick
#define sensor_guard_lockout keel__mcu_sensor_guard_lockout

#include "safety_core.h"

#endif /* KEEL_SIM_MCU_POLICY_H */
