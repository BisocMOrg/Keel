/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * I420 to packed RGB888 or BGR888: full-range BT.601 (sYCC) in 16-bit
 * fixed point, each chroma sample covering 2x2 pixels (SPEC section 26.5).
 * The bindings and the tests use exactly this arithmetic.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include "camera/camera_internal.h"

static uint8_t clamp_u8(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

/* Arithmetic shift right that floors for negative values on every compiler. */
static int32_t shift16(int32_t value)
{
    return value >= 0 ? value >> 16 : -(int32_t)((uint32_t)(-(value + 1)) >> 16) - 1;
}

void cam_convert_i420(const uint8_t *y, uint32_t y_stride, const uint8_t *u, uint32_t u_stride,
                      const uint8_t *v, uint32_t v_stride, uint32_t width, uint32_t height, uint8_t *out,
                      uint32_t out_stride, int bgr)
{
    uint32_t row;
    uint32_t column;
    for (row = 0; row < height; ++row) {
        const uint8_t *y_row = y + (size_t)row * y_stride;
        const uint8_t *u_row = u + (size_t)(row / 2u) * u_stride;
        const uint8_t *v_row = v + (size_t)(row / 2u) * v_stride;
        uint8_t *pixel = out + (size_t)row * out_stride;
        for (column = 0; column < width; ++column) {
            int32_t luma = y_row[column];
            int32_t d = (int32_t)u_row[column / 2u] - 128;
            int32_t e = (int32_t)v_row[column / 2u] - 128;
            uint8_t r = clamp_u8(luma + shift16(91881 * e + 32768));
            uint8_t g = clamp_u8(luma + shift16(-22554 * d - 46802 * e + 32768));
            uint8_t b = clamp_u8(luma + shift16(116130 * d + 32768));
            pixel[0] = bgr ? b : r;
            pixel[1] = g;
            pixel[2] = bgr ? r : b;
            pixel += 3;
        }
    }
}
