# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Cross toolchain for linux-arm64 (the robot: Raspberry Pi 4, Ubuntu, glibc)
# using the distribution's aarch64-linux-gnu GCC and its sysroot. When
# qemu-aarch64 is installed it becomes the CTest emulator; without it the
# test programs are built but not registered (see tests/CMakeLists.txt).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

find_program(KEEL_QEMU_AARCH64 qemu-aarch64)
if(KEEL_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${KEEL_QEMU_AARCH64};-L;/usr/aarch64-linux-gnu")
endif()

# pkg-config finds the target's .pc files only (multiarch arm64 -dev
# packages), never the build machine's: libkeel-camera is built only when
# libcamera-dev:arm64 is installed.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
