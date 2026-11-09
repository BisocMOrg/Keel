/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Wrapper translation unit: compiles the unmodified production policy
 * (safety_core.c) under the keel__mcu_* names of mcu_policy.h. Nothing but
 * the simulator and the reference tests links these symbols.
 */
#include "mcu_policy.h"

#include "safety_core.c"
