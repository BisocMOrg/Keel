/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * The struct versioning rule of the ABI (Linux copy_struct_from_user): every
 * struct starts with its uint32_t struct_size, set by the caller.
 */
#include <string.h>

#include "core/internal.h"

/* Reads the leading struct_size without assuming the caller's alignment. */
static uint32_t struct_size_of(const void *object)
{
    uint32_t size;
    memcpy(&size, object, sizeof size);
    return size;
}

/* A size the caller's buffer can really have: at least the 1.0 size, and
 * not the garbage of an uninitialised field, which would make the library
 * scan or zero far past the buffer. */
static int size_plausible(uint32_t size, size_t known)
{
    return size >= known && size <= (uint32_t)KEEL_STRUCT_SIZE_MAX;
}

int32_t keel__in_check(const void *in, size_t known)
{
    const unsigned char *bytes = in;
    uint32_t size;
    size_t i;
    if (!in) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    size = struct_size_of(in);
    if (!size_plausible(size, known)) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    /* A newer caller may append fields; they are harmless only when zero. */
    for (i = known; i < size; ++i) {
        if (bytes[i] != 0) {
            return KEEL_E_UNSUPPORTED;
        }
    }
    return KEEL_OK;
}

int keel__out_ok(const void *out, size_t known)
{
    return out && size_plausible(struct_size_of(out), known);
}

void keel__out_write(void *out, const void *src, size_t known)
{
    unsigned char *bytes = out;
    size_t size = struct_size_of(out);
    size_t copy = size < known ? size : known;
    if (copy > sizeof(uint32_t)) {
        memcpy(bytes + sizeof(uint32_t), (const unsigned char *)src + sizeof(uint32_t),
               copy - sizeof(uint32_t));
    }
    if (size > known) {
        memset(bytes + known, 0, size - known);
    }
}
