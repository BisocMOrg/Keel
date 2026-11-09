/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * A C99 program that sees only <keel/camera.h> and libkeel-camera.so: it
 * lists the cameras and, when one is present (the synthetic camera in the
 * test suite), captures raw frames and H.264 chunks. It needs no libkeel.so
 * and no keel_open().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <keel/camera.h>

static int failed(const char *what, int32_t status, keel_camera *camera)
{
    keel_camera_error error;
    memset(&error, 0, sizeof error);
    error.struct_size = sizeof error;
    if (camera != NULL) {
        keel_camera_last_error(camera, &error);
    }
    fprintf(stderr, "%s failed (%d): %s\n", what, (int)status, error.message);
    return 1;
}

int main(void)
{
    keel_camera_error error;
    keel_camera_list *list = NULL;
    keel_camera *camera = NULL;
    keel_camera_encoder_options encoder;
    keel_camera_stream_info stream;
    keel_camera_frame_info frame;
    keel_camera_encoded_info chunk;
    uint32_t count = 0;
    uint8_t *buffer;
    int32_t rc;
    int n;

    if (keel_camera_abi_version() >> 16 != KEEL_CAMERA_ABI_MAJOR) {
        fprintf(stderr, "libkeel-camera ABI %u, built for %u\n", keel_camera_abi_version() >> 16,
                (unsigned)KEEL_CAMERA_ABI_MAJOR);
        return 1;
    }
    printf("libkeel-camera %s\n", keel_camera_version_string());
    memset(&error, 0, sizeof error);
    error.struct_size = sizeof error;
    rc = keel_camera_list_open(&list, &error);
    if (rc != KEEL_OK) {
        fprintf(stderr, "listing failed: %s\n", error.message);
        return 1;
    }
    keel_camera_list_count(list, &count);
    keel_camera_list_close(list);
    printf("%u camera(s)\n", count);
    if (count == 0) {
        return 0; /* nothing to capture from on this machine */
    }

    rc = keel_camera_open(NULL, &camera, &error);
    if (rc != KEEL_OK) {
        fprintf(stderr, "open failed: %s\n", error.message);
        return 1;
    }
    memset(&encoder, 0, sizeof encoder);
    encoder.struct_size = sizeof encoder;
    encoder.codec = KEEL_CAMERA_CODEC_H264;
    rc = keel_camera_set_encoder(camera, &encoder);
    if (rc != KEEL_OK) {
        return failed("keel_camera_set_encoder", rc, camera);
    }
    memset(&stream, 0, sizeof stream);
    stream.struct_size = sizeof stream;
    keel_camera_get_stream(camera, &stream);
    buffer = malloc(stream.frame_bytes > stream.encoded_bytes_max ? stream.frame_bytes : stream.encoded_bytes_max);
    if (buffer == NULL) {
        return 1;
    }
    rc = keel_camera_start(camera);
    if (rc != KEEL_OK) {
        return failed("keel_camera_start", rc, camera);
    }
    for (n = 0; n < 5; ++n) {
        memset(&frame, 0, sizeof frame);
        frame.struct_size = sizeof frame;
        rc = keel_camera_read_frame(camera, buffer, stream.frame_bytes, 2000, &frame);
        if (rc != KEEL_OK) {
            return failed("keel_camera_read_frame", rc, camera);
        }
        memset(&chunk, 0, sizeof chunk);
        chunk.struct_size = sizeof chunk;
        rc = keel_camera_read_encoded(camera, buffer, stream.encoded_bytes_max, 2000, &chunk);
        if (rc != KEEL_OK) {
            return failed("keel_camera_read_encoded", rc, camera);
        }
    }
    printf("frame %ux%u, last chunk %u bytes%s\n", frame.width, frame.height, chunk.bytes,
           (chunk.flags & KEEL_CAMERA_ENCODED_KEYFRAME) ? " (keyframe)" : "");
    free(buffer);
    return keel_camera_close(camera) == KEEL_OK ? 0 : 1;
}
