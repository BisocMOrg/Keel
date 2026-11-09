/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Compile-only consumer of camera.h. tests/abi/check_headers.sh builds it as
 * C99, C11, C++11 and C++17 with the strictest warning set, so the header
 * stays usable the way applications write code: enumerators stored into
 * uint32_t without casts, struct_size initialisation, and every function.
 */
#include <stddef.h>
#include <string.h>

#include "keel/camera.h"

int keel_camera_header_use(uint8_t *buffer, uint32_t size);

int keel_camera_header_use(uint8_t *buffer, uint32_t size) {
    keel_camera_options options;
    keel_camera_error error;
    keel_camera_list *list = NULL;
    keel_camera_info info;
    keel_camera_mode mode;
    keel_camera_stream_info stream;
    keel_camera_encoder_options encoder;
    keel_camera_frame_info frame;
    keel_camera_encoded_info chunk;
    keel_camera_output_options output_options;
    keel_camera_output_stats output_stats;
    keel_camera_stats stats;
    keel_camera_still_options still;
    keel_camera_still_info still_info;
    keel_camera *camera = NULL;
    uint32_t count = 0;
    uint32_t output = 0;
    uint32_t af_state = KEEL_CAMERA_AF_STATE_IDLE;
    int32_t status;

    if (keel_camera_abi_version() >> 16 != KEEL_CAMERA_ABI_MAJOR || keel_camera_version_string()[0] == '\0') {
        return -1;
    }
    memset(&error, 0, sizeof error);
    error.struct_size = (uint32_t)sizeof error;
    status = keel_camera_list_open(&list, &error);
    if (status == KEEL_OK) {
        memset(&info, 0, sizeof info);
        info.struct_size = (uint32_t)sizeof info;
        memset(&mode, 0, sizeof mode);
        mode.struct_size = (uint32_t)sizeof mode;
        (void)keel_camera_list_count(list, &count);
        if (count > 0 && keel_camera_list_get(list, 0, &info) == KEEL_OK &&
            (info.flags & KEEL_CAMERA_INFO_AUTOFOCUS) != 0u && info.mode_count > 0u) {
            (void)keel_camera_list_get_mode(list, 0, 0, &mode);
        }
        (void)keel_camera_list_close(list);
    }

    memset(&options, 0, sizeof options);
    options.struct_size = (uint32_t)sizeof options;
    options.width = KEEL_CAMERA_WIDTH_DEFAULT;
    options.height = KEEL_CAMERA_HEIGHT_DEFAULT;
    options.framerate_milli = KEEL_CAMERA_FRAMERATE_DEFAULT_MILLI;
    options.format = KEEL_CAMERA_FORMAT_BGR888;
    options.flags = KEEL_CAMERA_HFLIP | KEEL_CAMERA_VFLIP;
    options.af_mode = KEEL_CAMERA_AF_CONTINUOUS;
    options.af_range = KEEL_CAMERA_AF_RANGE_NORMAL;
    options.awb_mode = KEEL_CAMERA_AWB_DAYLIGHT;
    options.ev_milli = -500;
    options.frame_queue = KEEL_CAMERA_FRAME_QUEUE_DEFAULT;
    status = keel_camera_open(&options, &camera, &error);
    if (status != KEEL_OK) {
        return error.status == status ? 1 : -1;
    }
    memset(&stream, 0, sizeof stream);
    stream.struct_size = (uint32_t)sizeof stream;
    (void)keel_camera_get_info(camera, &info);
    (void)keel_camera_get_stream(camera, &stream);

    memset(&encoder, 0, sizeof encoder);
    encoder.struct_size = (uint32_t)sizeof encoder;
    encoder.codec = KEEL_CAMERA_CODEC_H264;
    encoder.profile = KEEL_CAMERA_H264_PROFILE_MAIN;
    encoder.flags = KEEL_CAMERA_INLINE_HEADERS;
    encoder.bitrate = KEEL_CAMERA_BITRATE_MIN;
    encoder.encoded_queue = KEEL_CAMERA_ENCODED_QUEUE_DEFAULT;
    (void)keel_camera_set_encoder(camera, &encoder);

    memset(&output_options, 0, sizeof output_options);
    output_options.struct_size = (uint32_t)sizeof output_options;
    output_options.max_clients = KEEL_CAMERA_CLIENTS_DEFAULT;
    if (keel_camera_output_start(camera, "tcp://:5000?listen", &output_options, &output) == KEEL_OK) {
        memset(&output_stats, 0, sizeof output_stats);
        output_stats.struct_size = (uint32_t)sizeof output_stats;
        (void)keel_camera_get_output_stats(camera, output, &output_stats);
        if (output_stats.state == KEEL_CAMERA_OUTPUT_FAILED) {
            (void)keel_camera_output_stop(camera, output);
        }
    }

    (void)keel_camera_start(camera);
    memset(&frame, 0, sizeof frame);
    frame.struct_size = (uint32_t)sizeof frame;
    if (keel_camera_read_frame(camera, buffer, size, 100, &frame) == KEEL_OK &&
        frame.format == KEEL_CAMERA_FORMAT_I420) {
        (void)keel_camera_convert_i420(buffer, &frame, buffer, size, KEEL_CAMERA_FORMAT_RGB888, 0);
    }
    memset(&chunk, 0, sizeof chunk);
    chunk.struct_size = (uint32_t)sizeof chunk;
    if (keel_camera_read_encoded(camera, buffer, size, 100, &chunk) == KEEL_OK &&
        (chunk.flags & KEEL_CAMERA_ENCODED_KEYFRAME) != 0u) {
        (void)keel_camera_trigger_autofocus(camera, 1000, &af_state);
    }
    memset(&stats, 0, sizeof stats);
    stats.struct_size = (uint32_t)sizeof stats;
    (void)keel_camera_get_stats(camera, &stats);
    if (stats.state == KEEL_CAMERA_STATE_FAILED) {
        (void)keel_camera_last_error(camera, &error);
    }
    (void)keel_camera_stop(camera);

    memset(&still, 0, sizeof still);
    still.struct_size = (uint32_t)sizeof still;
    still.quality = KEEL_CAMERA_QUALITY_DEFAULT;
    memset(&still_info, 0, sizeof still_info);
    still_info.struct_size = (uint32_t)sizeof still_info;
    (void)keel_camera_capture_still(camera, &still, buffer, size, &still_info);
    (void)keel_camera_capture_still_file(camera, NULL, "still.jpg", NULL);
    return keel_camera_close(camera) == KEEL_OK && af_state != KEEL_CAMERA_AF_STATE_FAILED ? 0 : 1;
}
