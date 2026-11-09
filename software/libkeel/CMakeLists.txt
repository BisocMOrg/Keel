# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# libkeel: host library for the Keel robot baseboard.
#
# Targets:
#   keel         shared library libkeel.so.1 (exported as Keel::keel); exports
#                exactly the functions of tests/abi/symbols.txt (cmake/keel.map)
#   keel_static  static archive libkeel.a (Keel::keel_static, defines
#                KEEL_STATIC); also carries the C porting interface (port.h)
#   keel_camera  shared library libkeel-camera.so.1 (Keel::keel_camera), the
#                camera library, built when libcamera is found
#                (cmake/camera.cmake)
cmake_minimum_required(VERSION 3.21)

# The version has a single source: the KEEL_VERSION_* macros in keel.h.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/keel/keel.h" _keel_version_defines
     REGEX "^#define KEEL_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(_keel_line IN LISTS _keel_version_defines)
    if(_keel_line MATCHES "^#define KEEL_VERSION_([A-Z]+) ([0-9]+)$")
        set(_keel_version_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()
if(NOT DEFINED _keel_version_MAJOR OR NOT DEFINED _keel_version_MINOR
   OR NOT DEFINED _keel_version_PATCH)
    message(FATAL_ERROR "Could not parse KEEL_VERSION_* from include/keel/keel.h")
endif()

project(Keel
    VERSION ${_keel_version_MAJOR}.${_keel_version_MINOR}.${_keel_version_PATCH}
    DESCRIPTION "Host library for the Keel robot baseboard"
    LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options (test, tool and backend defaults depend on being the top-level
# project, so embedding libkeel with add_subdirectory builds only the library).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_keel_r3_default ON)
else()
    set(_keel_r3_default OFF)
endif()
option(KEEL_BACKEND_KEEL_R3 "Build the keel_r3 backend (R3 on a Raspberry Pi 4)"
       ${_keel_r3_default})
option(KEEL_BACKEND_SIM "Build the in-process simulator backend" ON)
set(KEEL_SANITIZE "" CACHE STRING
    "Comma-separated -fsanitize= list for first-party targets (set by presets only)")
option(KEEL_TSAN "Build first-party targets with ThreadSanitizer (set by presets only)" OFF)
option(KEEL_WERROR "Treat compiler warnings as errors" OFF)
option(KEEL_TOOLS "Build keel-live, keel-demo, keel-doctor and keel-setup" ${PROJECT_IS_TOP_LEVEL})
option(KEEL_PACKAGING "Configure the Debian packaging helpers" OFF)
option(KEEL_BUILD_TESTS "Build and register the test suite" ${PROJECT_IS_TOP_LEVEL})
set(KEEL_OS "posix" CACHE STRING "OS layer behind the handle registry: posix or none")
set_property(CACHE KEEL_OS PROPERTY STRINGS posix none)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include(compiler_options)

# ---------------------------------------------------------------------------
# Sources.
set(KEEL_CORE_SOURCES
    src/core/abi.c
    src/core/api.c
    src/core/attach.c
    src/core/buttons.c
    src/core/fault.c
    src/core/leds.c
    src/core/motion.c
    src/core/range.c
    src/core/registry.c
    src/core/safety.c
    src/core/servo.c
    src/core/snapshot.c
    src/core/version.c)

if(KEEL_OS STREQUAL "posix")
    set(KEEL_OS_SOURCES src/os/os_posix.c)
elseif(KEEL_OS STREQUAL "none")
    set(KEEL_OS_SOURCES src/os/os_none.c)
else()
    message(FATAL_ERROR "KEEL_OS must be posix or none, not '${KEEL_OS}'")
endif()

# Backends append their sources and names; a backend module sets
# KEEL_HAVE_BACKEND_<NAME> when it really contributes an implementation.
set(KEEL_BACKEND_SOURCES "")
set(KEEL_BACKEND_NAMES "")
set(KEEL_HAVE_BACKEND_R3 OFF)
set(KEEL_HAVE_BACKEND_SIM OFF)
if(KEEL_BACKEND_KEEL_R3)
    include(backend_keel_r3)
endif()
if(KEEL_BACKEND_SIM)
    list(APPEND KEEL_BACKEND_SOURCES
        src/backend/sim/sim_backend.c
        src/backend/sim/sim_board.c
        src/backend/sim/mcu/mcu_policy.c)
    list(APPEND KEEL_BACKEND_NAMES simulator)
    set(KEEL_HAVE_BACKEND_SIM ON)
endif()
if(KEEL_BACKEND_SOURCES)
    # The PWM pattern and lateness policy is shared by every motor backend.
    list(APPEND KEEL_BACKEND_SOURCES src/backend/pwm_policy.c)
    list(REMOVE_DUPLICATES KEEL_BACKEND_SOURCES)
endif()

# Build identification compiled into keel_version_string().
if(CMAKE_C_LIBRARY_ARCHITECTURE)
    set(KEEL_BUILD_TRIPLE "${CMAKE_C_LIBRARY_ARCHITECTURE}")
else()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                    OUTPUT_VARIABLE KEEL_BUILD_TRIPLE OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
if(NOT KEEL_BUILD_TRIPLE)
    set(KEEL_BUILD_TRIPLE "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}")
endif()
if(KEEL_BACKEND_NAMES)
    list(JOIN KEEL_BACKEND_NAMES "," KEEL_BUILD_BACKENDS)
else()
    set(KEEL_BUILD_BACKENDS "none")
endif()
set(KEEL_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file(src/core/build_config.h.in "${KEEL_GENERATED_DIR}/keel_build_config.h" @ONLY)

# ---------------------------------------------------------------------------
# Libraries. One position-independent object set feeds both libraries.
add_library(keel_objects OBJECT ${KEEL_CORE_SOURCES} ${KEEL_OS_SOURCES} ${KEEL_BACKEND_SOURCES})
set_target_properties(keel_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_include_directories(keel_objects
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src" "${KEEL_GENERATED_DIR}")
target_link_libraries(keel_objects PRIVATE keel_build_options Threads::Threads)

set(KEEL_VERSION_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/keel.map")
add_library(keel SHARED $<TARGET_OBJECTS:keel_objects>)
add_library(Keel::keel ALIAS keel)
set_target_properties(keel PROPERTIES
    OUTPUT_NAME keel
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
    LINK_DEPENDS "${KEEL_VERSION_SCRIPT}")
target_include_directories(keel PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_options(keel PRIVATE "-Wl,--version-script=${KEEL_VERSION_SCRIPT}" "-Wl,--no-undefined")
target_link_libraries(keel PRIVATE Threads::Threads "$<BUILD_INTERFACE:keel_build_options>")

add_library(keel_static STATIC $<TARGET_OBJECTS:keel_objects>)
add_library(Keel::keel_static ALIAS keel_static)
set_target_properties(keel_static PROPERTIES OUTPUT_NAME keel)
target_include_directories(keel_static PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(keel_static INTERFACE KEEL_STATIC)
target_link_libraries(keel_static PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Installation: headers, both libraries, the CMake package Keel, keel.pc and
# keel-static.pc.
set(KEEL_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/Keel")
install(TARGETS keel keel_static
    EXPORT KeelTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES include/keel/keel.h include/keel/port.h
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/keel")
install(EXPORT KeelTargets NAMESPACE Keel:: DESTINATION "${KEEL_CMAKE_INSTALL_DIR}")
configure_package_config_file(cmake/KeelConfig.cmake.in
    "${PROJECT_BINARY_DIR}/KeelConfig.cmake"
    INSTALL_DESTINATION "${KEEL_CMAKE_INSTALL_DIR}")
write_basic_package_version_file("${PROJECT_BINARY_DIR}/KeelConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES "${PROJECT_BINARY_DIR}/KeelConfig.cmake" "${PROJECT_BINARY_DIR}/KeelConfigVersion.cmake"
        DESTINATION "${KEEL_CMAKE_INSTALL_DIR}")
# keel.pc locates the prefix from its own directory, so the package works
# wherever it is installed (cmake --install --prefix, DESTDIR, packages).
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}" OR IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(KEEL_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
    set(KEEL_PC_LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}")
    set(KEEL_PC_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
else()
    file(RELATIVE_PATH _keel_pc_up "/keel/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/keel")
    string(REGEX REPLACE "/$" "" _keel_pc_up "${_keel_pc_up}")
    set(KEEL_PC_PREFIX "\${pcfiledir}/${_keel_pc_up}")
    set(KEEL_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
    set(KEEL_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
# keel-static.pc names libkeel.a by path: -lkeel would find the .so first.
configure_file(cmake/keel.pc.in "${PROJECT_BINARY_DIR}/keel.pc" @ONLY)
configure_file(cmake/keel-static.pc.in "${PROJECT_BINARY_DIR}/keel-static.pc" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/keel.pc" "${PROJECT_BINARY_DIR}/keel-static.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# ---------------------------------------------------------------------------
# The camera library libkeel-camera.so.1 (KEEL_CAMERA, SPEC section 26): a
# library of its own, so libkeel.so gains no camera code or dependency.
include(camera)

# ---------------------------------------------------------------------------
# Tools, packaging and tests.
if(KEEL_TOOLS)
    include(tools)
endif()
if(KEEL_PACKAGING)
    include(packaging)
endif()
if(KEEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
