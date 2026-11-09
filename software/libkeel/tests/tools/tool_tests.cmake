# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Tool checks (SPEC 23.1): keel-demo and keel-live against the simulator,
# keel-doctor against fake system trees and, white-box, against the fake
# kernel. cmake/tools.cmake includes this file at the end of the top-level
# directory (a deferred call), after testing is enabled. Cross builds without
# an emulator build the programs but register nothing.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

set(_keel_tool_tests_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_keel_fake_kernel_dir "${PROJECT_SOURCE_DIR}/tests/backend")
set(KEEL_TOOL_RUN_TESTS ON)
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(KEEL_TOOL_RUN_TESTS OFF)
endif()

# keel-doctor compiled with the backend tests' fake kernel for its GPIO checks.
add_executable(test_doctor "${_keel_tool_tests_dir}/test_doctor.c"
                           "${_keel_fake_kernel_dir}/fake_kernel.c")
set_target_properties(test_doctor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests/tools")
target_include_directories(test_doctor PRIVATE
    "${_keel_fake_kernel_dir}" "${PROJECT_SOURCE_DIR}/tests/common" "${PROJECT_SOURCE_DIR}/tools")
target_link_libraries(test_doctor PRIVATE keel_tool_common keel_build_options Threads::Threads)

if(NOT KEEL_TOOL_RUN_TESTS)
    return()
endif()

foreach(_keel_case IN ITEMS free_lines used_lines chips)
    add_test(NAME doctor.${_keel_case} COMMAND test_doctor ${_keel_case})
    set_tests_properties(doctor.${_keel_case} PROPERTIES LABELS tools TIMEOUT 60)
endforeach()

set(_keel_tool_cases doctor_pi4_tree doctor_bad_tree doctor_user doctor_text live_preconditions
    live_usage demo_usage)
if(KEEL_HAVE_BACKEND_SIM)
    list(APPEND _keel_tool_cases demo_drive demo_duration live_io live_leds live_buttons
         live_safety live_motors live_range live_pwm_jitter)
endif()
foreach(_keel_case IN LISTS _keel_tool_cases)
    add_test(NAME tools.${_keel_case}
        COMMAND sh "${_keel_tool_tests_dir}/check_tools.sh" "$<TARGET_FILE_DIR:keel-live>"
                ${_keel_case})
    set_tests_properties(tools.${_keel_case} PROPERTIES LABELS tools TIMEOUT 60)
endforeach()
# The simulator runs follow real time: keep them off a busy machine.
if(KEEL_HAVE_BACKEND_SIM)
    set_tests_properties(tools.live_safety tools.live_buttons tools.live_pwm_jitter
        PROPERTIES RUN_SERIAL TRUE)
endif()
