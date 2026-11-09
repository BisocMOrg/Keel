/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Stills: full resolution (libjpeg above the hardware encoder's 1920x1920),
 * a chosen size (the hardware path), into a buffer and into a file, and the
 * refusals (SPEC section 26.8).
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "camera_test.h"

static keel_camera_still_options still_options(uint32_t width, uint32_t height)
{
    keel_camera_still_options options;
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.width = width;
    options.height = height;
    return options;
}

static keel_camera_still_info still_info(void)
{
    keel_camera_still_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    return info;
}

/* Width and height from a JPEG's SOF segment; 0 when there is none. */
static void jpeg_size(const uint8_t *data, uint32_t bytes, uint32_t *width, uint32_t *height)
{
    uint32_t i = 2;
    *width = 0;
    *height = 0;
    while (i + 9u < bytes && data[i] == 0xFF) {
        uint8_t marker = data[i + 1u];
        uint32_t length = ((uint32_t)data[i + 2u] << 8) | data[i + 3u];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            *height = ((uint32_t)data[i + 5u] << 8) | data[i + 6u];
            *width = ((uint32_t)data[i + 7u] << 8) | data[i + 8u];
            return;
        }
        i += 2u + length;
    }
}

static int has_libjpeg(void)
{
    return strstr(keel_camera_version_string(), "jpeg") != NULL;
}

static void still_full_resolution(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_still_info info = still_info();
    uint32_t size = (3280u + 15u) / 16u * 16u * ((2464u + 15u) / 16u * 16u) * 3u + 2048u;
    uint8_t *buffer = malloc(size);
    uint32_t width;
    uint32_t height;
    CHECK(buffer != NULL);
    if (!has_libjpeg()) {
        CHECK_STATUS(keel_camera_capture_still(camera, NULL, buffer, size, &info), KEEL_E_UNSUPPORTED);
        CHECK(strstr(camera_last_error(camera).message, "libjpeg") != NULL);
    } else {
        CHECK_STATUS(keel_camera_capture_still(camera, NULL, buffer, size, &info), KEEL_OK);
        CHECK_EQ(info.width, 3280);
        CHECK_EQ(info.height, 2464);
        CHECK_EQ(info.flags, KEEL_CAMERA_STILL_SOFTWARE);
        CHECK(info.timestamp_ns > 0);
        CHECK(buffer[0] == 0xFF && buffer[1] == 0xD8);
        CHECK(buffer[info.bytes - 2u] == 0xFF && buffer[info.bytes - 1u] == 0xD9);
        jpeg_size(buffer, info.bytes, &width, &height);
        CHECK_EQ(width, 3280);
        CHECK_EQ(height, 2464);
        /* A buffer the image does not fit: refused, the size needed named. */
        CHECK_STATUS(keel_camera_capture_still(camera, NULL, buffer, 1000, &info), KEEL_E_INVALID_ARGUMENT);
        CHECK(strstr(camera_last_error(camera).message, "this still needs") != NULL);
    }
    /* The camera Module 3's full resolution. */
    keel_camera_close(camera);
    {
        keel_camera_options o = camera_options();
        o.index = 1;
        camera = camera_open(&o);
        if (has_libjpeg()) {
            uint32_t big = 4608u * 2592u * 3u + 2048u;
            uint8_t *large = malloc(big);
            CHECK(large != NULL);
            CHECK_STATUS(keel_camera_capture_still(camera, NULL, large, big, &info), KEEL_OK);
            jpeg_size(large, info.bytes, &width, &height);
            CHECK_EQ(width, 4608);
            CHECK_EQ(height, 2592);
            free(large);
        }
        keel_camera_close(camera);
    }
    free(buffer);
}

static void still_hardware(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_still_options options = still_options(1280, 960);
    keel_camera_still_info info = still_info();
    uint8_t buffer[4096];
    uint32_t width;
    uint32_t height;
    options.quality = 95;
    CHECK_STATUS(keel_camera_capture_still(camera, &options, buffer, sizeof(buffer), &info), KEEL_OK);
    CHECK_EQ(info.width, 1280);
    CHECK_EQ(info.height, 960);
    CHECK_EQ(info.flags, 0); /* the (synthetic) hardware encoder */
    jpeg_size(buffer, info.bytes, &width, &height);
    CHECK_EQ(width, 1280);
    CHECK_EQ(height, 960);
    /* The video stream works again afterwards. */
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    {
        keel_camera_stream_info stream = camera_stream(camera);
        uint8_t *frame = malloc(stream.frame_bytes);
        keel_camera_frame_info frame_info;
        CHECK(frame != NULL);
        frame_info = camera_read_frame(camera, frame, stream.frame_bytes);
        CHECK_EQ(frame_info.width, 640);
        camera_check_pattern(frame, &frame_info, 0, 0);
        free(frame);
    }
    /* Not while running. */
    CHECK_STATUS(keel_camera_capture_still(camera, &options, buffer, sizeof(buffer), &info), KEEL_E_STATE);
    CHECK(strstr(camera_last_error(camera).message, "keel_camera_stop") != NULL);
    keel_camera_close(camera);
}

static void still_file(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_still_options options = still_options(640, 480);
    keel_camera_still_info info = still_info();
    struct stat status;
    char path[256];
    char part[300];
    const char *dir = getenv("TMPDIR");
    snprintf(path, sizeof(path), "%s/keel-camera-still-%d.jpg", dir != NULL && dir[0] != '\0' ? dir : "/tmp",
             (int)getpid());
    snprintf(part, sizeof(part), "%s.part", path);
    CHECK_STATUS(keel_camera_capture_still_file(camera, &options, path, &info), KEEL_OK);
    CHECK(stat(path, &status) == 0);
    CHECK_EQ((uint64_t)status.st_size, info.bytes);
    CHECK(stat(part, &status) != 0); /* no partial file is left */
    CHECK_STATUS(keel_camera_capture_still_file(camera, &options, path, NULL), KEEL_OK);
    unlink(path);
    CHECK_STATUS(keel_camera_capture_still_file(camera, &options, "/nonexistent-directory/x.jpg", &info),
                 KEEL_E_NOT_FOUND);
    CHECK_EQ(camera_last_error(camera).resource, KEEL_CAMERA_RESOURCE_FILE);
    CHECK_STATUS(keel_camera_capture_still_file(camera, &options, NULL, &info), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_capture_still_file(camera, &options, "", &info), KEEL_E_INVALID_ARGUMENT);
    keel_camera_close(camera);
}

static void still_options_rules(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_still_info info = still_info();
    uint8_t buffer[64];
    static const struct {
        uint32_t width;
        uint32_t height;
        uint32_t quality;
        uint32_t timeout_ms;
    } bad[] = {
        {640, 0, 0, 0}, {0, 480, 0, 0}, {641, 480, 0, 0}, {62, 62, 0, 0}, {640, 480, 101, 0}, {640, 480, 0, 60001},
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        keel_camera_still_options options = still_options(bad[i].width, bad[i].height);
        options.quality = bad[i].quality;
        options.timeout_ms = bad[i].timeout_ms;
        CHECK_STATUS(keel_camera_capture_still(camera, &options, buffer, sizeof(buffer), &info),
                     KEEL_E_INVALID_ARGUMENT);
    }
    {
        keel_camera_still_options options = still_options(640, 480);
        options.reserved0 = 1;
        CHECK_STATUS(keel_camera_capture_still(camera, &options, buffer, sizeof(buffer), &info),
                     KEEL_E_INVALID_ARGUMENT);
    }
    CHECK_STATUS(keel_camera_capture_still(camera, NULL, NULL, 0, &info), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_capture_still(camera, NULL, buffer, sizeof(buffer), NULL), KEEL_E_INVALID_ARGUMENT);
    keel_camera_close(camera);
}

static void still_encoder_missing(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_still_options options = still_options(640, 480);
    keel_camera_still_info info = still_info();
    uint8_t buffer[640 * 480 * 3 + 2048];
    camera_fault("encoder-missing");
    if (has_libjpeg()) {
        /* Without the hardware image encoder, libjpeg takes over. */
        CHECK_STATUS(keel_camera_capture_still(camera, &options, buffer, sizeof(buffer), &info), KEEL_OK);
        CHECK_EQ(info.flags, KEEL_CAMERA_STILL_SOFTWARE);
    } else {
        CHECK_STATUS(keel_camera_capture_still(camera, &options, buffer, sizeof(buffer), &info), KEEL_E_NOT_FOUND);
    }
    camera_fault(NULL);
    keel_camera_close(camera);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"still_full_resolution", still_full_resolution},
        {"still_hardware", still_hardware},
        {"still_file", still_file},
        {"still_options_rules", still_options_rules},
        {"still_encoder_missing", still_encoder_missing},
    };
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
