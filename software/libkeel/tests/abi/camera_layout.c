/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Frozen struct layouts of the libkeel-camera 1.x ABI (camera.h).
 *
 * Compiled (never run) by tests/abi/check_headers.sh on the same targets as
 * layout.c: x86-64, i386, aarch64 LP64 and ILP32 where the toolchain supports
 * it, and as C++11. Every field's offset is pinned, and each field must start
 * exactly where the previous one ends, so no compiler-inserted padding can
 * exist on any target. The bindings read the FIRST/NEXT/LAST lines. Changing
 * a number here is an ABI break (new major version).
 */
#include <stddef.h>
#include <stdint.h>

#include "keel/camera.h"

#ifdef __cplusplus
#define KEEL_LAYOUT_ASSERT(cond, msg) static_assert(cond, msg)
#define KEEL_MEMBER_SIZE(T, m) sizeof(T::m)
#else
#define KEEL_LAYOUT_ASSERT(cond, msg) _Static_assert(cond, msg)
#define KEEL_MEMBER_SIZE(T, m) sizeof(((T *)0)->m)
#endif

#define KEEL_MEMBER_END(T, m) (offsetof(T, m) + KEEL_MEMBER_SIZE(T, m))
/* The first field sits at offset 0. */
#define FIRST(T, m) KEEL_LAYOUT_ASSERT(offsetof(T, m) == 0, #T "." #m " must be at offset 0");
/* A field sits at its frozen offset, directly after the previous field. */
#define NEXT(T, prev, m, off)                                                                      \
    KEEL_LAYOUT_ASSERT(offsetof(T, m) == (off), #T "." #m " offset changed");                    \
    KEEL_LAYOUT_ASSERT(KEEL_MEMBER_END(T, prev) == offsetof(T, m), #T ": padding before " #m);
/* The struct ends directly after its last field, at its frozen size. */
#define LAST(T, m, size)                                                                           \
    KEEL_LAYOUT_ASSERT(sizeof(T) == (size), "sizeof " #T " changed");                             \
    KEEL_LAYOUT_ASSERT(KEEL_MEMBER_END(T, m) == sizeof(T), #T ": tail padding after " #m);

FIRST(keel_camera_error, struct_size)
NEXT(keel_camera_error, struct_size, status, 4)
NEXT(keel_camera_error, status, resource, 8)
NEXT(keel_camera_error, resource, os_error, 12)
NEXT(keel_camera_error, os_error, path, 16)
NEXT(keel_camera_error, path, message, 272)
LAST(keel_camera_error, message, 528)

FIRST(keel_camera_info, struct_size)
NEXT(keel_camera_info, struct_size, index, 4)
NEXT(keel_camera_info, index, flags, 8)
NEXT(keel_camera_info, flags, mode_count, 12)
NEXT(keel_camera_info, mode_count, pixel_array_width, 16)
NEXT(keel_camera_info, pixel_array_width, pixel_array_height, 20)
NEXT(keel_camera_info, pixel_array_height, id, 24)
NEXT(keel_camera_info, id, model, 152)
NEXT(keel_camera_info, model, sensor, 216)
LAST(keel_camera_info, sensor, 248)

FIRST(keel_camera_mode, struct_size)
NEXT(keel_camera_mode, struct_size, width, 4)
NEXT(keel_camera_mode, width, height, 8)
NEXT(keel_camera_mode, height, bit_depth, 12)
NEXT(keel_camera_mode, bit_depth, max_framerate_milli, 16)
NEXT(keel_camera_mode, max_framerate_milli, crop_x, 20)
NEXT(keel_camera_mode, crop_x, crop_y, 24)
NEXT(keel_camera_mode, crop_y, crop_width, 28)
NEXT(keel_camera_mode, crop_width, crop_height, 32)
NEXT(keel_camera_mode, crop_height, reserved0, 36)
LAST(keel_camera_mode, reserved0, 40)

FIRST(keel_camera_options, struct_size)
NEXT(keel_camera_options, struct_size, index, 4)
NEXT(keel_camera_options, index, width, 8)
NEXT(keel_camera_options, width, height, 12)
NEXT(keel_camera_options, height, framerate_milli, 16)
NEXT(keel_camera_options, framerate_milli, format, 20)
NEXT(keel_camera_options, format, flags, 24)
NEXT(keel_camera_options, flags, rotation, 28)
NEXT(keel_camera_options, rotation, af_mode, 32)
NEXT(keel_camera_options, af_mode, af_range, 36)
NEXT(keel_camera_options, af_range, lens_position_milli, 40)
NEXT(keel_camera_options, lens_position_milli, exposure_us, 44)
NEXT(keel_camera_options, exposure_us, gain_milli, 48)
NEXT(keel_camera_options, gain_milli, ev_milli, 52)
NEXT(keel_camera_options, ev_milli, awb_mode, 56)
NEXT(keel_camera_options, awb_mode, red_gain_milli, 60)
NEXT(keel_camera_options, red_gain_milli, blue_gain_milli, 64)
NEXT(keel_camera_options, blue_gain_milli, frame_queue, 68)
LAST(keel_camera_options, frame_queue, 72)

FIRST(keel_camera_stream_info, struct_size)
NEXT(keel_camera_stream_info, struct_size, format, 4)
NEXT(keel_camera_stream_info, format, width, 8)
NEXT(keel_camera_stream_info, width, height, 12)
NEXT(keel_camera_stream_info, height, framerate_milli, 16)
NEXT(keel_camera_stream_info, framerate_milli, plane_count, 20)
NEXT(keel_camera_stream_info, plane_count, stride0, 24)
NEXT(keel_camera_stream_info, stride0, stride1, 28)
NEXT(keel_camera_stream_info, stride1, stride2, 32)
NEXT(keel_camera_stream_info, stride2, offset0, 36)
NEXT(keel_camera_stream_info, offset0, offset1, 40)
NEXT(keel_camera_stream_info, offset1, offset2, 44)
NEXT(keel_camera_stream_info, offset2, frame_bytes, 48)
NEXT(keel_camera_stream_info, frame_bytes, codec, 52)
NEXT(keel_camera_stream_info, codec, encoded_bytes_max, 56)
NEXT(keel_camera_stream_info, encoded_bytes_max, reserved0, 60)
LAST(keel_camera_stream_info, reserved0, 64)

FIRST(keel_camera_frame_info, struct_size)
NEXT(keel_camera_frame_info, struct_size, format, 4)
NEXT(keel_camera_frame_info, format, width, 8)
NEXT(keel_camera_frame_info, width, height, 12)
NEXT(keel_camera_frame_info, height, plane_count, 16)
NEXT(keel_camera_frame_info, plane_count, stride0, 20)
NEXT(keel_camera_frame_info, stride0, stride1, 24)
NEXT(keel_camera_frame_info, stride1, stride2, 28)
NEXT(keel_camera_frame_info, stride2, offset0, 32)
NEXT(keel_camera_frame_info, offset0, offset1, 36)
NEXT(keel_camera_frame_info, offset1, offset2, 40)
NEXT(keel_camera_frame_info, offset2, bytes, 44)
NEXT(keel_camera_frame_info, bytes, sequence, 48)
NEXT(keel_camera_frame_info, sequence, timestamp_ns, 56)
LAST(keel_camera_frame_info, timestamp_ns, 64)

FIRST(keel_camera_encoder_options, struct_size)
NEXT(keel_camera_encoder_options, struct_size, codec, 4)
NEXT(keel_camera_encoder_options, codec, bitrate, 8)
NEXT(keel_camera_encoder_options, bitrate, intra_period, 12)
NEXT(keel_camera_encoder_options, intra_period, profile, 16)
NEXT(keel_camera_encoder_options, profile, flags, 20)
NEXT(keel_camera_encoder_options, flags, quality, 24)
NEXT(keel_camera_encoder_options, quality, encoded_queue, 28)
LAST(keel_camera_encoder_options, encoded_queue, 32)

FIRST(keel_camera_encoded_info, struct_size)
NEXT(keel_camera_encoded_info, struct_size, codec, 4)
NEXT(keel_camera_encoded_info, codec, flags, 8)
NEXT(keel_camera_encoded_info, flags, bytes, 12)
NEXT(keel_camera_encoded_info, bytes, sequence, 16)
NEXT(keel_camera_encoded_info, sequence, timestamp_ns, 24)
LAST(keel_camera_encoded_info, timestamp_ns, 32)

FIRST(keel_camera_output_options, struct_size)
NEXT(keel_camera_output_options, struct_size, flags, 4)
NEXT(keel_camera_output_options, flags, queue, 8)
NEXT(keel_camera_output_options, queue, max_clients, 12)
NEXT(keel_camera_output_options, max_clients, datagram_bytes, 16)
NEXT(keel_camera_output_options, datagram_bytes, reserved0, 20)
LAST(keel_camera_output_options, reserved0, 24)

FIRST(keel_camera_output_stats, struct_size)
NEXT(keel_camera_output_stats, struct_size, state, 4)
NEXT(keel_camera_output_stats, state, status, 8)
NEXT(keel_camera_output_stats, status, os_error, 12)
NEXT(keel_camera_output_stats, os_error, clients, 16)
NEXT(keel_camera_output_stats, clients, reserved0, 20)
NEXT(keel_camera_output_stats, reserved0, bytes, 24)
NEXT(keel_camera_output_stats, bytes, chunks, 32)
NEXT(keel_camera_output_stats, chunks, chunks_dropped, 40)
NEXT(keel_camera_output_stats, chunks_dropped, message, 48)
LAST(keel_camera_output_stats, message, 304)

FIRST(keel_camera_stats, struct_size)
NEXT(keel_camera_stats, struct_size, state, 4)
NEXT(keel_camera_stats, state, frames_captured, 8)
NEXT(keel_camera_stats, frames_captured, frames_read, 16)
NEXT(keel_camera_stats, frames_read, frames_dropped, 24)
NEXT(keel_camera_stats, frames_dropped, encoder_frames_dropped, 32)
NEXT(keel_camera_stats, encoder_frames_dropped, chunks_encoded, 40)
NEXT(keel_camera_stats, chunks_encoded, chunks_read, 48)
NEXT(keel_camera_stats, chunks_read, chunks_dropped, 56)
NEXT(keel_camera_stats, chunks_dropped, outputs, 64)
NEXT(keel_camera_stats, outputs, reserved0, 68)
LAST(keel_camera_stats, reserved0, 72)

FIRST(keel_camera_still_options, struct_size)
NEXT(keel_camera_still_options, struct_size, width, 4)
NEXT(keel_camera_still_options, width, height, 8)
NEXT(keel_camera_still_options, height, quality, 12)
NEXT(keel_camera_still_options, quality, timeout_ms, 16)
NEXT(keel_camera_still_options, timeout_ms, reserved0, 20)
LAST(keel_camera_still_options, reserved0, 24)

FIRST(keel_camera_still_info, struct_size)
NEXT(keel_camera_still_info, struct_size, width, 4)
NEXT(keel_camera_still_info, width, height, 8)
NEXT(keel_camera_still_info, height, bytes, 12)
NEXT(keel_camera_still_info, bytes, flags, 16)
NEXT(keel_camera_still_info, flags, reserved0, 20)
NEXT(keel_camera_still_info, reserved0, timestamp_ns, 24)
LAST(keel_camera_still_info, timestamp_ns, 32)

/* Fixed-size arrays match their documented enumerators. */
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_camera_error, path) == KEEL_CAMERA_PATH_MAX, "path");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_camera_error, message) == KEEL_CAMERA_MESSAGE_MAX, "message");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_camera_info, id) == KEEL_CAMERA_ID_MAX, "id");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_camera_info, model) == KEEL_CAMERA_MODEL_MAX, "model");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_camera_info, sensor) == KEEL_CAMERA_SENSOR_MAX, "sensor");
KEEL_LAYOUT_ASSERT(KEEL_MEMBER_SIZE(keel_camera_output_stats, message) == KEEL_CAMERA_MESSAGE_MAX, "message");

/* Every struct fits the struct-size limit of keel.h. */
KEEL_LAYOUT_ASSERT(sizeof(keel_camera_error) <= KEEL_STRUCT_SIZE_MAX, "struct size limit");

/* Version identity: the packed ABI value follows the version macros. */
KEEL_LAYOUT_ASSERT(KEEL_CAMERA_ABI_VERSION == 0x00010000, "camera ABI version is 1.0");
KEEL_LAYOUT_ASSERT(KEEL_CAMERA_ABI_MAJOR == KEEL_CAMERA_VERSION_MAJOR, "camera ABI major");
KEEL_LAYOUT_ASSERT(KEEL_CAMERA_ABI_MINOR == KEEL_CAMERA_VERSION_MINOR, "camera ABI minor");
