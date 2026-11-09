/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * The core built with the single-threaded OS layer (src/os/os_none.c), as a
 * bare-metal port would use it: attach, arm against the reference MCU,
 * drive, close, and generation-checked handles without any OS lock.
 */
#include "check.h"
#include "fixture.h"

static void lifecycle(void)
{
    fixture f;
    keel_device *closed;
    uint32_t state = 0;
    fx_init(&f);
    fx_arm(&f);
    CHECK_RC(keel_set_motors(f.device, 300, -300), KEEL_OK);
    CHECK(fx_left(&f) == 300 && fx_right(&f) == -300);
    CHECK_RC(keel_disarm(f.device), KEEL_OK);
    fx_safe(&f);
    closed = f.device;
    fx_destroy(&f);
    CHECK_RC(keel_get_state(closed, &state), KEEL_E_INVALID_HANDLE);
    CHECK_RC(keel_close(closed, NULL), KEEL_E_INVALID_HANDLE);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"lifecycle", lifecycle},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
