/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * The public API of libkeel-camera (include/keel/camera.h, SPEC section 26):
 * argument and struct-size checks, option validation, the handle registry
 * and the thread rule. Behind it, camera_session.c runs an open camera.
 *
 * Handles are tokens (generation << 8) | (slot + 1) cast to the opaque
 * pointer types; the library never dereferences them. Each slot records
 * the owning thread and process and counts the calls in flight, so a close
 * from another thread can wake a blocked read and wait for it to return
 * before anything is freed.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "camera/camera_session.h"

#ifndef KEEL_CAMERA_BUILD_TRIPLE
#define KEEL_CAMERA_BUILD_TRIPLE "unknown"
#endif

enum {
    SLOT_COUNT = 64,
    SLOT_BITS = 8,
    GENERATION_MASK = 0xFFFFFF,
    KIND_CAMERA = 1,
    KIND_LIST = 2,
    LENS_POSITION_MAX = 32000,
    EXPOSURE_MAX_US = 1000000,
    GAIN_MIN = 1000,
    GAIN_MAX = 64000,
    EV_MAX = 8000,
    COLOUR_GAIN_MAX = 32000,
    INTRA_MAX = 1000
};

typedef struct camera_list {
    uint32_t count;
    cam_camera_desc cameras[CAM_CAMERAS_MAX];
} camera_list;

typedef struct slot {
    uint32_t generation;
    uint32_t kind;          /* 0 while free */
    int closing;
    uint32_t calls;         /* calls in flight, close included */
    pid_t pid;
    pthread_t owner;
    void *object;           /* cam_session or camera_list */
    cam_error last_error;   /* owner thread */
} slot;

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t registry_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t registry_once = PTHREAD_ONCE_INIT;
static slot slots[SLOT_COUNT];

/* ---- registry ----------------------------------------------------------- */

static void before_fork(void)
{
    pthread_mutex_lock(&registry_lock);
}

static void after_fork(void)
{
    pthread_mutex_unlock(&registry_lock);
}

static void registry_init(void)
{
    /* A child never inherits the registry lock mid-update. */
    pthread_atfork(before_fork, after_fork, after_fork);
}

static int32_t registry_add(uint32_t kind, void *object, uintptr_t *out)
{
    uint32_t i;
    pthread_once(&registry_once, registry_init);
    pthread_mutex_lock(&registry_lock);
    for (i = 0; i < SLOT_COUNT; ++i) {
        slot *s = &slots[i];
        if (s->kind == 0) {
            if (s->generation == 0) {
                s->generation = 1;
            }
            s->kind = kind;
            s->closing = 0;
            s->calls = 0;
            s->pid = getpid();
            s->owner = pthread_self();
            s->object = object;
            cam_error_clear(&s->last_error);
            *out = ((uintptr_t)s->generation << SLOT_BITS) | (uintptr_t)(i + 1u);
            pthread_mutex_unlock(&registry_lock);
            return KEEL_OK;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return KEEL_E_NO_MEMORY;
}

static slot *decode(const void *handle, uint32_t kind)
{
    uintptr_t token = (uintptr_t)handle;
    uintptr_t index = token & (((uintptr_t)1 << SLOT_BITS) - 1u);
    uintptr_t generation = token >> SLOT_BITS;
    slot *s;
    if (index == 0 || index > SLOT_COUNT || generation == 0 || generation > GENERATION_MASK) {
        return NULL;
    }
    s = &slots[index - 1u];
    if (s->kind != kind || s->generation != generation || s->closing) {
        return NULL;
    }
    return s;
}

/* Starts a call: checks the token, the process and (unless any_thread) the
 * owning thread, and counts the call. */
static int32_t enter(const void *handle, uint32_t kind, int any_thread, slot **out)
{
    slot *s;
    pthread_mutex_lock(&registry_lock);
    s = decode(handle, kind);
    if (s == NULL) {
        pthread_mutex_unlock(&registry_lock);
        return KEEL_E_INVALID_HANDLE;
    }
    if (s->pid != getpid()) {
        pthread_mutex_unlock(&registry_lock);
        return KEEL_E_NOT_OWNER;
    }
    if (!any_thread && !pthread_equal(s->owner, pthread_self())) {
        pthread_mutex_unlock(&registry_lock);
        return KEEL_E_CONFLICT;
    }
    s->calls++;
    pthread_mutex_unlock(&registry_lock);
    *out = s;
    return KEEL_OK;
}

static void leave(slot *s)
{
    pthread_mutex_lock(&registry_lock);
    s->calls--;
    if (s->closing && s->calls == 1u) {
        pthread_cond_broadcast(&registry_cond);
    }
    pthread_mutex_unlock(&registry_lock);
}

/* Starts a close: marks the slot so new calls fail, from any thread. */
static int32_t enter_close(const void *handle, uint32_t kind, slot **out)
{
    int32_t rc = enter(handle, kind, 1, out);
    if (rc != KEEL_OK) {
        return rc;
    }
    pthread_mutex_lock(&registry_lock);
    (*out)->closing = 1;
    pthread_mutex_unlock(&registry_lock);
    return KEEL_OK;
}

/* Waits for the other calls in flight, then frees the slot. */
static void retire(slot *s)
{
    pthread_mutex_lock(&registry_lock);
    while (s->calls > 1u) {
        pthread_cond_wait(&registry_cond, &registry_lock);
    }
    pthread_mutex_unlock(&registry_lock);
}

static void release_slot(slot *s)
{
    pthread_mutex_lock(&registry_lock);
    s->kind = 0;
    s->object = NULL;
    s->calls = 0;
    s->closing = 0;
    s->generation = (s->generation % GENERATION_MASK) + 1u;
    pthread_mutex_unlock(&registry_lock);
}

/* Records a failed call's error on the handle and returns its status. */
static int32_t record(slot *s, int32_t rc, const cam_error *error)
{
    if (rc != KEEL_OK) {
        s->last_error = *error;
        s->last_error.status = rc;
    }
    return rc;
}

/* ---- struct rules (those of keel.h) ------------------------------------- */

static int zero_tail(const void *data, uint32_t known, uint32_t size)
{
    const uint8_t *bytes = data;
    uint32_t i;
    for (i = known; i < size; ++i) {
        if (bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* KEEL_OK, KEEL_E_INVALID_ARGUMENT (size out of range) or KEEL_E_UNSUPPORTED
 * (a non-zero tail). */
static int32_t check_input(const void *data, uint32_t known)
{
    uint32_t size = *(const uint32_t *)data;
    if (size < known || size > KEEL_STRUCT_SIZE_MAX) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    return zero_tail(data, known, size) ? KEEL_OK : KEEL_E_UNSUPPORTED;
}

static int output_ok(const void *data, uint32_t known)
{
    uint32_t size;
    if (data == NULL) {
        return 0;
    }
    size = *(const uint32_t *)data;
    return size >= known && size <= KEEL_STRUCT_SIZE_MAX;
}

/* Writes known bytes of value (struct_size excluded) and zeroes the rest. */
static void output_write(void *data, const void *value, uint32_t known)
{
    uint32_t size = *(uint32_t *)data;
    memcpy((uint8_t *)data + sizeof(uint32_t), (const uint8_t *)value + sizeof(uint32_t), known - sizeof(uint32_t));
    memset((uint8_t *)data + known, 0, size - known);
}

static int32_t bad_argument(cam_error *error, const char *format, const char *what)
{
    return cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, NULL, format, what);
}

static void write_error(keel_camera_error *out, int32_t status, const cam_error *error)
{
    keel_camera_error value;
    memset(&value, 0, sizeof(value));
    value.status = status;
    if (status != KEEL_OK) {
        value.resource = error->resource;
        value.os_error = error->os_error;
        cam_copy_string(value.path, sizeof(value.path), error->path);
        cam_copy_string(value.message, sizeof(value.message), error->message);
    }
    output_write(out, &value, sizeof(value));
}

/* ---- sources ------------------------------------------------------------ */

static const cam_source_ops *source_ops(void)
{
#ifdef KEEL_CAMERA_TEST_HOOKS
    if (cam_test_selected()) {
        return &cam_test_source;
    }
#endif
    return &cam_libcamera_source;
}

static const cam_encoder_ops *encoder_ops(void)
{
#ifdef KEEL_CAMERA_TEST_HOOKS
    const char *encoder = getenv("KEEL_CAMERA_TEST_ENCODER");
    if (cam_test_selected() && (encoder == NULL || strcmp(encoder, "v4l2") != 0)) {
        return &cam_test_encoder;
    }
#endif
    return &cam_v4l2_encoder;
}

/* ---- metadata ----------------------------------------------------------- */

uint32_t keel_camera_abi_version(void)
{
    return KEEL_CAMERA_ABI_VERSION;
}

static char version_text[KEEL_STRING_MAX];
static pthread_once_t version_once = PTHREAD_ONCE_INIT;

static void build_version(void)
{
    snprintf(version_text, sizeof(version_text), "%d.%d.%d (%s; libcamera %s%s%s%s)", KEEL_CAMERA_VERSION_MAJOR,
             KEEL_CAMERA_VERSION_MINOR, KEEL_CAMERA_VERSION_PATCH, KEEL_CAMERA_BUILD_TRIPLE, cam_libcamera_version(),
             cam_jpeg_software_available() ? "; jpeg" : "", cam_libav_available() ? "; libav" : "",
#ifdef KEEL_CAMERA_TEST_HOOKS
             "; test-hooks"
#else
             ""
#endif
    );
}

const char *keel_camera_version_string(void)
{
    pthread_once(&version_once, build_version);
    return version_text;
}

int32_t keel_camera_convert_i420(const uint8_t *frame, const keel_camera_frame_info *layout, uint8_t *out,
                                 uint32_t out_size, uint32_t out_format, uint32_t out_stride)
{
    uint64_t y_end;
    uint64_t u_end;
    uint64_t v_end;
    uint64_t needed;
    uint32_t stride;
    if (frame == NULL || layout == NULL || out == NULL || layout->struct_size < sizeof(*layout) ||
        layout->struct_size > KEEL_STRUCT_SIZE_MAX) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    if (layout->format != KEEL_CAMERA_FORMAT_I420 || layout->plane_count != 3 || layout->width == 0 ||
        layout->height == 0 || (layout->width & 1u) || (layout->height & 1u) ||
        layout->stride0 < layout->width || layout->stride1 < layout->width / 2u ||
        layout->stride2 < layout->width / 2u) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    y_end = (uint64_t)layout->offset0 + (uint64_t)layout->stride0 * (layout->height - 1u) + layout->width;
    u_end = (uint64_t)layout->offset1 + (uint64_t)layout->stride1 * (layout->height / 2u - 1u) + layout->width / 2u;
    v_end = (uint64_t)layout->offset2 + (uint64_t)layout->stride2 * (layout->height / 2u - 1u) + layout->width / 2u;
    if (y_end > layout->bytes || u_end > layout->bytes || v_end > layout->bytes) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    if (out_format != KEEL_CAMERA_FORMAT_RGB888 && out_format != KEEL_CAMERA_FORMAT_BGR888) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    stride = out_stride != 0 ? out_stride : layout->width * 3u;
    if (stride < layout->width * 3u) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    needed = (uint64_t)stride * (layout->height - 1u) + (uint64_t)layout->width * 3u;
    if (out_size < needed) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    cam_convert_i420(frame + layout->offset0, layout->stride0, frame + layout->offset1, layout->stride1,
                     frame + layout->offset2, layout->stride2, layout->width, layout->height, out, stride,
                     out_format == KEEL_CAMERA_FORMAT_BGR888);
    return KEEL_OK;
}

/* ---- listing ------------------------------------------------------------ */

int32_t keel_camera_list_open(keel_camera_list **out_list, keel_camera_error *error)
{
    cam_error failure;
    camera_list *list;
    uintptr_t token;
    int32_t rc;
    if (error != NULL && !output_ok(error, sizeof(*error))) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    cam_error_clear(&failure);
    if (out_list == NULL) {
        rc = bad_argument(&failure, "%s must not be NULL", "out_list");
        if (error != NULL) {
            write_error(error, rc, &failure);
        }
        return rc;
    }
    *out_list = NULL;
    list = calloc(1, sizeof(*list));
    if (list == NULL) {
        rc = cam_fail(&failure, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    } else {
        rc = source_ops()->list(list->cameras, CAM_CAMERAS_MAX, &list->count, &failure);
    }
    if (rc == KEEL_OK && registry_add(KIND_LIST, list, &token) != KEEL_OK) {
        rc = cam_fail(&failure, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL,
                      "every handle slot is in use: close some handles");
    }
    if (rc != KEEL_OK) {
        free(list);
    } else {
        *out_list = (keel_camera_list *)token;
    }
    if (error != NULL) {
        write_error(error, rc, &failure);
    }
    return rc;
}

int32_t keel_camera_list_count(const keel_camera_list *handle, uint32_t *out_count)
{
    slot *s;
    int32_t rc = enter(handle, KIND_LIST, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (out_count == NULL) {
        rc = KEEL_E_INVALID_ARGUMENT;
    } else {
        *out_count = ((const camera_list *)s->object)->count;
    }
    leave(s);
    return rc;
}

int32_t keel_camera_list_get(const keel_camera_list *handle, uint32_t index, keel_camera_info *out)
{
    const camera_list *list;
    slot *s;
    int32_t rc = enter(handle, KIND_LIST, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    list = s->object;
    if (!output_ok(out, sizeof(*out))) {
        rc = KEEL_E_INVALID_ARGUMENT;
    } else if (index >= list->count) {
        keel_camera_info empty;
        memset(&empty, 0, sizeof(empty));
        output_write(out, &empty, sizeof(empty));
        rc = KEEL_E_NOT_FOUND;
    } else {
        output_write(out, &list->cameras[index].info, sizeof(*out));
    }
    leave(s);
    return rc;
}

int32_t keel_camera_list_get_mode(const keel_camera_list *handle, uint32_t index, uint32_t mode,
                                  keel_camera_mode *out)
{
    const camera_list *list;
    slot *s;
    int32_t rc = enter(handle, KIND_LIST, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    list = s->object;
    if (!output_ok(out, sizeof(*out))) {
        rc = KEEL_E_INVALID_ARGUMENT;
    } else if (index >= list->count || mode >= list->cameras[index].info.mode_count) {
        keel_camera_mode empty;
        memset(&empty, 0, sizeof(empty));
        output_write(out, &empty, sizeof(empty));
        rc = KEEL_E_NOT_FOUND;
    } else {
        output_write(out, &list->cameras[index].modes[mode], sizeof(*out));
    }
    leave(s);
    return rc;
}

int32_t keel_camera_list_close(keel_camera_list *handle)
{
    slot *s;
    int32_t rc;
    if (handle == NULL) {
        return KEEL_OK;
    }
    rc = enter_close(handle, KIND_LIST, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    retire(s);
    free(s->object);
    release_slot(s);
    return KEEL_OK;
}

/* ---- open and close ----------------------------------------------------- */

static int even_size(uint32_t value)
{
    return value == 0 || (value >= KEEL_CAMERA_SIZE_MIN && value <= KEEL_CAMERA_SIZE_MAX && (value & 1u) == 0);
}

static int32_t resolve_options(const keel_camera_options *o, cam_session_config *config, cam_error *error)
{
    memset(config, 0, sizeof(*config));
    if (!even_size(o->width) || !even_size(o->height)) {
        return bad_argument(error, "%s must be 0 or even, 64..16384", "width and height");
    }
    if (o->framerate_milli != 0 &&
        (o->framerate_milli < KEEL_CAMERA_FRAMERATE_MIN_MILLI || o->framerate_milli > KEEL_CAMERA_FRAMERATE_MAX_MILLI)) {
        return bad_argument(error, "%s must be 0 or 1000..240000 (fps x 1000)", "framerate_milli");
    }
    if (o->format > KEEL_CAMERA_FORMAT_BGR888) {
        return bad_argument(error, "%s must be a keel_camera_format", "format");
    }
    if (o->flags & ~(uint32_t)(KEEL_CAMERA_HFLIP | KEEL_CAMERA_VFLIP)) {
        return bad_argument(error, "%s has unknown bits", "flags");
    }
    if (o->rotation != 0 && o->rotation != 180) {
        return bad_argument(error, "%s must be 0 or 180", "rotation");
    }
    if (o->af_mode > KEEL_CAMERA_AF_CONTINUOUS || o->af_range > KEEL_CAMERA_AF_RANGE_FULL) {
        return bad_argument(error, "%s must be a keel_camera_af_mode and a keel_camera_af_range", "af_mode and af_range");
    }
    if (o->lens_position_milli != 0 &&
        (o->af_mode != KEEL_CAMERA_AF_MANUAL || o->lens_position_milli > LENS_POSITION_MAX)) {
        return bad_argument(error, "%s must be 0, or 0..32000 with KEEL_CAMERA_AF_MANUAL", "lens_position_milli");
    }
    if (o->exposure_us > EXPOSURE_MAX_US) {
        return bad_argument(error, "%s must be 0..1000000", "exposure_us");
    }
    if (o->gain_milli != 0 && (o->gain_milli < GAIN_MIN || o->gain_milli > GAIN_MAX)) {
        return bad_argument(error, "%s must be 0 or 1000..64000", "gain_milli");
    }
    if (o->ev_milli < -EV_MAX || o->ev_milli > EV_MAX) {
        return bad_argument(error, "%s must be -8000..8000", "ev_milli");
    }
    if (o->awb_mode > KEEL_CAMERA_AWB_MANUAL) {
        return bad_argument(error, "%s must be a keel_camera_awb", "awb_mode");
    }
    if (o->awb_mode == KEEL_CAMERA_AWB_MANUAL) {
        if (o->red_gain_milli == 0 || o->blue_gain_milli == 0 || o->red_gain_milli > COLOUR_GAIN_MAX ||
            o->blue_gain_milli > COLOUR_GAIN_MAX) {
            return bad_argument(error, "%s must both be 1..32000 with KEEL_CAMERA_AWB_MANUAL",
                                "red_gain_milli and blue_gain_milli");
        }
    } else if (o->red_gain_milli != 0 || o->blue_gain_milli != 0) {
        return bad_argument(error, "%s apply only with KEEL_CAMERA_AWB_MANUAL", "red_gain_milli and blue_gain_milli");
    }
    if (o->frame_queue > KEEL_CAMERA_FRAME_QUEUE_MAX) {
        return bad_argument(error, "%s must be 0..8", "frame_queue");
    }
    config->index = o->index;
    config->stream.width = o->width != 0 ? o->width : KEEL_CAMERA_WIDTH_DEFAULT;
    config->stream.height = o->height != 0 ? o->height : KEEL_CAMERA_HEIGHT_DEFAULT;
    config->stream.framerate_milli = o->framerate_milli != 0 ? o->framerate_milli : KEEL_CAMERA_FRAMERATE_DEFAULT_MILLI;
    config->stream.format = o->format != 0 ? o->format : KEEL_CAMERA_FORMAT_I420;
    /* A rotation by 180 degrees is both flips. */
    config->stream.hflip = ((o->flags & KEEL_CAMERA_HFLIP) != 0) ^ (o->rotation == 180);
    config->stream.vflip = ((o->flags & KEEL_CAMERA_VFLIP) != 0) ^ (o->rotation == 180);
    config->controls.af_mode = o->af_mode;
    config->controls.af_range = o->af_range;
    config->controls.lens_position_milli = o->lens_position_milli;
    config->controls.exposure_us = o->exposure_us;
    config->controls.gain_milli = o->gain_milli;
    config->controls.ev_milli = o->ev_milli;
    config->controls.awb_mode = o->awb_mode;
    config->controls.red_gain_milli = o->red_gain_milli;
    config->controls.blue_gain_milli = o->blue_gain_milli;
    config->frame_queue = o->frame_queue != 0 ? o->frame_queue : KEEL_CAMERA_FRAME_QUEUE_DEFAULT;
    return KEEL_OK;
}

int32_t keel_camera_open(const keel_camera_options *options, keel_camera **out_camera, keel_camera_error *error)
{
    keel_camera_options defaults;
    cam_session_config config;
    cam_session *session = NULL;
    cam_error failure;
    uintptr_t token;
    int32_t rc = KEEL_OK;
    if (error != NULL && !output_ok(error, sizeof(*error))) {
        return KEEL_E_INVALID_ARGUMENT;
    }
    cam_error_clear(&failure);
    if (out_camera == NULL) {
        rc = bad_argument(&failure, "%s must not be NULL", "out_camera");
    } else {
        *out_camera = NULL;
    }
    if (rc == KEEL_OK && options == NULL) {
        memset(&defaults, 0, sizeof(defaults));
        defaults.struct_size = sizeof(defaults);
        options = &defaults;
    }
    if (rc == KEEL_OK && check_input(options, sizeof(*options)) != KEEL_OK) {
        rc = bad_argument(&failure, "%s is below its 1.0 size, above KEEL_STRUCT_SIZE_MAX, or has a non-zero "
                                    "tail this library does not know", "options->struct_size");
    }
    if (rc == KEEL_OK) {
        rc = resolve_options(options, &config, &failure);
    }
    if (rc == KEEL_OK) {
        rc = cam_session_open(source_ops(), encoder_ops(), &config, &session, &failure);
    }
    if (rc == KEEL_OK && registry_add(KIND_CAMERA, session, &token) != KEEL_OK) {
        cam_session_close(session);
        rc = cam_fail(&failure, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL,
                      "every handle slot is in use: close some handles");
    }
    if (rc == KEEL_OK) {
        *out_camera = (keel_camera *)token;
    }
    if (error != NULL) {
        write_error(error, rc, &failure);
    }
    return rc;
}

int32_t keel_camera_close(keel_camera *camera)
{
    slot *s;
    int32_t rc;
    if (camera == NULL) {
        return KEEL_OK;
    }
    rc = enter_close(camera, KIND_CAMERA, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_session_wake(s->object);
    retire(s);
    rc = cam_session_close(s->object);
    release_slot(s);
    return rc;
}

/* ---- handle calls ------------------------------------------------------- */

int32_t keel_camera_get_info(const keel_camera *camera, keel_camera_info *out)
{
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!output_ok(out, sizeof(*out))) {
        rc = KEEL_E_INVALID_ARGUMENT;
    } else {
        output_write(out, &cam_session_desc(s->object)->info, sizeof(*out));
    }
    leave(s);
    return rc;
}

int32_t keel_camera_get_stream(const keel_camera *camera, keel_camera_stream_info *out)
{
    keel_camera_stream_info value;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!output_ok(out, sizeof(*out))) {
        rc = KEEL_E_INVALID_ARGUMENT;
    } else {
        memset(&value, 0, sizeof(value));
        cam_session_stream_info(s->object, &value);
        output_write(out, &value, sizeof(value));
    }
    leave(s);
    return rc;
}

int32_t keel_camera_get_stats(const keel_camera *camera, keel_camera_stats *out)
{
    keel_camera_stats value;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!output_ok(out, sizeof(*out))) {
        rc = KEEL_E_INVALID_ARGUMENT;
    } else {
        memset(&value, 0, sizeof(value));
        cam_session_stats(s->object, &value);
        output_write(out, &value, sizeof(value));
    }
    leave(s);
    return rc;
}

int32_t keel_camera_last_error(const keel_camera *camera, keel_camera_error *out)
{
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (!output_ok(out, sizeof(*out))) {
        rc = KEEL_E_INVALID_ARGUMENT;
    } else {
        cam_error failure;
        /* A capture failure since the last call counts as the latest. */
        if (cam_session_take_failure(s->object, &failure)) {
            s->last_error = failure;
        }
        write_error(out, s->last_error.status, &s->last_error);
    }
    leave(s);
    return rc;
}

static int32_t resolve_encoder(const cam_session_config *config, const keel_camera_encoder_options *o,
                               cam_session_encoder *out, cam_error *error)
{
    const cam_stream *s = &config->stream;
    memset(out, 0, sizeof(*out));
    if (o->codec > KEEL_CAMERA_CODEC_MJPEG) {
        return bad_argument(error, "%s must be a keel_camera_codec", "codec");
    }
    if (o->encoded_queue > KEEL_CAMERA_ENCODED_QUEUE_MAX) {
        return bad_argument(error, "%s must be 0..256", "encoded_queue");
    }
    if (o->codec == KEEL_CAMERA_CODEC_H264) {
        if (o->bitrate != 0 && (o->bitrate < KEEL_CAMERA_BITRATE_MIN || o->bitrate > KEEL_CAMERA_BITRATE_MAX)) {
            return bad_argument(error, "%s must be 0 or 25000..25000000", "bitrate");
        }
        if (o->intra_period > INTRA_MAX || o->profile > KEEL_CAMERA_H264_PROFILE_HIGH) {
            return bad_argument(error, "%s must be 0..1000 and a keel_camera_h264_profile", "intra_period and profile");
        }
        if (o->quality != 0) {
            return bad_argument(error, "%s applies only to MJPEG", "quality");
        }
        if (o->flags & ~(uint32_t)KEEL_CAMERA_INLINE_HEADERS) {
            return bad_argument(error, "%s has unknown bits", "flags");
        }
    } else if (o->bitrate != 0 || o->intra_period != 0 || o->profile != 0 || o->flags != 0) {
        return bad_argument(error, "%s apply only to H.264", "bitrate, intra_period, profile and flags");
    } else if (o->quality > 100) {
        return bad_argument(error, "%s must be 0..100", "quality");
    }
    out->codec = o->codec;
    out->flags = o->flags;
    out->profile = o->profile;
    out->bitrate = o->bitrate != 0 ? o->bitrate : cam_default_bitrate(s->width, s->height, s->framerate_milli);
    out->intra_period = o->intra_period != 0 ? o->intra_period : (s->framerate_milli + 500u) / 1000u;
    if (out->intra_period == 0) {
        out->intra_period = 1;
    }
    out->quality = o->quality != 0 ? o->quality : KEEL_CAMERA_QUALITY_DEFAULT;
    out->encoded_queue = o->encoded_queue != 0 ? o->encoded_queue : KEEL_CAMERA_ENCODED_QUEUE_DEFAULT;
    if (o->codec != KEEL_CAMERA_CODEC_H264) {
        out->bitrate = 0;
        out->intra_period = 0;
    }
    return KEEL_OK;
}

int32_t keel_camera_set_encoder(keel_camera *camera, const keel_camera_encoder_options *options)
{
    cam_session_encoder encoder;
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    if (options == NULL) {
        rc = bad_argument(&failure, "%s must not be NULL", "options");
    } else if ((rc = check_input(options, sizeof(*options))) != KEEL_OK) {
        cam_fail(&failure, rc, KEEL_CAMERA_RESOURCE_OPTIONS, 0, NULL, "%s",
                 rc == KEEL_E_UNSUPPORTED ? "options has a non-zero tail this library does not know"
                                          : "options->struct_size is out of range");
    } else {
        rc = resolve_encoder(cam_session_config_of(s->object), options, &encoder, &failure);
        if (rc == KEEL_OK) {
            rc = cam_session_set_encoder(s->object, &encoder, &failure);
        }
    }
    rc = record(s, rc, &failure);
    leave(s);
    return rc;
}

int32_t keel_camera_start(keel_camera *camera)
{
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    rc = record(s, cam_session_start(s->object, &failure), &failure);
    leave(s);
    return rc;
}

int32_t keel_camera_stop(keel_camera *camera)
{
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_session_stop(s->object);
    leave(s);
    return KEEL_OK;
}

int32_t keel_camera_read_frame(keel_camera *camera, uint8_t *buffer, uint32_t size, uint32_t timeout_ms,
                               keel_camera_frame_info *out_info)
{
    keel_camera_frame_info value;
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    if (buffer == NULL || !output_ok(out_info, sizeof(*out_info)) || timeout_ms > KEEL_CAMERA_TIMEOUT_MAX_MS) {
        rc = bad_argument(&failure, "%s", "buffer and out_info must not be NULL, out_info->struct_size must be "
                                          "valid and timeout_ms at most 60000");
    } else {
        memset(&value, 0, sizeof(value));
        rc = cam_session_read_frame(s->object, buffer, size, timeout_ms, &value, &failure);
        if (rc != KEEL_E_INVALID_ARGUMENT && rc != KEEL_E_INVALID_HANDLE) {
            output_write(out_info, &value, sizeof(value));
        }
    }
    rc = record(s, rc, &failure);
    leave(s);
    return rc;
}

int32_t keel_camera_read_encoded(keel_camera *camera, uint8_t *buffer, uint32_t size, uint32_t timeout_ms,
                                 keel_camera_encoded_info *out_info)
{
    keel_camera_encoded_info value;
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    if (buffer == NULL || !output_ok(out_info, sizeof(*out_info)) || timeout_ms > KEEL_CAMERA_TIMEOUT_MAX_MS) {
        rc = bad_argument(&failure, "%s", "buffer and out_info must not be NULL, out_info->struct_size must be "
                                          "valid and timeout_ms at most 60000");
    } else {
        memset(&value, 0, sizeof(value));
        rc = cam_session_read_encoded(s->object, buffer, size, timeout_ms, &value, &failure);
        if (rc != KEEL_E_INVALID_ARGUMENT && rc != KEEL_E_INVALID_HANDLE) {
            output_write(out_info, &value, sizeof(value));
        }
    }
    rc = record(s, rc, &failure);
    leave(s);
    return rc;
}

int32_t keel_camera_trigger_autofocus(keel_camera *camera, uint32_t timeout_ms, uint32_t *out_af_state)
{
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    if (out_af_state == NULL || timeout_ms == 0 || timeout_ms > KEEL_CAMERA_TIMEOUT_MAX_MS) {
        rc = bad_argument(&failure, "%s", "out_af_state must not be NULL and timeout_ms must be 1..60000");
    } else {
        rc = cam_session_autofocus(s->object, timeout_ms, out_af_state, &failure);
    }
    rc = record(s, rc, &failure);
    leave(s);
    return rc;
}

/* ---- outputs ------------------------------------------------------------ */

int32_t keel_camera_output_start(keel_camera *camera, const char *url, const keel_camera_output_options *options,
                                 uint32_t *out_output)
{
    keel_camera_output_options value;
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    memset(&value, 0, sizeof(value));
    value.struct_size = sizeof(value);
    if (url == NULL || out_output == NULL || strnlen(url, KEEL_CAMERA_PATH_MAX) == KEEL_CAMERA_PATH_MAX) {
        rc = bad_argument(&failure, "%s", "url and out_output must not be NULL, and url must be shorter than 256 bytes");
    } else if (options != NULL && (rc = check_input(options, sizeof(*options))) != KEEL_OK) {
        cam_fail(&failure, rc, KEEL_CAMERA_RESOURCE_OPTIONS, 0, url, "%s",
                 rc == KEEL_E_UNSUPPORTED ? "options has a non-zero tail this library does not know"
                                          : "options->struct_size is out of range");
    } else {
        if (options != NULL) {
            memcpy(&value, options, sizeof(value));
        }
        if (value.flags != 0 || value.reserved0 != 0) {
            rc = bad_argument(&failure, "%s must be 0", "flags and reserved0");
        } else if (value.queue > KEEL_CAMERA_ENCODED_QUEUE_MAX || value.max_clients > KEEL_CAMERA_CLIENTS_MAX) {
            rc = bad_argument(&failure, "%s", "queue must be 0..256 and max_clients 0..16");
        } else if (value.datagram_bytes != 0 &&
                   (value.datagram_bytes < KEEL_CAMERA_DATAGRAM_MIN || value.datagram_bytes > KEEL_CAMERA_DATAGRAM_MAX)) {
            rc = bad_argument(&failure, "%s must be 0 or 512..65507", "datagram_bytes");
        } else {
            rc = cam_session_output_start(s->object, url, &value, out_output, &failure);
        }
    }
    rc = record(s, rc, &failure);
    leave(s);
    return rc;
}

int32_t keel_camera_output_stop(keel_camera *camera, uint32_t output)
{
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    rc = record(s, cam_session_output_stop(s->object, output, &failure), &failure);
    leave(s);
    return rc;
}

int32_t keel_camera_get_output_stats(const keel_camera *camera, uint32_t output, keel_camera_output_stats *out)
{
    keel_camera_output_stats value;
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    if (!output_ok(out, sizeof(*out))) {
        rc = KEEL_E_INVALID_ARGUMENT;
    } else {
        memset(&value, 0, sizeof(value));
        rc = cam_session_output_stats(s->object, output, &value, &failure);
        output_write(out, &value, sizeof(value));
    }
    leave(s);
    return rc;
}

/* ---- stills ------------------------------------------------------------- */

static int32_t resolve_still(const keel_camera_still_options *options, cam_still_request *request,
                             cam_error *error)
{
    keel_camera_still_options value;
    memset(&value, 0, sizeof(value));
    if (options != NULL) {
        int32_t rc = check_input(options, sizeof(*options));
        if (rc != KEEL_OK) {
            return cam_fail(error, rc, KEEL_CAMERA_RESOURCE_OPTIONS, 0, NULL, "%s",
                            rc == KEEL_E_UNSUPPORTED ? "options has a non-zero tail this library does not know"
                                                     : "options->struct_size is out of range");
        }
        memcpy(&value, options, sizeof(value));
    }
    if (!even_size(value.width) || !even_size(value.height) || ((value.width == 0) != (value.height == 0))) {
        return bad_argument(error, "%s must both be 0 (full resolution) or both even, 64..16384", "width and height");
    }
    if (value.quality > 100 || value.timeout_ms > KEEL_CAMERA_TIMEOUT_MAX_MS || value.reserved0 != 0) {
        return bad_argument(error, "%s", "quality must be 0..100, timeout_ms 0..60000 and reserved0 0");
    }
    request->width = value.width;
    request->height = value.height;
    request->quality = value.quality != 0 ? value.quality : KEEL_CAMERA_QUALITY_DEFAULT;
    request->timeout_ms = value.timeout_ms != 0 ? value.timeout_ms : KEEL_CAMERA_STILL_TIMEOUT_DEFAULT_MS;
    request->buffer = NULL;
    request->size = 0;
    request->fd = -1;
    return KEEL_OK;
}

int32_t keel_camera_capture_still(keel_camera *camera, const keel_camera_still_options *options, uint8_t *buffer,
                                  uint32_t size, keel_camera_still_info *out_info)
{
    keel_camera_still_info value;
    cam_still_request request;
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    if (buffer == NULL || !output_ok(out_info, sizeof(*out_info))) {
        rc = bad_argument(&failure, "%s", "buffer and out_info must not be NULL, and out_info->struct_size valid");
    } else if ((rc = resolve_still(options, &request, &failure)) == KEEL_OK) {
        request.buffer = buffer;
        request.size = size;
        memset(&value, 0, sizeof(value));
        rc = cam_session_still(s->object, &request, &value, &failure);
        if (rc != KEEL_E_INVALID_ARGUMENT) {
            output_write(out_info, &value, sizeof(value));
        }
    }
    rc = record(s, rc, &failure);
    leave(s);
    return rc;
}

int32_t keel_camera_capture_still_file(keel_camera *camera, const keel_camera_still_options *options,
                                       const char *path, keel_camera_still_info *out_info)
{
    keel_camera_still_info value;
    cam_still_request request;
    cam_error failure;
    slot *s;
    int32_t rc = enter(camera, KIND_CAMERA, 0, &s);
    if (rc != KEEL_OK) {
        return rc;
    }
    cam_error_clear(&failure);
    if (path == NULL || path[0] == '\0' || strnlen(path, KEEL_CAMERA_PATH_MAX) == KEEL_CAMERA_PATH_MAX ||
        (out_info != NULL && !output_ok(out_info, sizeof(*out_info)))) {
        rc = bad_argument(&failure, "%s", "path must be a file name shorter than 256 bytes, and out_info NULL or "
                                          "with a valid struct_size");
    } else if ((rc = resolve_still(options, &request, &failure)) == KEEL_OK) {
        char temporary[KEEL_CAMERA_PATH_MAX + 16];
        /* Written next to the target and renamed: the file is complete or absent. */
        snprintf(temporary, sizeof(temporary), "%s.part", path);
        request.fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (request.fd < 0) {
            int os_error = errno;
            rc = cam_fail(&failure, cam_status_for_errno(os_error), KEEL_CAMERA_RESOURCE_FILE, os_error, path,
                          "cannot create %s: %s", path, strerror(os_error));
        } else {
            memset(&value, 0, sizeof(value));
            rc = cam_session_still(s->object, &request, &value, &failure);
            if (close(request.fd) != 0 && rc == KEEL_OK) {
                int os_error = errno;
                rc = cam_fail(&failure, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_FILE, os_error, path,
                              "writing %s failed: %s", path, strerror(os_error));
            }
            if (rc == KEEL_OK && rename(temporary, path) != 0) {
                int os_error = errno;
                rc = cam_fail(&failure, cam_status_for_errno(os_error), KEEL_CAMERA_RESOURCE_FILE, os_error, path,
                              "cannot create %s: %s", path, strerror(os_error));
            }
            if (rc != KEEL_OK) {
                unlink(temporary);
            }
            if (out_info != NULL && rc != KEEL_E_INVALID_ARGUMENT) {
                output_write(out_info, &value, sizeof(value));
            }
        }
    }
    rc = record(s, rc, &failure);
    leave(s);
    return rc;
}
