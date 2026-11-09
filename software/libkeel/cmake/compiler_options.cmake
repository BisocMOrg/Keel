# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# keel_build_options: the compiler and linker settings shared by every
# first-party target (library objects, tests, tools; the C++ glue of
# libkeel-camera gets the flags that apply to C++). It is an INTERFACE
# target that is never installed or exported, so warnings, -Werror,
# sanitizers and hardening never become usage requirements of Keel::keel or
# Keel::keel_static.

add_library(keel_build_options INTERFACE)

target_compile_options(keel_build_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef
    "$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes;-Wmissing-prototypes>"
    $<$<BOOL:${KEEL_WERROR}>:-Werror>)

# Sanitizers come from the presets (asan-ubsan, tsan) only.
if(KEEL_TSAN AND KEEL_SANITIZE)
    message(FATAL_ERROR "KEEL_TSAN cannot be combined with KEEL_SANITIZE=${KEEL_SANITIZE}")
endif()
if(KEEL_SANITIZE)
    target_compile_options(keel_build_options INTERFACE
        -fsanitize=${KEEL_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(keel_build_options INTERFACE -fsanitize=${KEEL_SANITIZE})
endif()
if(KEEL_TSAN)
    target_compile_options(keel_build_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(keel_build_options INTERFACE -fsanitize=thread)
endif()

# Release hardening. Ubuntu's compiler predefines _FORTIFY_SOURCE, hence the
# -U before the -D (a plain redefinition would warn, and -Werror would fail).
set(_keel_optimized "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
target_compile_options(keel_build_options INTERFACE
    "$<${_keel_optimized}:-fstack-protector-strong;-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3>")
target_link_options(keel_build_options INTERFACE "$<${_keel_optimized}:-Wl,-z,relro,-z,now>")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_options(keel_build_options INTERFACE
        "$<${_keel_optimized}:-mbranch-protection=standard>")
endif()
