# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Consumers of the public library (SPEC section 23.1, label "consumers"):
#   consumer.c        C99 program that sees only keel.h and libkeel.so
#   consumer.cxx      C++17 program: RAII handle, exceptions by class, a second thread
#   consumer.readme   the quick-start example of README.md, compiled and run
#   ctypes.<case>     Python ctypes reference binding (also label "abi"); ctypes.scenarios
#                     replays the conformance scenarios of tests/bindings/scenarios.json
#   consumer.camera_c   C99 program that sees only camera.h and libkeel-camera.so
#   consumer.camera_cxx C++17 program: RAII handle, exceptions with the library's
#                       message, a close from a second thread
#   consumer.install  the staged install via pkg-config, find_package(Keel) and ctypes
#                     (and the camera library's keel-camera.pc and Keel::keel_camera)
# Sanitized builds run only the compiled consumers: a sanitized libkeel.so
# needs its runtime loaded first, which neither Python nor a separately
# configured project provides.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

set(KEEL_CONSUMER_SANITIZE "")
if(KEEL_SANITIZE)
    set(KEEL_CONSUMER_SANITIZE "-fsanitize=${KEEL_SANITIZE}")
elseif(KEEL_TSAN)
    set(KEEL_CONSUMER_SANITIZE "-fsanitize=thread")
endif()
set(KEEL_CONSUMER_SANITIZED OFF)
if(KEEL_CONSUMER_SANITIZE)
    set(KEEL_CONSUMER_SANITIZED ON)
endif()

add_executable(c_consumer c_consumer.c)
set_target_properties(c_consumer PROPERTIES C_STANDARD 99)
target_link_libraries(c_consumer PRIVATE keel keel_build_options)

# The README's quick start, regenerated whenever the README changes.
set(_keel_readme_example "${CMAKE_CURRENT_BINARY_DIR}/readme_example.c")
add_custom_command(OUTPUT "${_keel_readme_example}"
    COMMAND "${CMAKE_COMMAND}" -DDOCUMENT=${PROJECT_SOURCE_DIR}/README.md
            -DOUTPUT=${_keel_readme_example} -P "${CMAKE_CURRENT_SOURCE_DIR}/extract_example.cmake"
    DEPENDS "${PROJECT_SOURCE_DIR}/README.md" "${CMAKE_CURRENT_SOURCE_DIR}/extract_example.cmake"
    COMMENT "Extracting the README quick-start example")
add_executable(readme_example "${_keel_readme_example}")
target_link_libraries(readme_example PRIVATE keel keel_build_options)

# The camera library's C consumer: no libkeel.so involved.
if(KEEL_HAVE_CAMERA)
    add_executable(camera_c_consumer camera_c_consumer.c)
    set_target_properties(camera_c_consumer PROPERTIES C_STANDARD 99)
    target_link_libraries(camera_c_consumer PRIVATE keel_camera keel_build_options)
endif()

# C++ is optional: a cross toolchain file may name a C++ compiler that is not
# installed. keel_build_options carries C-only warning flags, so the C++
# consumer states its own.
set(KEEL_CONSUMER_CXX OFF)
if(DEFINED CMAKE_CXX_COMPILER)
    find_program(KEEL_CONSUMER_CXX_PATH "${CMAKE_CXX_COMPILER}")
    if(KEEL_CONSUMER_CXX_PATH)
        set(KEEL_CONSUMER_CXX ON)
    endif()
else()
    include(CheckLanguage)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
        set(KEEL_CONSUMER_CXX ON)
    endif()
endif()
if(KEEL_CONSUMER_CXX)
    enable_language(CXX)
    add_executable(cxx_consumer cxx_consumer.cpp)
    set_target_properties(cxx_consumer PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(cxx_consumer PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wold-style-cast
        $<$<BOOL:${KEEL_WERROR}>:-Werror> ${KEEL_CONSUMER_SANITIZE})
    target_link_options(cxx_consumer PRIVATE ${KEEL_CONSUMER_SANITIZE})
    target_link_libraries(cxx_consumer PRIVATE keel Threads::Threads)
    if(KEEL_HAVE_CAMERA)
        add_executable(camera_cxx_consumer camera_cxx_consumer.cpp)
        set_target_properties(camera_cxx_consumer PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_compile_options(camera_cxx_consumer PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wold-style-cast
            $<$<BOOL:${KEEL_WERROR}>:-Werror> ${KEEL_CONSUMER_SANITIZE})
        target_link_options(camera_cxx_consumer PRIVATE ${KEEL_CONSUMER_SANITIZE})
        target_link_libraries(camera_cxx_consumer PRIVATE keel_camera Threads::Threads)
    endif()
endif()

if(NOT KEEL_RUN_TESTS)
    return()
endif()

add_test(NAME consumer.c COMMAND c_consumer)
# The example opens with defaults (KEEL_BACKEND_AUTO), so the environment
# selects the simulator; it runs on the real-time clock.
add_test(NAME consumer.readme COMMAND readme_example)
set_tests_properties(consumer.c consumer.readme PROPERTIES LABELS consumers TIMEOUT 60)
set_tests_properties(consumer.readme PROPERTIES ENVIRONMENT "KEEL_BACKEND=simulator" RUN_SERIAL TRUE)
# Against the synthetic camera, which a test-hooks build contains.
if(TARGET camera_c_consumer AND KEEL_CAMERA_TEST_HOOKS)
    add_test(NAME consumer.camera_c COMMAND camera_c_consumer)
    set_tests_properties(consumer.camera_c PROPERTIES LABELS "consumers;camera" TIMEOUT 60
        ENVIRONMENT "KEEL_CAMERA_TEST_SOURCE=1")
    if(TARGET camera_cxx_consumer)
        add_test(NAME consumer.camera_cxx COMMAND camera_cxx_consumer)
        set_tests_properties(consumer.camera_cxx PROPERTIES LABELS "consumers;camera" TIMEOUT 60
            ENVIRONMENT "KEEL_CAMERA_TEST_SOURCE=1")
    endif()
endif()
if(TARGET cxx_consumer)
    add_test(NAME consumer.cxx COMMAND cxx_consumer)
    set_tests_properties(consumer.cxx PROPERTIES LABELS consumers TIMEOUT 60)
endif()

if(KEEL_CONSUMER_SANITIZED)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(_keel_case IN ITEMS metadata layout loop faults peripherals handles scenarios)
    add_test(NAME ctypes.${_keel_case}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ctypes.py" ${_keel_case})
    set_tests_properties(ctypes.${_keel_case} PROPERTIES LABELS "abi;consumers" TIMEOUT 60
        ENVIRONMENT "KEEL_LIBRARY=$<TARGET_FILE:keel>;CC=${CMAKE_C_COMPILER}")
endforeach()
add_test(NAME consumer.install
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_consumers.sh" staged "${PROJECT_BINARY_DIR}"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/install")
set(_keel_install_env "CMAKE=${CMAKE_COMMAND}" "CC=${CMAKE_C_COMPILER}" "PYTHON=${Python3_EXECUTABLE}")
if(KEEL_CAMERA_TEST_HOOKS)
    list(APPEND _keel_install_env "KEEL_CAMERA_TEST_SOURCE=1")
endif()
if(CMAKE_CXX_COMPILER)
    list(APPEND _keel_install_env "CXX=${CMAKE_CXX_COMPILER}")
endif()
set_tests_properties(consumer.install PROPERTIES LABELS consumers TIMEOUT 120
    ENVIRONMENT "${_keel_install_env}")
