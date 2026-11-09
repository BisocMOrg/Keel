# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Hash guard of the production MCU policy copy (SPEC 18.2). Everything after
# the provenance marker of src/backend/sim/mcu/safety_core.{c,h} must be the
# original firmware file, byte for byte. A policy change is therefore a
# deliberate, reviewed event: update the copy and these hashes together.
#
# Usage: cmake -DPOLICY_DIR=<src/backend/sim/mcu> [-DSELF_TEST=ON -DWORK_DIR=<dir>]
#              -P check_mcu_hash.cmake
# With SELF_TEST the guard must reject a copy with one changed byte.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
cmake_minimum_required(VERSION 3.21)

set(KEEL_MCU_MARKER "/* keel-provenance-end */\n")
set(KEEL_MCU_FILES
    "safety_core.c=da9cc0d8b9e2a4735e0751860fd2501c2b898fecef27278977b7cbd9efba48b2"
    "safety_core.h=3a7c36f0c6e449968a9308381dd0bc14f57e670a1501613b834a69dd4e5b3985")

# Sets <result> to OK, or to a message describing the mismatch.
function(keel_check_policy_file path expected result)
    file(READ "${path}" content)
    string(FIND "${content}" "${KEEL_MCU_MARKER}" marker_at)
    if(marker_at EQUAL -1)
        set(${result} "${path}: provenance marker not found" PARENT_SCOPE)
        return()
    endif()
    string(LENGTH "${KEEL_MCU_MARKER}" marker_length)
    math(EXPR original_at "${marker_at} + ${marker_length}")
    string(SUBSTRING "${content}" ${original_at} -1 original)
    string(SHA256 actual "${original}")
    if(NOT actual STREQUAL expected)
        set(${result} "${path}: SHA-256 ${actual}, expected ${expected}" PARENT_SCOPE)
    else()
        set(${result} OK PARENT_SCOPE)
    endif()
endfunction()

if(NOT POLICY_DIR)
    message(FATAL_ERROR "POLICY_DIR is not set")
endif()

foreach(entry IN LISTS KEEL_MCU_FILES)
    string(REPLACE "=" ";" pair "${entry}")
    list(GET pair 0 name)
    list(GET pair 1 expected)
    if(SELF_TEST)
        # Change one byte of the original part and require a rejection.
        file(READ "${POLICY_DIR}/${name}" content)
        string(REPLACE "safety" "Safety" tampered "${content}")
        file(WRITE "${WORK_DIR}/${name}" "${tampered}")
        keel_check_policy_file("${WORK_DIR}/${name}" "${expected}" result)
        if(result STREQUAL "OK")
            message(FATAL_ERROR "the hash guard accepted a modified ${name}")
        endif()
        message(STATUS "rejected modified ${name}: ${result}")
    else()
        keel_check_policy_file("${POLICY_DIR}/${name}" "${expected}" result)
        if(NOT result STREQUAL "OK")
            message(FATAL_ERROR "${result}")
        endif()
        message(STATUS "${name}: unmodified production policy")
    endif()
endforeach()
