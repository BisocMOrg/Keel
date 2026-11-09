/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Shared helpers of libkeel-camera: error records, clocks, threads, stream
 * layout and sensor model names (SPEC section 26).
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "camera/camera_internal.h"

void cam_copy_string(char *out, size_t size, const char *text)
{
    size_t length;
    if (size == 0) {
        return;
    }
    if (text == NULL) {
        out[0] = '\0';
        return;
    }
    length = strnlen(text, size - 1);
    memcpy(out, text, length);
    out[length] = '\0';
}

void cam_error_clear(cam_error *error)
{
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
}

int32_t cam_fail(cam_error *error, int32_t status, uint32_t resource, int os_error, const char *path,
                 const char *format, ...)
{
    va_list args;
    if (error == NULL) {
        return status;
    }
    error->status = status;
    error->resource = resource;
    error->os_error = os_error;
    cam_copy_string(error->path, sizeof(error->path), path);
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
    return status;
}

int32_t cam_status_for_errno(int os_error)
{
    switch (os_error) {
    case EACCES:
    case EPERM:
    case EROFS:
        return KEEL_E_PERMISSION;
    case ENOENT:
    case ENOTDIR:
    case ENODEV:
    case ENXIO:
        return KEEL_E_NOT_FOUND;
    case EBUSY:
        return KEEL_E_IN_USE;
    case ENOMEM:
        return KEEL_E_NO_MEMORY;
    default:
        return KEEL_E_SETUP_IO;
    }
}

int32_t cam_fail_permission(cam_error *error, uint32_t resource, int os_error, const char *path)
{
    return cam_fail(error, KEEL_E_PERMISSION, resource, os_error, path,
                    "permission denied on %s: add the user to the video group (sudo keel-setup --user NAME), "
                    "then log in again", path);
}

uint64_t cam_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

void cam_deadline(struct timespec *out, uint32_t timeout_ms)
{
    clock_gettime(CLOCK_MONOTONIC, out);
    out->tv_sec += (time_t)(timeout_ms / 1000u);
    out->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

void cam_cond_init(pthread_cond_t *cond)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

int cam_thread_start(pthread_t *thread, const char *name, void *(*run)(void *), void *arg)
{
    sigset_t all;
    sigset_t previous;
    int rc;
    /* The new thread inherits this mask: library threads never run the
     * application's signal handlers. */
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    rc = pthread_create(thread, NULL, run, arg);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (rc == 0 && name != NULL) {
        pthread_setname_np(*thread, name);
    }
    return rc;
}

void cam_stream_layout(cam_stream *stream, uint32_t align)
{
    uint32_t stride;
    memset(stream->stride, 0, sizeof(stream->stride));
    memset(stream->offset, 0, sizeof(stream->offset));
    if (stream->format == KEEL_CAMERA_FORMAT_I420) {
        stride = (stream->width + align - 1u) / align * align;
        stream->plane_count = 3;
        stream->stride[0] = stride;
        stream->stride[1] = stride / 2u;
        stream->stride[2] = stride / 2u;
        stream->offset[1] = stride * stream->height;
        stream->offset[2] = stream->offset[1] + (stride / 2u) * (stream->height / 2u);
        stream->frame_bytes = stream->offset[2] + (stride / 2u) * (stream->height / 2u);
    } else {
        stride = (stream->width * 3u + align - 1u) / align * align;
        stream->plane_count = 1;
        stream->stride[0] = stride;
        stream->frame_bytes = stride * stream->height;
    }
}

const char *cam_model_name(const char *sensor)
{
    static const struct {
        const char *sensor;
        const char *model;
    } models[] = {
        {"imx219", "Camera Module 2"},
        {"imx708", "Camera Module 3"},
        {"imx708_wide", "Camera Module 3 Wide"},
        {"imx708_noir", "Camera Module 3 NoIR"},
        {"imx708_wide_noir", "Camera Module 3 Wide NoIR"},
        {"ov5647", "Camera Module 1"},
        {"imx477", "HQ Camera"},
        {"imx296", "Global Shutter Camera"},
    };
    size_t i;
    for (i = 0; i < sizeof(models) / sizeof(models[0]); ++i) {
        if (strcmp(models[i].sensor, sensor) == 0) {
            return models[i].model;
        }
    }
    return sensor;
}

uint32_t cam_default_bitrate(uint32_t width, uint32_t height, uint32_t framerate_milli)
{
    /* 0.15 bit per pixel and frame, within the encoder's range. */
    uint64_t bits = (uint64_t)width * height * framerate_milli * 15u / 100000u;
    if (bits < KEEL_CAMERA_BITRATE_MIN) {
        bits = KEEL_CAMERA_BITRATE_MIN;
    }
    if (bits > KEEL_CAMERA_BITRATE_MAX) {
        bits = KEEL_CAMERA_BITRATE_MAX;
    }
    return (uint32_t)bits;
}
