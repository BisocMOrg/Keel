/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Capture: raw frames (formats, layout, content, flips, order, the drop
 * policy, timeouts and states) and encoded chunks (H.264 and MJPEG, the
 * parameter sets, colour signalling, the drop-to-keyframe policy, encoder
 * options and failures), against the synthetic camera and encoder
 * (SPEC section 26).
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "camera_test.h"

static uint8_t *frame_buffer(keel_camera *camera, uint32_t *bytes)
{
    uint8_t *buffer;
    *bytes = camera_stream(camera).frame_bytes;
    buffer = malloc(*bytes);
    CHECK(buffer != NULL);
    return buffer;
}

static void frames_formats(void)
{
    static const struct {
        uint32_t format;
        uint32_t width;
        uint32_t height;
    } cases[] = {
        {KEEL_CAMERA_FORMAT_I420, 640, 480},   {KEEL_CAMERA_FORMAT_RGB888, 640, 480},
        {KEEL_CAMERA_FORMAT_BGR888, 640, 480}, {KEEL_CAMERA_FORMAT_I420, 1280, 720},
        {KEEL_CAMERA_FORMAT_RGB888, 1280, 720}, {KEEL_CAMERA_FORMAT_I420, 1000, 750},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        keel_camera_options o = camera_options();
        keel_camera *camera;
        keel_camera_stream_info stream;
        keel_camera_frame_info info;
        uint32_t bytes;
        uint8_t *buffer;
        int n;
        o.format = cases[i].format;
        o.width = cases[i].width;
        o.height = cases[i].height;
        o.framerate_milli = 60000;
        camera = camera_open(&o);
        stream = camera_stream(camera);
        buffer = frame_buffer(camera, &bytes);
        CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
        for (n = 0; n < 3; ++n) {
            info = camera_read_frame(camera, buffer, bytes);
            CHECK_EQ(info.format, cases[i].format);
            CHECK_EQ(info.width, cases[i].width);
            CHECK_EQ(info.height, cases[i].height);
            CHECK_EQ(info.bytes, stream.frame_bytes);
            CHECK_EQ(info.stride0, stream.stride0);
            CHECK_EQ(info.stride1, stream.stride1);
            CHECK_EQ(info.offset2, stream.offset2);
            CHECK_EQ(info.plane_count, stream.plane_count);
            CHECK_EQ(info.stride0 % 64, 0);
            camera_check_pattern(buffer, &info, 0, 0);
        }
        keel_camera_close(camera);
        free(buffer);
    }
}

static void frames_flips(void)
{
    static const struct {
        uint32_t flags;
        uint32_t rotation;
        int hflip;
        int vflip;
    } cases[] = {
        {KEEL_CAMERA_HFLIP, 0, 1, 0}, {KEEL_CAMERA_VFLIP, 0, 0, 1}, {0, 180, 1, 1},
        {KEEL_CAMERA_HFLIP, 180, 0, 1}, {KEEL_CAMERA_HFLIP | KEEL_CAMERA_VFLIP, 0, 1, 1},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t format;
        for (format = KEEL_CAMERA_FORMAT_I420; format <= KEEL_CAMERA_FORMAT_RGB888; ++format) {
            keel_camera_options o = camera_options();
            keel_camera *camera;
            keel_camera_frame_info info;
            uint32_t bytes;
            uint8_t *buffer;
            o.flags = cases[i].flags;
            o.rotation = cases[i].rotation;
            o.format = format;
            o.width = 128;
            o.height = 96;
            camera = camera_open(&o);
            buffer = frame_buffer(camera, &bytes);
            CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
            info = camera_read_frame(camera, buffer, bytes);
            camera_check_pattern(buffer, &info, cases[i].hflip, cases[i].vflip);
            keel_camera_close(camera);
            free(buffer);
        }
    }
}

static void frames_order(void)
{
    keel_camera_options o = camera_options();
    keel_camera *camera;
    keel_camera_frame_info info;
    keel_camera_stats stats;
    uint64_t sequence = 0;
    uint64_t timestamp = 0;
    uint32_t bytes;
    uint8_t *buffer;
    int n;
    o.width = 96;
    o.height = 64;
    o.framerate_milli = 100000;
    o.frame_queue = KEEL_CAMERA_FRAME_QUEUE_MAX;
    camera = camera_open(&o);
    buffer = frame_buffer(camera, &bytes);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    for (n = 0; n < 30; ++n) {
        info = camera_read_frame(camera, buffer, bytes);
        if (n > 0) {
            CHECK(info.sequence > sequence);
            CHECK(info.timestamp_ns > timestamp);
        }
        sequence = info.sequence;
        timestamp = info.timestamp_ns;
    }
    stats = camera_stats(camera);
    CHECK_EQ(stats.state, KEEL_CAMERA_STATE_RUNNING);
    CHECK_EQ(stats.frames_read, 30);
    CHECK(stats.frames_captured >= 30);
    keel_camera_close(camera);
    free(buffer);
}

static void frames_drop_oldest(void)
{
    keel_camera_options o = camera_options();
    keel_camera *camera;
    keel_camera_frame_info first;
    keel_camera_frame_info second;
    keel_camera_stats stats;
    uint32_t bytes;
    uint8_t *buffer;
    o.width = 96;
    o.height = 64;
    o.framerate_milli = 200000;
    camera = camera_open(&o); /* the default queue keeps the newest frame only */
    buffer = frame_buffer(camera, &bytes);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    first = camera_read_frame(camera, buffer, bytes);
    camera_sleep_ms(200); /* a slow consumer */
    second = camera_read_frame(camera, buffer, bytes);
    CHECK(second.sequence > first.sequence + 10u);
    stats = camera_stats(camera);
    CHECK(stats.frames_dropped >= 10);
    camera_check_pattern(buffer, &second, 0, 0);
    keel_camera_close(camera);
    free(buffer);
}

static void read_states(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_frame_info info;
    keel_camera_encoded_info encoded;
    keel_camera_error error;
    uint32_t bytes;
    uint8_t *buffer = frame_buffer(camera, &bytes);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    memset(&encoded, 0, sizeof(encoded));
    encoded.struct_size = sizeof(encoded);
    /* Before start. */
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, bytes, 0, &info), KEEL_E_STATE);
    error = camera_last_error(camera);
    CHECK_EQ(error.status, KEEL_E_STATE);
    CHECK(strstr(error.message, "keel_camera_start") != NULL);
    CHECK_STATUS(keel_camera_read_encoded(camera, buffer, bytes, 0, &encoded), KEEL_E_STATE);
    CHECK_STATUS(keel_camera_stop(camera), KEEL_OK); /* stopping a stopped camera is fine */
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    CHECK_STATUS(keel_camera_start(camera), KEEL_E_STATE);
    /* Arguments. */
    CHECK_STATUS(keel_camera_read_frame(camera, NULL, bytes, 0, &info), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, bytes, 0, NULL), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, bytes, KEEL_CAMERA_TIMEOUT_MAX_MS + 1u, &info),
                 KEEL_E_INVALID_ARGUMENT);
    /* A buffer too small: refused, and the frame stays queued. */
    camera_read_frame(camera, buffer, bytes);
    camera_sleep_ms(100);
    info.sequence = 12345;
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, bytes - 1u, 0, &info), KEEL_E_INVALID_ARGUMENT);
    CHECK_EQ(info.sequence, 12345); /* untouched */
    CHECK(strstr(camera_last_error(camera).message, "frame_bytes") != NULL);
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, bytes, 0, &info), KEEL_OK);
    /* After stop: STATE again, and the counters keep their values. */
    CHECK_STATUS(keel_camera_stop(camera), KEEL_OK);
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, bytes, 0, &info), KEEL_E_STATE);
    CHECK_EQ(camera_stats(camera).state, KEEL_CAMERA_STATE_STOPPED);
    CHECK(camera_stats(camera).frames_read >= 2);
    /* Start again after stop. */
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    camera_read_frame(camera, buffer, bytes);
    keel_camera_close(camera);
    free(buffer);
}

static void read_timeout(void)
{
    keel_camera *camera;
    keel_camera_frame_info info;
    uint32_t bytes;
    uint8_t *buffer;
    int n;
    camera_fault("stall"); /* three frames, then nothing */
    camera = camera_open(NULL);
    buffer = frame_buffer(camera, &bytes);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    for (n = 0; n < 3; ++n) {
        info = camera_read_frame(camera, buffer, bytes);
    }
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, bytes, 300, &info), KEEL_E_AGAIN);
    CHECK(strstr(camera_last_error(camera).message, "300 ms") != NULL);
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, bytes, 0, &info), KEEL_E_AGAIN);
    CHECK_EQ(camera_stats(camera).state, KEEL_CAMERA_STATE_RUNNING);
    keel_camera_close(camera);
    camera_fault(NULL);
    free(buffer);
}

static void capture_failure(void)
{
    keel_camera *camera;
    keel_camera_frame_info info;
    keel_camera_error error;
    uint32_t bytes;
    uint8_t *buffer;
    int32_t rc = KEEL_OK;
    int n;
    camera_fault("capture-fail");
    camera = camera_open(NULL);
    buffer = frame_buffer(camera, &bytes);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    for (n = 0; n < 10 && rc == KEEL_OK; ++n) {
        rc = keel_camera_read_frame(camera, buffer, bytes, 1000, &info);
    }
    CHECK_STATUS(rc, KEEL_E_PERIPHERAL);
    error = camera_last_error(camera);
    CHECK_EQ(error.status, KEEL_E_PERIPHERAL);
    CHECK_EQ(error.resource, KEEL_CAMERA_RESOURCE_CAMERA);
    CHECK(strstr(error.message, "stopped delivering") != NULL);
    CHECK_EQ(camera_stats(camera).state, KEEL_CAMERA_STATE_FAILED);
    CHECK_STATUS(keel_camera_start(camera), KEEL_E_STATE);
    CHECK_STATUS(keel_camera_stop(camera), KEEL_OK);
    CHECK_EQ(camera_stats(camera).state, KEEL_CAMERA_STATE_STOPPED);
    camera_fault(NULL);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    camera_read_frame(camera, buffer, bytes);
    keel_camera_close(camera);
    free(buffer);
}

/* ---- encoded ------------------------------------------------------------ */

static keel_camera *open_encoded(uint32_t codec, uint32_t flags, uint32_t queue, uint32_t framerate_milli)
{
    keel_camera_options o = camera_options();
    keel_camera_encoder_options e = camera_encoder(codec);
    keel_camera *camera;
    o.width = 320;
    o.height = 240;
    o.framerate_milli = framerate_milli;
    camera = camera_open(&o);
    e.flags = flags;
    e.encoded_queue = queue;
    if (codec == KEEL_CAMERA_CODEC_H264) {
        e.intra_period = 10;
    }
    CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_OK);
    return camera;
}

static void encoded_h264(void)
{
    keel_camera *camera = open_encoded(KEEL_CAMERA_CODEC_H264, 0, 0, 100000);
    keel_camera_stream_info stream = camera_stream(camera);
    keel_camera_encoded_info info;
    uint8_t *buffer;
    uint64_t last = 0;
    int keyframes = 0;
    int n;
    CHECK_EQ(stream.codec, KEEL_CAMERA_CODEC_H264);
    CHECK(stream.encoded_bytes_max >= 65536);
    buffer = malloc(stream.encoded_bytes_max);
    CHECK(buffer != NULL);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    for (n = 0; n < 25; ++n) {
        info = camera_read_encoded(camera, buffer, stream.encoded_bytes_max);
        CHECK_EQ(info.codec, KEEL_CAMERA_CODEC_H264);
        CHECK(info.bytes > 0 && info.bytes <= stream.encoded_bytes_max);
        CHECK(n == 0 || info.sequence > last);
        CHECK(info.timestamp_ns > 0);
        last = info.sequence;
        if (n == 0) {
            /* The first chunk is a keyframe with the parameter sets. */
            CHECK_EQ(info.flags, KEEL_CAMERA_ENCODED_KEYFRAME | KEEL_CAMERA_ENCODED_HEADERS);
            CHECK_EQ(camera_first_nal(buffer, info.bytes), 7);
            CHECK_EQ(camera_count_nal(buffer, info.bytes, 8), 1);
            CHECK_EQ(camera_count_nal(buffer, info.bytes, 5), 1);
            CHECK_EQ(camera_count_nal(buffer, info.bytes, 7), 1);
        } else if (info.flags & KEEL_CAMERA_ENCODED_KEYFRAME) {
            /* Without KEEL_CAMERA_INLINE_HEADERS later keyframes carry none. */
            CHECK_EQ(info.flags, KEEL_CAMERA_ENCODED_KEYFRAME);
            CHECK_EQ(camera_first_nal(buffer, info.bytes), 5);
            CHECK_EQ(camera_count_nal(buffer, info.bytes, 7), 0);
        } else {
            CHECK_EQ(camera_first_nal(buffer, info.bytes), 1);
        }
        keyframes += (info.flags & KEEL_CAMERA_ENCODED_KEYFRAME) != 0;
    }
    /* intra_period 10: keyframes at 0, 10 and 20. */
    CHECK_EQ(keyframes, 3);
    CHECK_EQ(camera_stats(camera).chunks_read, 25);
    keel_camera_close(camera);
    free(buffer);
}

static void encoded_inline_headers(void)
{
    keel_camera *camera = open_encoded(KEEL_CAMERA_CODEC_H264, KEEL_CAMERA_INLINE_HEADERS, 0, 100000);
    keel_camera_stream_info stream = camera_stream(camera);
    keel_camera_encoded_info info;
    uint8_t *buffer = malloc(stream.encoded_bytes_max);
    int with_headers = 0;
    int n;
    CHECK(buffer != NULL);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    for (n = 0; n < 21; ++n) {
        info = camera_read_encoded(camera, buffer, stream.encoded_bytes_max);
        if (info.flags & KEEL_CAMERA_ENCODED_KEYFRAME) {
            CHECK_EQ(info.flags, KEEL_CAMERA_ENCODED_KEYFRAME | KEEL_CAMERA_ENCODED_HEADERS);
            CHECK_EQ(camera_first_nal(buffer, info.bytes), 7);
            with_headers++;
        }
    }
    CHECK_EQ(with_headers, 3);
    keel_camera_close(camera);
    free(buffer);
}

static void encoded_mjpeg(void)
{
    keel_camera *camera = open_encoded(KEEL_CAMERA_CODEC_MJPEG, 0, 0, 60000);
    keel_camera_stream_info stream = camera_stream(camera);
    keel_camera_encoded_info info;
    uint8_t *buffer = malloc(stream.encoded_bytes_max);
    char text[64];
    int n;
    CHECK(buffer != NULL);
    CHECK_EQ(stream.codec, KEEL_CAMERA_CODEC_MJPEG);
    CHECK(stream.encoded_bytes_max >= 320u * 240u);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    for (n = 0; n < 10; ++n) {
        info = camera_read_encoded(camera, buffer, stream.encoded_bytes_max);
        CHECK_EQ(info.flags, KEEL_CAMERA_ENCODED_KEYFRAME);
        CHECK_EQ(info.codec, KEEL_CAMERA_CODEC_MJPEG);
        CHECK(buffer[0] == 0xFF && buffer[1] == 0xD8);
        CHECK(buffer[info.bytes - 2u] == 0xFF && buffer[info.bytes - 1u] == 0xD9);
        snprintf(text, sizeof(text), "frame %llu 320x240", (unsigned long long)info.sequence);
        CHECK(memmem(buffer, info.bytes, text, strlen(text)) != NULL);
    }
    keel_camera_close(camera);
    free(buffer);
}

static void encoded_drop_to_keyframe(void)
{
    keel_camera *camera = open_encoded(KEEL_CAMERA_CODEC_H264, 0, 4, 200000);
    keel_camera_stream_info stream = camera_stream(camera);
    keel_camera_encoded_info info;
    keel_camera_stats stats;
    uint8_t *buffer = malloc(stream.encoded_bytes_max);
    int n;
    CHECK(buffer != NULL);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    info = camera_read_encoded(camera, buffer, stream.encoded_bytes_max);
    CHECK(info.flags & KEEL_CAMERA_ENCODED_KEYFRAME);
    camera_sleep_ms(400); /* the queue of 4 overflows many times */
    info = camera_read_encoded(camera, buffer, stream.encoded_bytes_max);
    /* After a drop the reader resumes at a keyframe. */
    CHECK(info.flags & KEEL_CAMERA_ENCODED_KEYFRAME);
    for (n = 0; n < 3; ++n) {
        keel_camera_encoded_info next = camera_read_encoded(camera, buffer, stream.encoded_bytes_max);
        CHECK(next.sequence > info.sequence);
        info = next;
    }
    stats = camera_stats(camera);
    CHECK(stats.chunks_dropped > 10);
    keel_camera_close(camera);
    free(buffer);
}

static void encoded_buffer_too_small(void)
{
    keel_camera *camera = open_encoded(KEEL_CAMERA_CODEC_H264, 0, 0, 30000);
    keel_camera_stream_info stream = camera_stream(camera);
    keel_camera_encoded_info info;
    uint8_t *buffer = malloc(stream.encoded_bytes_max);
    CHECK(buffer != NULL);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    camera_sleep_ms(150);
    CHECK_STATUS(keel_camera_read_encoded(camera, buffer, 8, 1000, &info), KEEL_E_INVALID_ARGUMENT);
    CHECK(strstr(camera_last_error(camera).message, "encoded_bytes_max") != NULL);
    /* The chunk stayed queued: the keyframe comes next. */
    info = camera_read_encoded(camera, buffer, stream.encoded_bytes_max);
    CHECK(info.flags & KEEL_CAMERA_ENCODED_KEYFRAME);
    keel_camera_close(camera);
    free(buffer);
}

static void encoder_options(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_encoder_options e;
    keel_camera_error error;
#define REJECT(codec_value, field, value, words)                                              \
    do {                                                                                      \
        e = camera_encoder(codec_value);                                                      \
        e.field = (value);                                                                    \
        CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_E_INVALID_ARGUMENT);           \
        error = camera_last_error(camera);                                                    \
        CHECK(strstr(error.message, words) != NULL);                                          \
    } while (0)
    REJECT(KEEL_CAMERA_CODEC_H264, codec, 3, "codec");
    REJECT(KEEL_CAMERA_CODEC_H264, bitrate, 24999, "bitrate");
    REJECT(KEEL_CAMERA_CODEC_H264, bitrate, 25000001, "bitrate");
    REJECT(KEEL_CAMERA_CODEC_H264, intra_period, 1001, "intra_period");
    REJECT(KEEL_CAMERA_CODEC_H264, profile, 4, "profile");
    REJECT(KEEL_CAMERA_CODEC_H264, quality, 50, "quality");
    REJECT(KEEL_CAMERA_CODEC_H264, flags, 2, "flags");
    REJECT(KEEL_CAMERA_CODEC_H264, encoded_queue, 257, "encoded_queue");
    REJECT(KEEL_CAMERA_CODEC_MJPEG, bitrate, 1000000, "H.264");
    REJECT(KEEL_CAMERA_CODEC_MJPEG, flags, KEEL_CAMERA_INLINE_HEADERS, "H.264");
    REJECT(KEEL_CAMERA_CODEC_MJPEG, quality, 101, "quality");
#undef REJECT
    e = camera_encoder(KEEL_CAMERA_CODEC_H264);
    e.bitrate = 25000000;
    e.intra_period = 1000;
    e.profile = KEEL_CAMERA_H264_PROFILE_BASELINE;
    e.flags = KEEL_CAMERA_INLINE_HEADERS;
    e.encoded_queue = 256;
    CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_OK);
    /* Not while running. */
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_E_STATE);
    CHECK_STATUS(keel_camera_stop(camera), KEEL_OK);
    /* NONE removes it. */
    e = camera_encoder(KEEL_CAMERA_CODEC_NONE);
    CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_OK);
    CHECK_EQ(camera_stream(camera).codec, KEEL_CAMERA_CODEC_NONE);
    CHECK_EQ(camera_stream(camera).encoded_bytes_max, 0);
    keel_camera_close(camera);
    /* The hardware encoders take at most 1920x1920. */
    {
        keel_camera_options o = camera_options();
        o.width = 1920;
        o.height = 1080;
        camera = camera_open(&o);
        e = camera_encoder(KEEL_CAMERA_CODEC_H264);
        CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_OK);
        keel_camera_close(camera);
        o.width = 2304;
        o.height = 1296;
        camera = camera_open(&o);
        CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_E_UNSUPPORTED);
        CHECK(strstr(camera_last_error(camera).message, "1920x1920") != NULL);
        keel_camera_close(camera);
    }
}

static void encoder_missing(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_encoder_options e = camera_encoder(KEEL_CAMERA_CODEC_H264);
    keel_camera_error error;
    camera_fault("encoder-missing");
    CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_E_NOT_FOUND);
    error = camera_last_error(camera);
    CHECK_EQ(error.resource, KEEL_CAMERA_RESOURCE_ENCODER);
    CHECK(strstr(error.message, "H.264") != NULL);
    camera_fault(NULL);
    keel_camera_close(camera);
}

static void encoder_failure(void)
{
    keel_camera *camera;
    keel_camera_stream_info stream;
    keel_camera_encoded_info info;
    uint8_t *buffer;
    int32_t rc = KEEL_OK;
    int n;
    camera_fault("encoder-fail");
    camera = open_encoded(KEEL_CAMERA_CODEC_H264, 0, 0, 60000);
    stream = camera_stream(camera);
    buffer = malloc(stream.encoded_bytes_max);
    CHECK(buffer != NULL);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    for (n = 0; n < 10 && rc == KEEL_OK; ++n) {
        rc = keel_camera_read_encoded(camera, buffer, stream.encoded_bytes_max, 1000, &info);
    }
    CHECK_STATUS(rc, KEEL_E_PERIPHERAL);
    CHECK_EQ(camera_last_error(camera).resource, KEEL_CAMERA_RESOURCE_ENCODER);
    CHECK_EQ(camera_stats(camera).state, KEEL_CAMERA_STATE_FAILED);
    camera_fault(NULL);
    keel_camera_close(camera);
    free(buffer);
}

static void frames_with_encoder(void)
{
    /* Frames into the program and chunks at the same time. */
    keel_camera *camera = open_encoded(KEEL_CAMERA_CODEC_H264, 0, 0, 60000);
    keel_camera_stream_info stream = camera_stream(camera);
    uint8_t *frame = malloc(stream.frame_bytes);
    uint8_t *chunk = malloc(stream.encoded_bytes_max);
    int n;
    CHECK(frame != NULL && chunk != NULL);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    for (n = 0; n < 10; ++n) {
        keel_camera_frame_info info = camera_read_frame(camera, frame, stream.frame_bytes);
        camera_check_pattern(frame, &info, 0, 0);
        (void)camera_read_encoded(camera, chunk, stream.encoded_bytes_max);
    }
    keel_camera_close(camera);
    free(frame);
    free(chunk);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"frames_formats", frames_formats},
        {"frames_flips", frames_flips},
        {"frames_order", frames_order},
        {"frames_drop_oldest", frames_drop_oldest},
        {"read_states", read_states},
        {"read_timeout", read_timeout},
        {"capture_failure", capture_failure},
        {"encoded_h264", encoded_h264},
        {"encoded_inline_headers", encoded_inline_headers},
        {"encoded_mjpeg", encoded_mjpeg},
        {"encoded_drop_to_keyframe", encoded_drop_to_keyframe},
        {"encoded_buffer_too_small", encoded_buffer_too_small},
        {"encoder_options", encoder_options},
        {"encoder_missing", encoder_missing},
        {"encoder_failure", encoder_failure},
        {"frames_with_encoder", frames_with_encoder},
    };
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
