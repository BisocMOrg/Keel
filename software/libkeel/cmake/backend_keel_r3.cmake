# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# keel_r3 backend (R3 on a Raspberry Pi 4, SPEC section 17): GPIO
# character device v2, i2c-dev and the userspace PWM worker. Included by the
# top-level CMakeLists.txt when KEEL_BACKEND_KEEL_R3 is ON. It appends the
# backend's sources to KEEL_BACKEND_SOURCES, puts "r3" first in
# KEEL_BACKEND_NAMES and sets KEEL_HAVE_BACKEND_R3.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "KEEL_BACKEND_KEEL_R3 needs a Linux target; set it OFF for ${CMAKE_SYSTEM_NAME}")
endif()

# GPIO v2 line requests arrived in the Linux 5.10 UAPI headers.
include(CheckSymbolExists)
check_symbol_exists(GPIO_V2_GET_LINE_IOCTL "linux/gpio.h" KEEL_HAVE_GPIO_V2_UAPI)
if(NOT KEEL_HAVE_GPIO_V2_UAPI)
    message(FATAL_ERROR "keel_r3 needs the GPIO v2 UAPI (linux/gpio.h from Linux 5.10 or later)")
endif()

list(APPEND KEEL_BACKEND_SOURCES
    src/backend/keel_r3/r3_open.c
    src/backend/keel_r3/r3_gpio.c
    src/backend/keel_r3/r3_motor.c
    src/backend/keel_r3/r3_peripherals.c)
list(PREPEND KEEL_BACKEND_NAMES r3)
set(KEEL_HAVE_BACKEND_R3 ON)

# The backend's syscall-substitution tests live in tests/backend. They are
# defined once the top-level directory is complete, after tests/ has enabled
# testing and the tool targets exist, so this module needs no hook in
# tests/CMakeLists.txt. (A deferred call may not add a subdirectory, hence a
# module include.)
if(KEEL_BUILD_TESTS)
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}"
        CALL include "${PROJECT_SOURCE_DIR}/tests/backend/backend_tests.cmake")
endif()
