/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Camera API: metadata, listing, options, struct sizes, autofocus rules,
 * handles, the thread rule, fork and the I420 conversion (SPEC section 26).
 * Runs against the synthetic camera (KEEL_CAMERA_TEST_SOURCE=1).
 */
#define _GNU_SOURCE

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "camera_test.h"

static void metadata(void)
{
    const char *version = keel_camera_version_string();
    CHECK_EQ(keel_camera_abi_version(), KEEL_CAMERA_ABI_VERSION);
    CHECK_EQ(keel_camera_abi_version() >> 16, KEEL_CAMERA_VERSION_MAJOR);
    CHECK(strncmp(version, "1.0.0 (", 7) == 0);
    CHECK(strstr(version, "libcamera") != NULL);
    CHECK(strstr(version, "test-hooks") != NULL);
    CHECK(strlen(version) < KEEL_STRING_MAX);
}

static keel_camera_list *list_open(void)
{
    keel_camera_error error;
    keel_camera_list *list = NULL;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    CHECK_STATUS(keel_camera_list_open(&list, &error), KEEL_OK);
    CHECK_EQ(error.status, KEEL_OK);
    CHECK(list != NULL);
    return list;
}

static void listing(void)
{
    keel_camera_list *list = list_open();
    keel_camera_info info;
    keel_camera_mode mode;
    uint32_t count = 0;
    CHECK_STATUS(keel_camera_list_count(list, &count), KEEL_OK);
    CHECK_EQ(count, 2);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_list_get(list, 0, &info), KEEL_OK);
    CHECK(strcmp(info.model, "Camera Module 2") == 0);
    CHECK(strcmp(info.sensor, "imx219") == 0);
    CHECK_EQ(info.flags & KEEL_CAMERA_INFO_AUTOFOCUS, 0);
    CHECK_EQ(info.pixel_array_width, 3280);
    CHECK_EQ(info.pixel_array_height, 2464);
    CHECK_EQ(info.mode_count, 8);
    CHECK_STATUS(keel_camera_list_get(list, 1, &info), KEEL_OK);
    CHECK(strcmp(info.model, "Camera Module 3") == 0);
    CHECK(strcmp(info.sensor, "imx708") == 0);
    CHECK_EQ(info.flags & KEEL_CAMERA_INFO_AUTOFOCUS, KEEL_CAMERA_INFO_AUTOFOCUS);
    CHECK_EQ(info.index, 1);
    memset(&mode, 0, sizeof(mode));
    mode.struct_size = sizeof(mode);
    CHECK_STATUS(keel_camera_list_get_mode(list, 0, 0, &mode), KEEL_OK);
    CHECK_EQ(mode.width, 640);
    CHECK_EQ(mode.bit_depth, 8);
    CHECK_EQ(mode.max_framerate_milli, 200160);
    CHECK_EQ(mode.crop_width, 1280);
    CHECK_STATUS(keel_camera_list_get_mode(list, 1, 2, &mode), KEEL_OK);
    CHECK_EQ(mode.width, 4608);
    CHECK_EQ(mode.crop_width, 4608);
    /* Past the end. */
    CHECK_STATUS(keel_camera_list_get(list, 2, &info), KEEL_E_NOT_FOUND);
    CHECK_STATUS(keel_camera_list_get_mode(list, 1, 3, &mode), KEEL_E_NOT_FOUND);
    CHECK_STATUS(keel_camera_list_get(list, 0, NULL), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_list_count(list, NULL), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_list_close(list), KEEL_OK);
    CHECK_STATUS(keel_camera_list_close(list), KEEL_E_INVALID_HANDLE);
    CHECK_STATUS(keel_camera_list_count(list, &count), KEEL_E_INVALID_HANDLE);
    CHECK_STATUS(keel_camera_list_close(NULL), KEEL_OK);
}

static void list_in_use(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_list *list = list_open();
    keel_camera_info info;
    keel_camera_mode mode;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    memset(&mode, 0, sizeof(mode));
    mode.struct_size = sizeof(mode);
    CHECK_STATUS(keel_camera_list_get(list, 0, &info), KEEL_OK);
    CHECK_EQ(info.flags & KEEL_CAMERA_INFO_IN_USE, KEEL_CAMERA_INFO_IN_USE);
    CHECK_STATUS(keel_camera_list_get_mode(list, 0, 0, &mode), KEEL_OK);
    CHECK_EQ(mode.max_framerate_milli, 0);
    CHECK_EQ(mode.width, 640);
    CHECK_STATUS(keel_camera_list_get(list, 1, &info), KEEL_OK);
    CHECK_EQ(info.flags & KEEL_CAMERA_INFO_IN_USE, 0);
    keel_camera_list_close(list);
    keel_camera_close(camera);
}

static void list_faults(void)
{
    static const struct {
        const char *fault;
        int32_t status;
        uint32_t resource;
        const char *words;
    } cases[] = {
        {"permission", KEEL_E_PERMISSION, KEEL_CAMERA_RESOURCE_DEVICE_NODE, "video group"},
        {"ipa-missing", KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_IPA, "ipa_rpi_vc4.so"},
    };
    keel_camera_error error;
    keel_camera_list *list;
    uint32_t count = 99;
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        camera_fault(cases[i].fault);
        memset(&error, 0, sizeof(error));
        error.struct_size = sizeof(error);
        list = (keel_camera_list *)(uintptr_t)1;
        CHECK_STATUS(keel_camera_list_open(&list, &error), cases[i].status);
        CHECK(list == NULL);
        CHECK_EQ(error.status, cases[i].status);
        CHECK_EQ(error.resource, cases[i].resource);
        CHECK(strstr(error.message, cases[i].words) != NULL);
    }
    /* Nothing connected is a valid, empty snapshot. */
    camera_fault("no-camera");
    list = list_open();
    CHECK_STATUS(keel_camera_list_count(list, &count), KEEL_OK);
    CHECK_EQ(count, 0);
    keel_camera_list_close(list);
    camera_fault(NULL);
    /* The error struct follows the struct rules. */
    memset(&error, 0, sizeof(error));
    error.struct_size = 4;
    CHECK_STATUS(keel_camera_list_open(&list, &error), KEEL_E_INVALID_ARGUMENT);
    error.struct_size = sizeof(error);
    CHECK_STATUS(keel_camera_list_open(NULL, &error), KEEL_E_INVALID_ARGUMENT);
    CHECK_EQ(error.status, KEEL_E_INVALID_ARGUMENT);
}

static void open_defaults(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_stream_info stream = camera_stream(camera);
    keel_camera_info info;
    keel_camera_stats stats = camera_stats(camera);
    keel_camera_error error = camera_last_error(camera);
    CHECK_EQ(stream.width, 640);
    CHECK_EQ(stream.height, 480);
    CHECK_EQ(stream.format, KEEL_CAMERA_FORMAT_I420);
    CHECK_EQ(stream.framerate_milli, 30000);
    CHECK_EQ(stream.plane_count, 3);
    CHECK_EQ(stream.stride0, 640);
    CHECK_EQ(stream.stride1, 320);
    CHECK_EQ(stream.offset1, 640 * 480);
    CHECK_EQ(stream.offset2, 640 * 480 + 320 * 240);
    CHECK_EQ(stream.frame_bytes, 640 * 480 * 3 / 2);
    CHECK_EQ(stream.codec, KEEL_CAMERA_CODEC_NONE);
    CHECK_EQ(stream.encoded_bytes_max, 0);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_get_info(camera, &info), KEEL_OK);
    CHECK(strcmp(info.sensor, "imx219") == 0);
    CHECK_EQ(stats.state, KEEL_CAMERA_STATE_STOPPED);
    CHECK_EQ(stats.frames_captured, 0);
    CHECK_EQ(error.status, KEEL_OK);
    CHECK_EQ(error.message[0], '\0');
    CHECK_STATUS(keel_camera_close(camera), KEEL_OK);
}

static void open_sizes(void)
{
    static const struct {
        uint32_t format;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        uint32_t bytes;
    } cases[] = {
        {KEEL_CAMERA_FORMAT_I420, 1280, 720, 1280, 1280 * 720 * 3 / 2},
        {KEEL_CAMERA_FORMAT_I420, 1000, 750, 1024, 1024 * 750 + 512 * 375 * 2},
        {KEEL_CAMERA_FORMAT_RGB888, 640, 480, 1920, 1920 * 480},
        {KEEL_CAMERA_FORMAT_BGR888, 100, 64, 320, 320 * 64},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        keel_camera_options options = camera_options();
        keel_camera *camera;
        keel_camera_stream_info stream;
        options.format = cases[i].format;
        options.width = cases[i].width;
        options.height = cases[i].height;
        options.framerate_milli = 15000;
        camera = camera_open(&options);
        stream = camera_stream(camera);
        CHECK_EQ(stream.width, cases[i].width);
        CHECK_EQ(stream.stride0, cases[i].stride);
        CHECK_EQ(stream.frame_bytes, cases[i].bytes);
        CHECK_EQ(stream.framerate_milli, 15000);
        CHECK_EQ(stream.plane_count, cases[i].format == KEEL_CAMERA_FORMAT_I420 ? 3u : 1u);
        keel_camera_close(camera);
    }
}

static void option_rejected(keel_camera_options options, const char *words)
{
    keel_camera_error error;
    keel_camera *camera = (keel_camera *)(uintptr_t)1;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    CHECK_STATUS(keel_camera_open(&options, &camera, &error), KEEL_E_INVALID_ARGUMENT);
    CHECK(camera == NULL);
    CHECK_EQ(error.status, KEEL_E_INVALID_ARGUMENT);
    CHECK_EQ(error.resource, KEEL_CAMERA_RESOURCE_OPTIONS);
    if (strstr(error.message, words) == NULL) {
        fprintf(stderr, "message '%s' lacks '%s'\n", error.message, words);
        exit(1);
    }
}

static void options_validation(void)
{
    keel_camera_options o;
#define REJECT(field, value, words)                                                           \
    do {                                                                                      \
        o = camera_options();                                                                 \
        o.field = (value);                                                                    \
        option_rejected(o, words);                                                            \
    } while (0)
    REJECT(width, 63, "width");
    REJECT(width, 641, "width");
    REJECT(height, 16386, "height");
    REJECT(framerate_milli, 999, "framerate_milli");
    REJECT(framerate_milli, 240001, "framerate_milli");
    REJECT(format, 4, "format");
    REJECT(flags, 4, "flags");
    REJECT(rotation, 90, "rotation");
    REJECT(af_mode, 4, "af_mode");
    REJECT(af_range, 4, "af_range");
    REJECT(lens_position_milli, 1000, "lens_position_milli");
    REJECT(exposure_us, 1000001, "exposure_us");
    REJECT(gain_milli, 999, "gain_milli");
    REJECT(gain_milli, 64001, "gain_milli");
    REJECT(ev_milli, 8001, "ev_milli");
    REJECT(ev_milli, -8001, "ev_milli");
    REJECT(awb_mode, 8, "awb_mode");
    REJECT(red_gain_milli, 1000, "red_gain_milli");
    REJECT(frame_queue, 9, "frame_queue");
#undef REJECT
    /* Combinations: lens position only with manual focus, gains only with
     * manual white balance, and both gains together. */
    o = camera_options();
    o.af_mode = KEEL_CAMERA_AF_MANUAL;
    o.lens_position_milli = 32001;
    option_rejected(o, "lens_position_milli");
    o = camera_options();
    o.awb_mode = KEEL_CAMERA_AWB_MANUAL;
    o.red_gain_milli = 1500;
    option_rejected(o, "blue_gain_milli");
    o = camera_options();
    o.awb_mode = KEEL_CAMERA_AWB_MANUAL;
    o.red_gain_milli = 1500;
    o.blue_gain_milli = 32001;
    option_rejected(o, "blue_gain_milli");
    /* Every valid extreme opens. */
    o = camera_options();
    o.index = 1;
    o.width = 64;
    o.height = 64;
    o.framerate_milli = 240000;
    o.format = KEEL_CAMERA_FORMAT_BGR888;
    o.flags = KEEL_CAMERA_HFLIP | KEEL_CAMERA_VFLIP;
    o.rotation = 180;
    o.af_mode = KEEL_CAMERA_AF_MANUAL;
    o.af_range = KEEL_CAMERA_AF_RANGE_MACRO;
    o.lens_position_milli = 32000;
    o.exposure_us = 1000000;
    o.gain_milli = 64000;
    o.ev_milli = -8000;
    o.awb_mode = KEEL_CAMERA_AWB_MANUAL;
    o.red_gain_milli = 1;
    o.blue_gain_milli = 32000;
    o.frame_queue = KEEL_CAMERA_FRAME_QUEUE_MAX;
    keel_camera_close(camera_open(&o));
}

static void struct_rules(void)
{
    struct {
        keel_camera_options options;
        uint32_t tail[4];
    } big;
    keel_camera_error error;
    keel_camera_info info;
    keel_camera_stats stats;
    keel_camera_encoder_options encoder;
    keel_camera *camera = NULL;
    /* Options: too small, too large, an unknown tail. */
    memset(&big, 0, sizeof(big));
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    big.options.struct_size = 8;
    CHECK_STATUS(keel_camera_open(&big.options, &camera, &error), KEEL_E_INVALID_ARGUMENT);
    big.options.struct_size = KEEL_STRUCT_SIZE_MAX + 4u;
    CHECK_STATUS(keel_camera_open(&big.options, &camera, &error), KEEL_E_INVALID_ARGUMENT);
    big.options.struct_size = sizeof(big);
    big.tail[2] = 1;
    CHECK_STATUS(keel_camera_open(&big.options, &camera, &error), KEEL_E_INVALID_ARGUMENT);
    CHECK_EQ(error.resource, KEEL_CAMERA_RESOURCE_OPTIONS);
    big.tail[2] = 0;
    camera = camera_open(&big.options); /* a zero tail is accepted */
    /* Outputs: a larger struct is zero-filled past the known size. */
    {
        struct {
            keel_camera_stats stats;
            uint8_t tail[8];
        } bigger;
        memset(&bigger, 0xEE, sizeof(bigger));
        bigger.stats.struct_size = sizeof(bigger);
        CHECK_STATUS(keel_camera_get_stats(camera, &bigger.stats), KEEL_OK);
        CHECK_EQ(bigger.stats.struct_size, sizeof(bigger));
        CHECK_EQ(bigger.tail[0], 0);
        CHECK_EQ(bigger.tail[7], 0);
        CHECK_EQ(bigger.stats.reserved0, 0);
    }
    /* Outputs stay untouched on INVALID_ARGUMENT. */
    memset(&info, 0xEE, sizeof(info));
    info.struct_size = 12;
    CHECK_STATUS(keel_camera_get_info(camera, &info), KEEL_E_INVALID_ARGUMENT);
    CHECK_EQ(info.flags, 0xEEEEEEEEu);
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = KEEL_STRUCT_SIZE_MAX + 4u;
    CHECK_STATUS(keel_camera_get_stats(camera, &stats), KEEL_E_INVALID_ARGUMENT);
    /* An input with an unknown tail: UNSUPPORTED after open, with no effect. */
    {
        struct {
            keel_camera_encoder_options encoder;
            uint32_t tail[2];
        } larger;
        memset(&larger, 0, sizeof(larger));
        larger.encoder.struct_size = sizeof(larger);
        larger.encoder.codec = KEEL_CAMERA_CODEC_H264;
        larger.tail[1] = 7;
        CHECK_STATUS(keel_camera_set_encoder(camera, &larger.encoder), KEEL_E_UNSUPPORTED);
        CHECK_EQ(camera_stream(camera).codec, KEEL_CAMERA_CODEC_NONE);
        larger.tail[1] = 0;
        CHECK_STATUS(keel_camera_set_encoder(camera, &larger.encoder), KEEL_OK);
    }
    encoder = camera_encoder(KEEL_CAMERA_CODEC_H264);
    encoder.struct_size = 4;
    CHECK_STATUS(keel_camera_set_encoder(camera, &encoder), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_set_encoder(camera, NULL), KEEL_E_INVALID_ARGUMENT);
    keel_camera_close(camera);
}

static void autofocus_rules(void)
{
    keel_camera_options o = camera_options();
    keel_camera_error error;
    keel_camera *camera = NULL;
    uint32_t state = 99;
    /* Camera Module 2 has a fixed-focus lens. */
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    o.af_mode = KEEL_CAMERA_AF_CONTINUOUS;
    CHECK_STATUS(keel_camera_open(&o, &camera, &error), KEEL_E_UNSUPPORTED);
    CHECK(strstr(error.message, "fixed-focus") != NULL);
    CHECK(strstr(error.message, "Camera Module 2") != NULL);
    o = camera_options();
    o.af_range = KEEL_CAMERA_AF_RANGE_MACRO;
    CHECK_STATUS(keel_camera_open(&o, &camera, &error), KEEL_E_UNSUPPORTED);
    camera = camera_open(NULL);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    CHECK_STATUS(keel_camera_trigger_autofocus(camera, 500, &state), KEEL_E_UNSUPPORTED);
    keel_camera_close(camera);
    /* Camera Module 3 focuses. */
    o = camera_options();
    o.index = 1;
    o.af_mode = KEEL_CAMERA_AF_AUTO;
    o.af_range = KEEL_CAMERA_AF_RANGE_FULL;
    camera = camera_open(&o);
    CHECK_STATUS(keel_camera_trigger_autofocus(camera, 500, &state), KEEL_E_STATE);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    CHECK_STATUS(keel_camera_trigger_autofocus(camera, 0, &state), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_trigger_autofocus(camera, 500, NULL), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_trigger_autofocus(camera, 1000, &state), KEEL_OK);
    CHECK_EQ(state, KEEL_CAMERA_AF_STATE_FOCUSED);
    keel_camera_close(camera);
    /* Manual focus has no scan. */
    o = camera_options();
    o.index = 1;
    o.af_mode = KEEL_CAMERA_AF_MANUAL;
    o.lens_position_milli = 2000;
    camera = camera_open(&o);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    CHECK_STATUS(keel_camera_trigger_autofocus(camera, 500, &state), KEEL_E_UNSUPPORTED);
    keel_camera_close(camera);
}

static void open_errors(void)
{
    static const struct {
        const char *fault;
        int32_t status;
        uint32_t resource;
        const char *words;
    } cases[] = {
        {"no-camera", KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_CAMERA, "config.txt enables the camera"},
        {"busy", KEEL_E_IN_USE, KEEL_CAMERA_RESOURCE_CAMERA, "in use by another process"},
        {"permission", KEEL_E_PERMISSION, KEEL_CAMERA_RESOURCE_DEVICE_NODE, "keel-setup --user"},
        {"ipa-missing", KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_IPA, "IPA module"},
    };
    keel_camera_options o = camera_options();
    keel_camera_error error;
    keel_camera *camera;
    size_t i;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        camera_fault(cases[i].fault);
        camera = (keel_camera *)(uintptr_t)1;
        CHECK_STATUS(keel_camera_open(NULL, &camera, &error), cases[i].status);
        CHECK(camera == NULL);
        CHECK_EQ(error.resource, cases[i].resource);
        CHECK(strstr(error.message, cases[i].words) != NULL);
    }
    camera_fault(NULL);
    o.index = 2;
    CHECK_STATUS(keel_camera_open(&o, &camera, &error), KEEL_E_NOT_FOUND);
    CHECK(strstr(error.message, "there is no camera 2") != NULL);
    CHECK_STATUS(keel_camera_open(NULL, NULL, &error), KEEL_E_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "out_camera") != NULL);
}

static void busy(void)
{
    keel_camera_options o = camera_options();
    keel_camera_error error;
    keel_camera *first = camera_open(NULL);
    keel_camera *second = NULL;
    keel_camera *other;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    CHECK_STATUS(keel_camera_open(NULL, &second, &error), KEEL_E_IN_USE);
    CHECK(second == NULL);
    CHECK(strstr(error.message, "in use") != NULL);
    o.index = 1;
    other = camera_open(&o); /* another camera is free */
    keel_camera_close(first);
    second = camera_open(NULL); /* free again after close */
    keel_camera_close(second);
    keel_camera_close(other);
}

static void handles(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_stats stats;
    keel_camera *forged = (keel_camera *)(uintptr_t)0x12345678u;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    CHECK_STATUS(keel_camera_get_stats(NULL, &stats), KEEL_E_INVALID_HANDLE);
    CHECK_STATUS(keel_camera_get_stats(forged, &stats), KEEL_E_INVALID_HANDLE);
    CHECK_STATUS(keel_camera_start(forged), KEEL_E_INVALID_HANDLE);
    CHECK_STATUS(keel_camera_close(forged), KEEL_E_INVALID_HANDLE);
    CHECK_STATUS(keel_camera_close(camera), KEEL_OK);
    /* A closed handle stays invalid, even when its slot is reused. */
    CHECK_STATUS(keel_camera_get_stats(camera, &stats), KEEL_E_INVALID_HANDLE);
    CHECK_STATUS(keel_camera_close(camera), KEEL_E_INVALID_HANDLE);
    {
        keel_camera *again = camera_open(NULL);
        CHECK(again != camera);
        CHECK_STATUS(keel_camera_get_stats(camera, &stats), KEEL_E_INVALID_HANDLE);
        keel_camera_close(again);
    }
    /* A list handle is not a camera handle, and the other way round. */
    {
        keel_camera_list *list = NULL;
        keel_camera_error error;
        uint32_t count;
        memset(&error, 0, sizeof(error));
        error.struct_size = sizeof(error);
        CHECK_STATUS(keel_camera_list_open(&list, &error), KEEL_OK);
        CHECK_STATUS(keel_camera_get_stats((keel_camera *)list, &stats), KEEL_E_INVALID_HANDLE);
        camera = camera_open(NULL);
        CHECK_STATUS(keel_camera_list_count((keel_camera_list *)camera, &count), KEEL_E_INVALID_HANDLE);
        keel_camera_close(camera);
        keel_camera_list_close(list);
    }
}

typedef struct thread_call {
    keel_camera *camera;
    int32_t start;
    int32_t stats;
    int32_t read;
    int32_t close;
} thread_call;

static void *foreign_calls(void *arg)
{
    thread_call *call = arg;
    keel_camera_stats stats;
    uint8_t buffer[16];
    keel_camera_frame_info info;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    call->start = keel_camera_start(call->camera);
    call->stats = keel_camera_get_stats(call->camera, &stats);
    call->read = keel_camera_read_frame(call->camera, buffer, sizeof(buffer), 10, &info);
    return NULL;
}

static void thread_rule(void)
{
    thread_call call;
    pthread_t thread;
    memset(&call, 0, sizeof(call));
    call.camera = camera_open(NULL);
    CHECK(pthread_create(&thread, NULL, foreign_calls, &call) == 0);
    pthread_join(thread, NULL);
    CHECK_STATUS(call.start, KEEL_E_CONFLICT);
    CHECK_STATUS(call.stats, KEEL_E_CONFLICT);
    CHECK_STATUS(call.read, KEEL_E_CONFLICT);
    /* Nothing happened: the camera is still stopped. */
    CHECK_EQ(camera_stats(call.camera).state, KEEL_CAMERA_STATE_STOPPED);
    keel_camera_close(call.camera);
}

static void *close_later(void *arg)
{
    thread_call *call = arg;
    camera_sleep_ms(200);
    call->close = keel_camera_close(call->camera);
    return NULL;
}

static void close_from_other_thread(void)
{
    thread_call call;
    pthread_t thread;
    keel_camera_options o = camera_options();
    keel_camera_frame_info info;
    uint8_t *buffer;
    uint32_t bytes;
    int fds = camera_count_fds();
    int threads = camera_count_threads();
    memset(&call, 0, sizeof(call));
    o.framerate_milli = 1000; /* a read waits up to a second for its frame */
    call.camera = camera_open(&o);
    bytes = camera_stream(call.camera).frame_bytes;
    buffer = malloc(bytes);
    CHECK(buffer != NULL);
    CHECK_STATUS(keel_camera_start(call.camera), KEEL_OK);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_read_frame(call.camera, buffer, bytes, 3000, &info), KEEL_OK);
    CHECK(pthread_create(&thread, NULL, close_later, &call) == 0);
    /* Blocked in a read, woken by the close of another thread. */
    CHECK_STATUS(keel_camera_read_frame(call.camera, buffer, bytes, 10000, &info), KEEL_E_INVALID_HANDLE);
    pthread_join(thread, NULL);
    CHECK_STATUS(call.close, KEEL_OK);
    free(buffer);
    CHECK_EQ(camera_count_fds(), (unsigned)fds);
    CHECK_EQ(camera_count_threads(), (unsigned)threads);
}

static void fork_child(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_stats stats;
    int status = 0;
    pid_t child;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int ok = keel_camera_get_stats(camera, &stats) == KEEL_E_NOT_OWNER &&
                 keel_camera_close(camera) == KEEL_E_NOT_OWNER;
        _exit(ok ? 0 : 1);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* The parent's handle is untouched. */
    CHECK_STATUS(keel_camera_get_stats(camera, &stats), KEEL_OK);
    CHECK_EQ(stats.state, KEEL_CAMERA_STATE_RUNNING);
    keel_camera_close(camera);
}

/* An independent statement of the conversion: floor division by 65536. */
static int32_t floor_shift(int32_t value)
{
    int32_t quotient = value / 65536;
    return (value % 65536 != 0 && value < 0) ? quotient - 1 : quotient;
}

static uint8_t clamp_channel(int32_t value)
{
    return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static void reference_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t out[3])
{
    int32_t d = (int32_t)u - 128;
    int32_t e = (int32_t)v - 128;
    out[0] = clamp_channel(y + floor_shift(91881 * e + 32768));
    out[1] = clamp_channel(y + floor_shift(-22554 * d - 46802 * e + 32768));
    out[2] = clamp_channel(y + floor_shift(116130 * d + 32768));
}

static void convert(void)
{
    /* Known values: grey, black, white, the extremes of chroma, clamping. */
    static const uint8_t vectors[][6] = {
        {128, 128, 128, 128, 128, 128}, {0, 128, 128, 0, 0, 0}, {255, 128, 128, 255, 255, 255},
        {76, 85, 255, 254, 0, 0},       {150, 44, 21, 0, 255, 1}, {29, 255, 107, 0, 0, 254},
        {255, 255, 255, 255, 121, 255}, {0, 0, 0, 0, 135, 0},    {16, 128, 128, 16, 16, 16},
    };
    uint8_t frame[4 * 2 + 2 * 1 * 2];
    uint8_t rgb[4 * 2 * 3 + 6];
    keel_camera_frame_info info;
    size_t i;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    info.format = KEEL_CAMERA_FORMAT_I420;
    info.width = 4;
    info.height = 2;
    info.plane_count = 3;
    info.stride0 = 4;
    info.stride1 = 2;
    info.stride2 = 2;
    info.offset1 = 8;
    info.offset2 = 10;
    info.bytes = sizeof(frame);
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        uint8_t expected[3];
        memset(frame, vectors[i][0], 8);
        memset(frame + 8, vectors[i][1], 2);
        memset(frame + 10, vectors[i][2], 2);
        CHECK_STATUS(keel_camera_convert_i420(frame, &info, rgb, sizeof(rgb), KEEL_CAMERA_FORMAT_RGB888, 0), KEEL_OK);
        reference_rgb(vectors[i][0], vectors[i][1], vectors[i][2], expected);
        CHECK_EQ(expected[0], vectors[i][3]);
        CHECK_EQ(expected[1], vectors[i][4]);
        CHECK_EQ(expected[2], vectors[i][5]);
        CHECK_EQ(rgb[0], vectors[i][3]);
        CHECK_EQ(rgb[1], vectors[i][4]);
        CHECK_EQ(rgb[2], vectors[i][5]);
        CHECK_STATUS(keel_camera_convert_i420(frame, &info, rgb, sizeof(rgb), KEEL_CAMERA_FORMAT_BGR888, 0), KEEL_OK);
        CHECK_EQ(rgb[0], vectors[i][5]);
        CHECK_EQ(rgb[2], vectors[i][3]);
    }
    /* Every Y, U and V of a varied frame against the reference. */
    {
        enum { W = 64, H = 32, PAD = 16 };
        static uint8_t big[(W + PAD) * H + 2 * ((W / 2 + PAD) * (H / 2))];
        static uint8_t out[(W * 3 + 5) * H];
        uint32_t x;
        uint32_t y;
        info.width = W;
        info.height = H;
        info.stride0 = W + PAD;
        info.stride1 = W / 2 + PAD;
        info.stride2 = W / 2 + PAD;
        info.offset1 = (W + PAD) * H;
        info.offset2 = info.offset1 + (W / 2 + PAD) * (H / 2);
        info.bytes = sizeof(big);
        for (i = 0; i < sizeof(big); ++i) {
            big[i] = (uint8_t)(i * 37u + 11u);
        }
        CHECK_STATUS(keel_camera_convert_i420(big, &info, out, sizeof(out), KEEL_CAMERA_FORMAT_RGB888, W * 3 + 5),
                     KEEL_OK);
        for (y = 0; y < H; ++y) {
            for (x = 0; x < W; ++x) {
                uint8_t expected[3];
                reference_rgb(big[y * info.stride0 + x], big[info.offset1 + (y / 2) * info.stride1 + x / 2],
                              big[info.offset2 + (y / 2) * info.stride2 + x / 2], expected);
                CHECK(memcmp(out + y * (W * 3 + 5) + x * 3, expected, 3) == 0);
            }
        }
        /* Refusals. */
        CHECK_STATUS(keel_camera_convert_i420(big, &info, out, (W * 3 + 5) * H - 6, KEEL_CAMERA_FORMAT_RGB888,
                                              W * 3 + 5), KEEL_E_INVALID_ARGUMENT);
        CHECK_STATUS(keel_camera_convert_i420(big, &info, out, sizeof(out), KEEL_CAMERA_FORMAT_I420, 0),
                     KEEL_E_INVALID_ARGUMENT);
        CHECK_STATUS(keel_camera_convert_i420(big, &info, out, sizeof(out), KEEL_CAMERA_FORMAT_RGB888, W * 3 - 1),
                     KEEL_E_INVALID_ARGUMENT);
        CHECK_STATUS(keel_camera_convert_i420(NULL, &info, out, sizeof(out), KEEL_CAMERA_FORMAT_RGB888, 0),
                     KEEL_E_INVALID_ARGUMENT);
        info.bytes = info.offset2 + info.stride2 * (H / 2 - 1) + W / 2 - 1; /* the last V sample is outside */
        CHECK_STATUS(keel_camera_convert_i420(big, &info, out, sizeof(out), KEEL_CAMERA_FORMAT_RGB888, 0),
                     KEEL_E_INVALID_ARGUMENT);
        info.bytes = sizeof(big);
        info.format = KEEL_CAMERA_FORMAT_RGB888;
        CHECK_STATUS(keel_camera_convert_i420(big, &info, out, sizeof(out), KEEL_CAMERA_FORMAT_RGB888, 0),
                     KEEL_E_INVALID_ARGUMENT);
    }
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"metadata", metadata},
        {"listing", listing},
        {"list_in_use", list_in_use},
        {"list_faults", list_faults},
        {"open_defaults", open_defaults},
        {"open_sizes", open_sizes},
        {"options_validation", options_validation},
        {"struct_rules", struct_rules},
        {"autofocus_rules", autofocus_rules},
        {"open_errors", open_errors},
        {"busy", busy},
        {"handles", handles},
        {"thread_rule", thread_rule},
        {"close_from_other_thread", close_from_other_thread},
        {"fork_child", fork_child},
        {"convert", convert},
    };
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
