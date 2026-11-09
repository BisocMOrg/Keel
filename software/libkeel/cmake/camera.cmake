# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# libkeel-camera (SPEC section 26): the camera library libkeel-camera.so.1
# (Keel::keel_camera), its keel-camera tool and its tests. It is separate
# from libkeel.so, which gains no camera code and no dependency.
#
#   KEEL_CAMERA             AUTO (default): built when libcamera and a C++17
#                           compiler are found; ON: required; OFF: skipped
#   KEEL_CAMERA_JPEG        AUTO, ON or OFF: libjpeg for stills larger than
#                           the hardware JPEG encoder's 1920x1920
#   KEEL_CAMERA_LIBAV       AUTO, ON or OFF: FFmpeg's libavformat for .mp4,
#                           .mkv, .mov and .ts files and rtsp, rtmp and srt
#   KEEL_CAMERA_TEST_HOOKS  OFF by default, ON in the presets: compiles the
#                           synthetic camera and encoder that
#                           KEEL_CAMERA_TEST_SOURCE=1 selects (tests only;
#                           packages and install.sh never set it)
#
# The version has a single source: the KEEL_CAMERA_VERSION_* macros of
# include/keel/camera.h. The exports are exactly tests/abi/camera_symbols.txt
# (cmake/keel-camera.map). Sets KEEL_HAVE_CAMERA when the library is built.
#
# SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

set(KEEL_CAMERA "AUTO" CACHE STRING "Build libkeel-camera: AUTO (when libcamera is found), ON or OFF")
set_property(CACHE KEEL_CAMERA PROPERTY STRINGS AUTO ON OFF)
set(KEEL_CAMERA_JPEG "AUTO" CACHE STRING "libjpeg for large stills: AUTO, ON or OFF")
set_property(CACHE KEEL_CAMERA_JPEG PROPERTY STRINGS AUTO ON OFF)
set(KEEL_CAMERA_LIBAV "AUTO" CACHE STRING "libavformat outputs (containers, rtsp, rtmp, srt): AUTO, ON or OFF")
set_property(CACHE KEEL_CAMERA_LIBAV PROPERTY STRINGS AUTO ON OFF)
option(KEEL_CAMERA_TEST_HOOKS "Compile the synthetic camera and encoder for tests (never in packages)" OFF)

set(KEEL_HAVE_CAMERA OFF)
foreach(_keel_option KEEL_CAMERA KEEL_CAMERA_JPEG KEEL_CAMERA_LIBAV)
    if(NOT ${_keel_option} MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR "${_keel_option} must be AUTO, ON or OFF, not '${${_keel_option}}'")
    endif()
endforeach()
if(KEEL_CAMERA STREQUAL "OFF")
    return()
endif()

# Reports a missing dependency (the message parts are joined): fatal when the
# option is ON.
function(keel_camera_missing option)
    string(JOIN "" why ${ARGN})
    if(${option} STREQUAL "ON")
        message(FATAL_ERROR "${option}=ON, but ${why}")
    endif()
    message(STATUS "${option}: ${why}")
endfunction()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    keel_camera_missing(KEEL_CAMERA "libkeel-camera needs a Linux target: skipped")
    return()
endif()
include(CheckLanguage)
check_language(CXX)
if(NOT CMAKE_CXX_COMPILER)
    keel_camera_missing(KEEL_CAMERA "no C++ compiler (libcamera's API is C++): libkeel-camera skipped")
    return()
endif()
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(KEEL_LIBCAMERA QUIET libcamera)
endif()
if(NOT KEEL_LIBCAMERA_FOUND)
    keel_camera_missing(KEEL_CAMERA "libcamera was not found (pkg-config libcamera; install libcamera-dev): "
                        "libkeel-camera skipped")
    return()
endif()
enable_language(CXX)

set(KEEL_CAMERA_HAVE_JPEG OFF)
if(NOT KEEL_CAMERA_JPEG STREQUAL "OFF")
    pkg_check_modules(KEEL_LIBJPEG QUIET libjpeg)
    if(KEEL_LIBJPEG_FOUND)
        set(KEEL_CAMERA_HAVE_JPEG ON)
    else()
        keel_camera_missing(KEEL_CAMERA_JPEG "libjpeg was not found (install libjpeg-dev): stills above "
                            "1920x1920 are KEEL_E_UNSUPPORTED")
    endif()
endif()
set(KEEL_CAMERA_HAVE_LIBAV OFF)
if(NOT KEEL_CAMERA_LIBAV STREQUAL "OFF")
    pkg_check_modules(KEEL_LIBAV QUIET libavformat libavcodec libavutil)
    if(KEEL_LIBAV_FOUND)
        set(KEEL_CAMERA_HAVE_LIBAV ON)
    else()
        keel_camera_missing(KEEL_CAMERA_LIBAV "libavformat was not found (install libavformat-dev): "
                            "container and protocol outputs are KEEL_E_UNSUPPORTED")
    endif()
endif()
message(STATUS "libkeel-camera: libcamera ${KEEL_LIBCAMERA_VERSION}, jpeg ${KEEL_CAMERA_HAVE_JPEG}, "
               "libav ${KEEL_CAMERA_HAVE_LIBAV}, test hooks ${KEEL_CAMERA_TEST_HOOKS}")

file(STRINGS "${PROJECT_SOURCE_DIR}/include/keel/camera.h" _keel_camera_defines
     REGEX "^#define KEEL_CAMERA_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(_keel_line IN LISTS _keel_camera_defines)
    if(_keel_line MATCHES "^#define KEEL_CAMERA_VERSION_([A-Z]+) ([0-9]+)$")
        set(_keel_camera_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()
set(KEEL_CAMERA_VERSION "${_keel_camera_MAJOR}.${_keel_camera_MINOR}.${_keel_camera_PATCH}")
if(NOT KEEL_CAMERA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Could not parse KEEL_CAMERA_VERSION_* from include/keel/camera.h")
endif()

set(KEEL_CAMERA_SOURCES
    src/camera/camera_api.c
    src/camera/camera_session.c
    src/camera/capture_libcamera.cpp
    src/camera/common.c
    src/camera/convert.c
    src/camera/destination.c
    src/camera/diagnose.c
    src/camera/encoder_v4l2.c
    src/camera/h264_colour.c
    src/camera/jpeg_software.c
    src/camera/output.c
    src/camera/output_libav.c
    src/camera/pipeline.c)
if(KEEL_CAMERA_TEST_HOOKS)
    list(APPEND KEEL_CAMERA_SOURCES src/camera/capture_synthetic.c src/camera/encoder_synthetic.c)
endif()

set(KEEL_CAMERA_VERSION_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/keel-camera.map")
add_library(keel_camera SHARED ${KEEL_CAMERA_SOURCES})
add_library(Keel::keel_camera ALIAS keel_camera)
set_target_properties(keel_camera PROPERTIES
    OUTPUT_NAME keel-camera
    VERSION ${KEEL_CAMERA_VERSION}
    SOVERSION ${_keel_camera_MAJOR}
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    LINK_DEPENDS "${KEEL_CAMERA_VERSION_SCRIPT}")
target_include_directories(keel_camera
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
# libcamera's headers are system headers: the strict warnings apply to this
# project's code only.
target_include_directories(keel_camera SYSTEM PRIVATE ${KEEL_LIBCAMERA_INCLUDE_DIRS})
target_compile_options(keel_camera PRIVATE ${KEEL_LIBCAMERA_CFLAGS_OTHER})
target_compile_definitions(keel_camera PRIVATE "KEEL_CAMERA_BUILD_TRIPLE=\"${KEEL_BUILD_TRIPLE}\""
    $<$<BOOL:${KEEL_CAMERA_TEST_HOOKS}>:KEEL_CAMERA_TEST_HOOKS>
    $<$<BOOL:${KEEL_CAMERA_HAVE_JPEG}>:KEEL_CAMERA_HAVE_JPEG>
    $<$<BOOL:${KEEL_CAMERA_HAVE_LIBAV}>:KEEL_CAMERA_HAVE_LIBAV>)
target_link_options(keel_camera PRIVATE "-Wl,--version-script=${KEEL_CAMERA_VERSION_SCRIPT}" "-Wl,--no-undefined")
target_link_directories(keel_camera PRIVATE ${KEEL_LIBCAMERA_LIBRARY_DIRS})
target_link_libraries(keel_camera PRIVATE ${KEEL_LIBCAMERA_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS}
    "$<BUILD_INTERFACE:keel_build_options>")
if(KEEL_CAMERA_HAVE_JPEG)
    target_include_directories(keel_camera SYSTEM PRIVATE ${KEEL_LIBJPEG_INCLUDE_DIRS})
    target_link_directories(keel_camera PRIVATE ${KEEL_LIBJPEG_LIBRARY_DIRS})
    target_link_libraries(keel_camera PRIVATE ${KEEL_LIBJPEG_LIBRARIES})
endif()
if(KEEL_CAMERA_HAVE_LIBAV)
    target_include_directories(keel_camera SYSTEM PRIVATE ${KEEL_LIBAV_INCLUDE_DIRS})
    target_link_directories(keel_camera PRIVATE ${KEEL_LIBAV_LIBRARY_DIRS})
    target_link_libraries(keel_camera PRIVATE ${KEEL_LIBAV_LIBRARIES})
endif()
set(KEEL_HAVE_CAMERA ON)

# Installation: the library, camera.h, keel-camera.pc and the CMake target
# Keel::keel_camera (find_package(Keel COMPONENTS camera)). The target has an
# export file of its own, KeelCameraTargets.cmake, which KeelConfig.cmake
# includes when it is installed: the board's development files (libkeel-dev)
# then work without the camera's (libkeel-camera-dev).
install(TARGETS keel_camera EXPORT KeelCameraTargets LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(EXPORT KeelCameraTargets NAMESPACE Keel:: DESTINATION "${KEEL_CMAKE_INSTALL_DIR}")
install(FILES include/keel/camera.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/keel")
configure_file(cmake/keel-camera.pc.in "${PROJECT_BINARY_DIR}/keel-camera.pc" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/keel-camera.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# keel-camera, the command-line tool (keel-camera package). It links only
# libkeel-camera.
if(KEEL_TOOLS)
    add_executable(keel-camera tools/keel-camera.c)
    target_link_libraries(keel-camera PRIVATE keel_camera keel_build_options Threads::Threads)
    install(TARGETS keel-camera RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
