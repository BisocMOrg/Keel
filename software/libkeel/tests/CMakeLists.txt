# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# libkeel test suite (SPEC section 23.1). Every case is registered as its own
# CTest test named <program>.<case>, with a layer label and a 60 s timeout.
# Cross builds without an emulator build the programs but register nothing.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

set(KEEL_RUN_TESTS ON)
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(KEEL_RUN_TESTS OFF)
endif()

# keel_add_cases(<target> <prefix> <label> <case>...)
function(keel_add_cases target prefix label)
    if(NOT KEEL_RUN_TESTS)
        return()
    endif()
    foreach(test_case IN LISTS ARGN)
        add_test(NAME ${prefix}.${test_case} COMMAND ${target} ${test_case})
        set_tests_properties(${prefix}.${test_case} PROPERTIES LABELS ${label} TIMEOUT 60)
    endforeach()
endfunction()

set(KEEL_TEST_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/common")

# ---- unit: the shared PWM policy ---------------------------------------------
if(KEEL_BACKEND_SOURCES)
    add_executable(test_pwm_policy backend/test_pwm_policy.c)
    target_include_directories(test_pwm_policy PRIVATE "${KEEL_TEST_COMMON}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(test_pwm_policy PRIVATE keel_static keel_build_options)
    keel_add_cases(test_pwm_policy pwm_policy unit pattern late_wakes_tolerated overrun degraded)
endif()

if(KEEL_HAVE_BACKEND_SIM)
    # ---- reference model: the core against the production MCU policy ----------
    set(KEEL_FIXTURE_INCLUDES "${KEEL_TEST_COMMON}" "${CMAKE_CURRENT_SOURCE_DIR}/core"
        "${PROJECT_SOURCE_DIR}/src")
    add_executable(test_reference core/test_reference.c core/fixture.c)
    target_include_directories(test_reference PRIVATE ${KEEL_FIXTURE_INCLUDES})
    target_link_libraries(test_reference PRIVATE keel_static keel_build_options)
    keel_add_cases(test_reference reference reference
        schedule_sweep stale_ack invalid_schedules clock_and_cycle_id early_call_long_gap
        health_and_deadlines feedback_and_fault motion_and_reversal backend_latch_priority
        buttons blocking_callbacks leds failures fresh_safety_diagnostics command_expiry
        disarmed_restart emergency_stop_and_holdoff brake armed_unexpected)

    # ---- servo bursts and ranging against the MCU policy ------------------------
    add_executable(test_servo_range core/test_servo_range.c core/fixture.c)
    target_include_directories(test_servo_range PRIVATE ${KEEL_FIXTURE_INCLUDES})
    target_link_libraries(test_servo_range PRIVATE keel_static keel_build_options)
    keel_add_cases(test_servo_range servo_range reference
        servo_burst servo_sweep servo_cooldown_after_reset servo_stops)
    keel_add_cases(test_servo_range servo_range unit
        servo_preconditions servo_feedback range_statuses range_rules range_owner_rule)

    # ---- error propagation: every hook x every status x every entry point ------
    add_executable(test_errors integration/test_errors.c core/fixture.c)
    target_include_directories(test_errors PRIVATE ${KEEL_FIXTURE_INCLUDES})
    target_link_libraries(test_errors PRIVATE keel_static keel_build_options)
    keel_add_cases(test_errors errors errors
        tick cycle request_arm set_motors set_motors_zero set_motors_invalid brake disarm
        emergency_stop clear_fault take_buttons read_safety_inputs set_button_leds servo_start
        servo_set_pulse servo_end servo_grant_tick sleep_until range_measure close peripheral_hooks
        fault_latch_hooks hal_contract_again overrun_between_ticks)

    # ---- random walk with per-millisecond invariants ----------------------------
    add_executable(test_walk integration/test_walk.c core/fixture.c)
    target_include_directories(test_walk PRIVATE ${KEEL_FIXTURE_INCLUDES})
    target_link_libraries(test_walk PRIVATE keel_static keel_build_options)
    keel_add_cases(test_walk walk walk seed_a seed_b seed_c)

    # ---- threads: stop epoch, ownership, close races, disarm latency -----------
    add_executable(test_threads integration/test_threads.c)
    target_include_directories(test_threads PRIVATE "${KEEL_TEST_COMMON}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(test_threads PRIVATE keel_static keel_build_options Threads::Threads)
    keel_add_cases(test_threads threads threads
        stop_vs_cycle foreign_motion hammer_close disarm_latency close_bound)

    # ---- arbitration: every reachable cell, both thread columns (public API) ---
    # One case per table row; the row count is read from the table itself.
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/integration/arbitration_table.h" _keel_arb_rows
         REGEX "^    \\{\"")
    list(LENGTH _keel_arb_rows _keel_arb_count)
    math(EXPR _keel_arb_last "${_keel_arb_count} - 1")
    add_executable(test_arbitration integration/test_arbitration.c)
    target_include_directories(test_arbitration PRIVATE "${KEEL_TEST_COMMON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/integration")
    target_link_libraries(test_arbitration PRIVATE keel keel_build_options Threads::Threads)
    if(KEEL_RUN_TESTS)
        foreach(_keel_row RANGE 0 ${_keel_arb_last})
            add_test(NAME arbitration.row${_keel_row} COMMAND test_arbitration ${_keel_row})
            set_tests_properties(arbitration.row${_keel_row}
                PROPERTIES LABELS arbitration TIMEOUT 60)
        endforeach()
    endif()

    # ---- unit: the core on the single-threaded OS layer -------------------------
    set(_keel_os_none_sources ${KEEL_CORE_SOURCES} src/os/os_none.c ${KEEL_BACKEND_SOURCES})
    list(TRANSFORM _keel_os_none_sources PREPEND "${PROJECT_SOURCE_DIR}/")
    add_executable(test_os_none core/test_os_none.c core/fixture.c ${_keel_os_none_sources})
    target_include_directories(test_os_none PRIVATE ${KEEL_FIXTURE_INCLUDES}
        "${PROJECT_SOURCE_DIR}/include" "${KEEL_GENERATED_DIR}")
    target_compile_definitions(test_os_none PRIVATE KEEL_STATIC)
    target_link_libraries(test_os_none PRIVATE keel_build_options Threads::Threads)
    keel_add_cases(test_os_none os_none unit lifecycle)

    # ---- registry: handles, close races and fork (public API) -------------------
    add_executable(test_registry core/test_registry.c)
    target_include_directories(test_registry PRIVATE "${KEEL_TEST_COMMON}")
    target_link_libraries(test_registry PRIVATE keel keel_build_options Threads::Threads)
    keel_add_cases(test_registry registry unit stale_handle double_close forged_handles
        slot_exhaustion)
    keel_add_cases(test_registry registry threads close_vs_call fork_child)

    # ---- simulator: the integration suite through keel.h only -------------------
    add_executable(test_sim_api sim/test_sim_api.c)
    # keel_build_config.h only tells the test which backends are built in.
    target_include_directories(test_sim_api PRIVATE "${KEEL_TEST_COMMON}" "${KEEL_GENERATED_DIR}")
    target_link_libraries(test_sim_api PRIVATE keel keel_build_options Threads::Threads)
    keel_add_cases(test_sim_api sim_api simulator end_to_end_virtual end_to_end_realtime
        worker_overrun worker_lateness_tolerated worker_stall leds_and_buttons struct_rules
        owner_rules emergency_stop metadata servo_and_range stop_records_prior_state
        motion_stop_checks_latch range_fresh_ready)
    keel_add_cases(test_sim_api sim_api abi struct_handshake r3_dispatch)

    # Real-time cases measure wall-clock bounds; keep them off a busy machine.
    if(KEEL_RUN_TESTS)
        set_tests_properties(registry.close_vs_call registry.fork_child
            sim_api.end_to_end_realtime threads.stop_vs_cycle threads.foreign_motion
            threads.hammer_close threads.disarm_latency threads.close_bound
            PROPERTIES RUN_SERIAL TRUE)
    endif()

    # ---- consumers: C, C++, the README example, Python ctypes, the install ------
    add_subdirectory(consumers)
endif()

# ---- camera: libkeel-camera against its synthetic camera and encoder ----------
if(KEEL_HAVE_CAMERA)
    add_subdirectory(camera)
endif()

if(NOT KEEL_RUN_TESTS)
    return()
endif()

# ---- simulator: hash guard of the production MCU policy ------------------------
set(_keel_policy_dir "${PROJECT_SOURCE_DIR}/src/backend/sim/mcu")
set(_keel_hash_work "${CMAKE_CURRENT_BINARY_DIR}/mcu_hash_self_test")
file(MAKE_DIRECTORY "${_keel_hash_work}")
add_test(NAME mcu_policy.hash
    COMMAND "${CMAKE_COMMAND}" -DPOLICY_DIR=${_keel_policy_dir}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/sim/check_mcu_hash.cmake")
add_test(NAME mcu_policy.hash_rejects_change
    COMMAND "${CMAKE_COMMAND}" -DPOLICY_DIR=${_keel_policy_dir} -DSELF_TEST=ON
            -DWORK_DIR=${_keel_hash_work} -P "${CMAKE_CURRENT_SOURCE_DIR}/sim/check_mcu_hash.cmake")
set_tests_properties(mcu_policy.hash mcu_policy.hash_rejects_change
    PROPERTIES LABELS simulator TIMEOUT 60)

# ---- abi: header and export gates ----------------------------------------------
add_test(NAME abi.exports
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/abi/check_exports.sh" $<TARGET_FILE:keel>
            $<TARGET_FILE:keel_static> "${CMAKE_CURRENT_SOURCE_DIR}/abi/symbols.txt"
            "${PROJECT_SOURCE_DIR}/cmake/keel.map")
set_tests_properties(abi.exports PROPERTIES ENVIRONMENT "NM=${CMAKE_NM}")
add_test(NAME abi.headers COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/abi/check_headers.sh")
set_tests_properties(abi.exports abi.headers PROPERTIES LABELS abi TIMEOUT 60)
if(KEEL_HAVE_CAMERA)
    add_test(NAME abi.camera_exports
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/abi/check_exports.sh" $<TARGET_FILE:keel_camera> -
                "${CMAKE_CURRENT_SOURCE_DIR}/abi/camera_symbols.txt" "${PROJECT_SOURCE_DIR}/cmake/keel-camera.map")
    set_tests_properties(abi.camera_exports PROPERTIES LABELS abi TIMEOUT 60 ENVIRONMENT "NM=${CMAKE_NM}")
endif()

# ---- hygiene: licence lines, line endings, other parties' names ----------------
add_test(NAME hygiene.tree COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/hygiene/check_tree.sh")
set_tests_properties(hygiene.tree PROPERTIES LABELS hygiene TIMEOUT 60)

# ---- docs: the documents against the test table and keel.h ---------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# The documents carry the arbitration table and the status and cause tables
# exactly as the test table and keel.h define them.
add_test(NAME docs.arbitration_table
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/docs/check_arbitration_doc.sh"
            "${PROJECT_SOURCE_DIR}/docs/spec.md" "${PROJECT_SOURCE_DIR}/docs/safety.md")
add_test(NAME docs.tables
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/docs/check_doc_tables.py")
# Rules that exist only in prose: signal safety, the struct_size refinements.
add_test(NAME docs.rules
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/docs/check_doc_rules.py")
set_tests_properties(docs.arbitration_table docs.tables docs.rules PROPERTIES LABELS docs TIMEOUT 60)

# ---- packaging: metadata consistency, keel-setup and install.sh ---------------
add_test(NAME packaging.metadata
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/check_package_metadata.py")
add_test(NAME packaging.keel_setup
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test_keel_setup.py")
add_test(NAME packaging.install_script
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test_install_script.sh")
set_tests_properties(packaging.metadata packaging.keel_setup packaging.install_script
    PROPERTIES LABELS packaging TIMEOUT 60)
