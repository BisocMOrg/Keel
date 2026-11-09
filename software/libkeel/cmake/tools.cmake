# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# keel-live, keel-demo and keel-doctor (SPEC 22, 23.1): C programs that link
# only the public shared library, built when KEEL_TOOLS is ON and installed
# to the bin directory (the keel-tools package). keel-setup is a script and
# is not built here.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

# keel-doctor reads the GPIO character device; the tools serve Linux robots.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "keel tools skipped: they need a Linux target")
    return()
endif()

set(KEEL_TOOL_NAMES keel-live keel-demo keel-doctor)

add_library(keel_tool_common STATIC tools/tool_common.c)
target_include_directories(keel_tool_common PUBLIC "${PROJECT_SOURCE_DIR}/tools")
target_link_libraries(keel_tool_common PUBLIC keel PRIVATE keel_build_options)

foreach(_keel_tool IN LISTS KEEL_TOOL_NAMES)
    add_executable(${_keel_tool} tools/${_keel_tool}.c)
    target_link_libraries(${_keel_tool} PRIVATE keel_tool_common keel keel_build_options
                          Threads::Threads)
endforeach()

install(TARGETS ${KEEL_TOOL_NAMES} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# The tool checks run the tools against the simulator and keel-doctor
# against fake system trees; like the backend tests they are defined once
# the top-level directory is complete.
if(KEEL_BUILD_TESTS)
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}"
        CALL include "${PROJECT_SOURCE_DIR}/tests/tools/tool_tests.cmake")
endif()
