# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# keel_r3 backend tests (SPEC 23.1, layer "backend"). cmake/backend_keel_r3.cmake
# includes this file at the end of the top-level directory (a deferred call),
# when testing is enabled and the library targets exist. Every case is its own
# CTest test with a 60 s timeout; cross builds without an emulator build the
# program but register nothing.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

set(_keel_backend_dir "${CMAKE_CURRENT_LIST_DIR}")
set(KEEL_BACKEND_RUN_TESTS ON)
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(KEEL_BACKEND_RUN_TESTS OFF)
endif()

# keel_backend_cases(<target> <prefix> <label> <case>...)
function(keel_backend_cases target prefix label)
    if(NOT KEEL_BACKEND_RUN_TESTS)
        return()
    endif()
    foreach(test_case IN LISTS ARGN)
        add_test(NAME ${prefix}.${test_case} COMMAND ${target} ${test_case})
        set_tests_properties(${prefix}.${test_case} PROPERTIES LABELS ${label} TIMEOUT 60)
    endforeach()
endfunction()

# The backend's sources are compiled into the test with their system calls
# renamed to the fake kernel; the core comes from the static archive, whose
# own keel_r3 members are then never pulled in.
add_executable(test_r3 "${_keel_backend_dir}/test_r3.c" "${_keel_backend_dir}/fake_kernel.c")
set_target_properties(test_r3 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests/backend")
target_include_directories(test_r3 PRIVATE
    "${_keel_backend_dir}" "${PROJECT_SOURCE_DIR}/tests/common" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(test_r3 PRIVATE keel_static keel_build_options Threads::Threads)
keel_backend_cases(test_r3 r3 backend
    requests never_requested chip_discovery errno_mapping line_in_use interlocks stop_epoch
    backend_latch realtime_fallback lateness_budget stall worker_faults i2c range)
keel_backend_cases(test_r3 r3 threads end_to_end fork_child stop_fairness)
if(KEEL_BACKEND_RUN_TESTS)
    # Wall-clock cases: keep them off a busy machine.
    set_tests_properties(r3.end_to_end r3.fork_child r3.stop_fairness PROPERTIES RUN_SERIAL TRUE)
endif()
