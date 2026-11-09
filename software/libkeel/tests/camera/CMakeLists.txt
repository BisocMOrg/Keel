# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# libkeel-camera tests (SPEC section 26.9, CTest label "camera"). Every case
# runs against the synthetic camera and encoder, which only a build with
# KEEL_CAMERA_TEST_HOOKS=ON contains (the presets set it), selected through
# KEEL_CAMERA_TEST_SOURCE=1. The real libcamera and V4L2 paths are checked
# on the robot (docs/spec.md section 26.12 lists those checks). Every case is
# its own CTest test, camera.<program>.<case>, with a 60 s timeout.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

if(NOT KEEL_CAMERA_TEST_HOOKS)
    message(STATUS "camera tests skipped: they need KEEL_CAMERA_TEST_HOOKS=ON (the CMake presets set it)")
    return()
endif()

add_library(keel_camera_test_support STATIC camera_test.c)
target_include_directories(keel_camera_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${KEEL_TEST_COMMON}")
target_link_libraries(keel_camera_test_support PUBLIC keel_camera Threads::Threads PRIVATE keel_build_options)

set(_keel_camera_programs api capture outputs still lifecycle)
set(_keel_camera_cases_api metadata listing list_in_use list_faults open_defaults open_sizes options_validation
    struct_rules autofocus_rules open_errors busy handles thread_rule close_from_other_thread fork_child convert)
set(_keel_camera_cases_capture frames_formats frames_flips frames_order frames_drop_oldest read_states
    read_timeout capture_failure encoded_h264 encoded_inline_headers encoded_mjpeg encoded_drop_to_keyframe
    encoded_buffer_too_small encoder_options encoder_missing encoder_failure frames_with_encoder)
set(_keel_camera_cases_outputs file_h264 file_mjpeg tcp_client tcp_refused tcp_listen_clients listen_port_in_use
    udp join_running refusals output_failure libav_files frames_while_streaming)
set(_keel_camera_cases_still still_full_resolution still_hardware still_file still_options_rules
    still_encoder_missing)
set(_keel_camera_cases_lifecycle reopen_cycles cloexec signals close_running_with_outputs)

foreach(_keel_program IN LISTS _keel_camera_programs)
    add_executable(test_camera_${_keel_program} test_camera_${_keel_program}.c)
    target_link_libraries(test_camera_${_keel_program} PRIVATE keel_camera_test_support keel_build_options)
endforeach()

# White-box units: compiled from the library's own sources.
set(_keel_camera_unit_sources common.c convert.c destination.c diagnose.c h264_colour.c pipeline.c)
list(TRANSFORM _keel_camera_unit_sources PREPEND "${PROJECT_SOURCE_DIR}/src/camera/")
add_executable(test_camera_units test_camera_units.c ${_keel_camera_unit_sources})
target_include_directories(test_camera_units PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include"
    "${KEEL_TEST_COMMON}")
target_link_libraries(test_camera_units PRIVATE keel_build_options Threads::Threads)

if(NOT KEEL_RUN_TESTS)
    return()
endif()

set(_keel_camera_env "KEEL_CAMERA_TEST_SOURCE=1")
foreach(_keel_program IN LISTS _keel_camera_programs)
    foreach(_keel_case IN LISTS _keel_camera_cases_${_keel_program})
        add_test(NAME camera.${_keel_program}.${_keel_case} COMMAND test_camera_${_keel_program} ${_keel_case})
        set_tests_properties(camera.${_keel_program}.${_keel_case} PROPERTIES LABELS camera TIMEOUT 60
            ENVIRONMENT "${_keel_camera_env}")
    endforeach()
endforeach()
foreach(_keel_case IN ITEMS destinations h264_colour_hardware h264_colour_without_vui h264_scan packet_queues
        frame_queue coded_copy layout_and_names diagnose)
    add_test(NAME camera.units.${_keel_case} COMMAND test_camera_units ${_keel_case})
    set_tests_properties(camera.units.${_keel_case} PROPERTIES LABELS "camera;unit" TIMEOUT 60)
endforeach()

# keel-camera, the tool.
if(TARGET keel-camera)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(_keel_case IN ITEMS usage list still record stream_tcp stream_udp stream_stdout frames interrupt
            failures)
        add_test(NAME camera.tool.${_keel_case}
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_camera_tool.sh" $<TARGET_FILE:keel-camera> ${_keel_case})
        set_tests_properties(camera.tool.${_keel_case} PROPERTIES LABELS "camera;tools" TIMEOUT 60
            ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
    endforeach()
endif()

# Cases that measure time or count every thread and descriptor of the
# process run alone.
set_tests_properties(camera.api.close_from_other_thread camera.capture.frames_drop_oldest
    camera.capture.encoded_drop_to_keyframe camera.lifecycle.reopen_cycles camera.lifecycle.signals
    PROPERTIES RUN_SERIAL TRUE)
