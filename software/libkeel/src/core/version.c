/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/* Library identification: ABI version and build string. */
#include "core/internal.h"
#include "keel_build_config.h"

#define KEEL__STRINGIFY(x) #x
#define KEEL__TO_STRING(x) KEEL__STRINGIFY(x)

static const char version_string[] =
    KEEL__TO_STRING(KEEL_VERSION_MAJOR) "." KEEL__TO_STRING(KEEL_VERSION_MINOR) "." KEEL__TO_STRING(
        KEEL_VERSION_PATCH) " (" KEEL_BUILD_TRIPLE "; backends " KEEL_BUILD_BACKENDS ")";

_Static_assert(sizeof version_string <= KEEL_STRING_MAX, "version string exceeds KEEL_STRING_MAX");

KEEL_API uint32_t keel_abi_version(void)
{
    return KEEL_ABI_VERSION;
}

KEEL_API const char *keel_version_string(void)
{
    return version_string;
}
