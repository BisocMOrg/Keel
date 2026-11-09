/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * RGB LED driver protocol (IS31FL3218-compatible at I2C address 0x54).
 * LED i uses PWM bytes 3i (R), 3i+1 (G) and 3i+2 (B). A frame is one
 * PWM block write plus one latch, so the six LEDs never tear. Every failure
 * is KEEL_E_PERIPHERAL and never latches. Runs with LEDS held.
 */
#include <string.h>

#include "core/internal.h"

enum {
    LED_ADDRESS = 0x54,
    REG_SHUTDOWN = 0x00, /* 0 = shutdown, 1 = normal operation */
    REG_PWM = 0x01,      /* 18 PWM bytes, 0x01..0x12 */
    REG_ENABLE = 0x13,   /* three channel-enable bytes, 0x13..0x15 */
    REG_UPDATE = 0x16,   /* any write latches the PWM bytes */
    REG_RESET = 0x17,    /* any write resets every register */
    ENABLE_ALL = 0x3F,
    ENABLE_BYTES = 3
};

static int32_t led_write(keel__device *dev, uint8_t reg, const uint8_t *data, size_t count)
{
    return dev->be.i2c_write(dev->be.user, LED_ADDRESS, reg, data, count) == KEEL_OK
               ? KEEL_OK
               : KEEL_E_PERIPHERAL;
}

static int32_t led_byte(keel__device *dev, uint8_t reg, uint8_t value)
{
    return led_write(dev, reg, &value, 1);
}

int32_t keel__leds_init(keel__device *dev)
{
    static const uint8_t enables[ENABLE_BYTES] = {ENABLE_ALL, ENABLE_ALL, ENABLE_ALL};
    memset(dev->rgb, 0, sizeof dev->rgb);
    /* Shut down first, then reset, clear the PWM bytes, enable every
     * channel, latch, and wake: the LEDs never flash on start-up. */
    if (keel__leds_shutdown(dev) != KEEL_OK || led_byte(dev, REG_RESET, 0) != KEEL_OK ||
        led_write(dev, REG_PWM, dev->rgb, sizeof dev->rgb) != KEEL_OK ||
        led_write(dev, REG_ENABLE, enables, sizeof enables) != KEEL_OK ||
        led_byte(dev, REG_UPDATE, 0) != KEEL_OK || led_byte(dev, REG_SHUTDOWN, 1) != KEEL_OK) {
        return KEEL_E_PERIPHERAL;
    }
    return KEEL_OK;
}

int32_t keel__leds_frame(keel__device *dev, const uint8_t frame[KEEL__RGB_BYTES])
{
    if (led_write(dev, REG_PWM, frame, KEEL__RGB_BYTES) != KEEL_OK ||
        led_byte(dev, REG_UPDATE, 0) != KEEL_OK) {
        return KEEL_E_PERIPHERAL; /* the cache keeps the last good frame */
    }
    memcpy(dev->rgb, frame, KEEL__RGB_BYTES);
    return KEEL_OK;
}

int32_t keel__leds_shutdown(keel__device *dev)
{
    return led_byte(dev, REG_SHUTDOWN, 0);
}
