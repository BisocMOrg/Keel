/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Button debounce: a raw level must hold for 20 ms before the
 * debounced level follows it; each change accumulates a press or release
 * edge until it is taken. A button held at open becomes a press after 20 ms.
 */
#include "core/internal.h"

/* Buttons are reported as logical "pressed" bits above KEEL_IN_BUTTON_SHIFT. */
static uint32_t raw_buttons(const keel__device *dev)
{
    return (dev->inputs & KEEL_IN_BUTTONS) >> KEEL_IN_BUTTON_SHIFT;
}

void keel__buttons_init(keel__device *dev, uint64_t now)
{
    unsigned i;
    dev->buttons.candidate = raw_buttons(dev);
    for (i = 0; i < KEEL_BUTTON_COUNT; ++i) {
        dev->buttons.since_us[i] = now;
    }
}

void keel__buttons_debounce(keel__device *dev, uint64_t now)
{
    keel__buttons *b = &dev->buttons;
    uint32_t raw = raw_buttons(dev);
    unsigned i;
    for (i = 0; i < KEEL_BUTTON_COUNT; ++i) {
        uint32_t bit = 1u << i;
        if ((raw ^ b->candidate) & bit) {
            b->candidate ^= bit;
            b->since_us[i] = now;
        } else if (((b->held ^ raw) & bit) && now - b->since_us[i] >= KEEL__MS(KEEL__DEBOUNCE_MS)) {
            b->held ^= bit;
            if (raw & bit) {
                b->pressed |= bit;
            } else {
                b->released |= bit;
            }
        }
    }
}
