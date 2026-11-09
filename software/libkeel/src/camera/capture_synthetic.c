/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * The synthetic camera of test builds (KEEL_CAMERA_TEST_HOOKS=ON), selected
 * with KEEL_CAMERA_TEST_SOURCE=1 (SPEC section 26.9). Test-only: it lets
 * this library's tests and the bindings' tests run every path above the
 * capture source without a camera.
 *
 * Camera 0 is an IMX219 (Camera Module 2, fixed focus), camera 1 an IMX708
 * (Camera Module 3, autofocus), with their real sensor modes. Frames carry
 * a pattern every test can recompute; padding bytes are 0xA5:
 *   I420:   Y = (7x + 3y + n) & 0xFF at pixel (x, y);
 *           U = (5x + n) & 0xFF and V = (5y + 2n) & 0xFF at chroma sample (x, y)
 *   RGB888: R = (x + n) & 0xFF, G = (y + n) & 0xFF, B = (x + y) & 0xFF
 *           (BGR888 stores the same values as B, G, R)
 * Positions are those of the image before HFLIP/VFLIP (a flipped frame shows
 * the pattern mirrored); n is the low byte of the sequence number. Strides
 * are rounded up to 64 bytes, as on the Raspberry Pi ISP.
 *
 * KEEL_CAMERA_TEST_FAULT injects one failure:
 *   no-camera        nothing is connected
 *   permission       /dev/media0 is not accessible
 *   ipa-missing      the camera is connected but the IPA module is missing
 *   busy             another process holds the camera
 *   stall            the camera stops after 3 frames
 *   capture-fail     the camera fails after 3 frames
 *   encoder-missing  no hardware encoder
 *   encoder-fail     the encoder fails after 3 chunks
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "camera/camera_internal.h"

enum {
    TEST_CAMERAS = 2,
    TEST_BUFFERS_MAX = KEEL_CAMERA_FRAME_QUEUE_MAX + CAM_EXTRA_BUFFERS + 4,
    STRIDE_ALIGN = 64,
    FAULT_AFTER = 3
};

typedef struct test_model {
    const char *id;
    const char *sensor;
    uint32_t autofocus;
    uint32_t width;
    uint32_t height;
    uint32_t mode_count;
    keel_camera_mode modes[8];
} test_model;

/* The modes libcamera 0.7 reports for these sensors on a Raspberry Pi 4. */
#define MODE(w, h, depth, fps, cx, cy, cw, ch) {sizeof(keel_camera_mode), w, h, depth, fps, cx, cy, cw, ch, 0}
static const test_model models[TEST_CAMERAS] = {
    {"/base/soc/i2c0mux/i2c@1/imx219@10", "imx219", 0, 3280, 2464, 8,
     {MODE(640, 480, 8, 200160, 1000, 752, 1280, 960), MODE(1640, 1232, 8, 81070, 0, 0, 3280, 2464),
      MODE(1920, 1080, 8, 47570, 680, 692, 1920, 1080), MODE(3280, 2464, 8, 21190, 0, 0, 3280, 2464),
      MODE(640, 480, 10, 200160, 1000, 752, 1280, 960), MODE(1640, 1232, 10, 81070, 0, 0, 3280, 2464),
      MODE(1920, 1080, 10, 47570, 680, 692, 1920, 1080), MODE(3280, 2464, 10, 21190, 0, 0, 3280, 2464)}},
    {"/base/soc/i2c0mux/i2c@1/imx708@1a", "imx708", 1, 4608, 2592, 3,
     {MODE(1536, 864, 10, 120130, 768, 432, 3072, 1728), MODE(2304, 1296, 10, 56030, 0, 0, 4608, 2592),
      MODE(4608, 2592, 10, 14350, 0, 0, 4608, 2592)}},
};
#undef MODE

static pthread_mutex_t acquired_lock = PTHREAD_MUTEX_INITIALIZER;
static int acquired[TEST_CAMERAS];

typedef struct test_source {
    cam_source base;
    uint32_t index;
    const test_model *model;
    cam_stream stream;
    cam_controls controls;
    cam_frame frames[TEST_BUFFERS_MAX];
    uint8_t *memory[TEST_BUFFERS_MAX];
    int busy[TEST_BUFFERS_MAX]; /* under lock: the pipeline holds the frame */
    uint32_t buffer_count;
    pthread_t thread;
    int thread_started;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int stop;                   /* under lock */
    uint64_t sequence;
    cam_frame_sink sink;
    cam_failure_sink failure;
    void *context;
} test_source;

int cam_test_selected(void)
{
    const char *value = getenv("KEEL_CAMERA_TEST_SOURCE");
    return value != NULL && strcmp(value, "1") == 0;
}

int cam_test_fault(const char *name)
{
    const char *value = getenv("KEEL_CAMERA_TEST_FAULT");
    return value != NULL && strcmp(value, name) == 0;
}

/* Faults that make the camera unusable, as libcamera and the diagnosis report them. */
static int32_t setup_fault(cam_error *error)
{
    if (cam_test_fault("permission")) {
        return cam_fail_permission(error, KEEL_CAMERA_RESOURCE_DEVICE_NODE, EACCES, "/dev/media0");
    }
    if (cam_test_fault("ipa-missing")) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_IPA, ENOENT, "/usr/lib/libcamera/ipa",
                        "the imx219 camera is connected, but libcamera's Raspberry Pi IPA module "
                        "(ipa_rpi_vc4.so, ipa_rpi_pisp.so) is not in /usr/lib/libcamera/ipa: install it with "
                        "libcamera (Debian and Ubuntu: sudo apt install libcamera-ipa)");
    }
    return KEEL_OK;
}

static void describe(uint32_t index, cam_camera_desc *desc)
{
    const test_model *model = &models[index];
    memset(desc, 0, sizeof(*desc));
    desc->info.struct_size = sizeof(desc->info);
    desc->info.index = index;
    desc->info.flags = model->autofocus ? KEEL_CAMERA_INFO_AUTOFOCUS : 0u;
    desc->info.mode_count = model->mode_count;
    desc->info.pixel_array_width = model->width;
    desc->info.pixel_array_height = model->height;
    cam_copy_string(desc->info.id, sizeof(desc->info.id), model->id);
    cam_copy_string(desc->info.sensor, sizeof(desc->info.sensor), model->sensor);
    cam_copy_string(desc->info.model, sizeof(desc->info.model), cam_model_name(model->sensor));
    memcpy(desc->modes, model->modes, sizeof(model->modes));
}

static int32_t test_list(cam_camera_desc *cameras, uint32_t capacity, uint32_t *count, cam_error *error)
{
    uint32_t i;
    int32_t rc = setup_fault(error);
    *count = 0;
    if (rc != KEEL_OK || cam_test_fault("no-camera")) {
        return rc;
    }
    for (i = 0; i < TEST_CAMERAS && i < capacity; ++i) {
        describe(i, &cameras[i]);
        pthread_mutex_lock(&acquired_lock);
        if (acquired[i] || cam_test_fault("busy")) {
            /* Listed, but its modes could not be probed. */
            uint32_t m;
            cameras[i].info.flags |= KEEL_CAMERA_INFO_IN_USE;
            for (m = 0; m < cameras[i].info.mode_count; ++m) {
                cameras[i].modes[m].max_framerate_milli = 0;
                cameras[i].modes[m].crop_x = cameras[i].modes[m].crop_y = 0;
                cameras[i].modes[m].crop_width = cameras[i].modes[m].crop_height = 0;
            }
        }
        pthread_mutex_unlock(&acquired_lock);
        (*count)++;
    }
    return KEEL_OK;
}

static int32_t test_open(uint32_t index, cam_source **out, cam_camera_desc *desc, cam_error *error)
{
    test_source *source;
    int32_t rc = setup_fault(error);
    *out = NULL;
    if (rc != KEEL_OK) {
        return rc;
    }
    if (cam_test_fault("no-camera")) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_CAMERA, ENODEV, NULL,
                        "no camera detected: with the power off, check the ribbon cable at the Raspberry "
                        "Pi's camera connector, and that config.txt enables the camera (camera_auto_detect=1, "
                        "or dtoverlay=imx219 or dtoverlay=imx708)");
    }
    if (index >= TEST_CAMERAS) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_CAMERA, ENODEV, NULL,
                        "there is no camera %u: %u camera(s) connected", index, (unsigned)TEST_CAMERAS);
    }
    describe(index, desc);
    pthread_mutex_lock(&acquired_lock);
    if (acquired[index] || cam_test_fault("busy")) {
        pthread_mutex_unlock(&acquired_lock);
        return cam_fail(error, KEEL_E_IN_USE, KEEL_CAMERA_RESOURCE_CAMERA, EBUSY, NULL,
                        "camera %u (%s) is in use by another process or handle; only one can use a camera at "
                        "a time", index, models[index].sensor);
    }
    source = calloc(1, sizeof(*source));
    if (source == NULL) {
        pthread_mutex_unlock(&acquired_lock);
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    acquired[index] = 1;
    pthread_mutex_unlock(&acquired_lock);
    source->base.ops = &cam_test_source;
    source->index = index;
    source->model = &models[index];
    pthread_mutex_init(&source->lock, NULL);
    cam_cond_init(&source->cond);
    *out = &source->base;
    return KEEL_OK;
}

static int32_t test_configure(cam_source *base, cam_stream *stream, cam_error *error)
{
    test_source *source = (test_source *)base;
    (void)error;
    /* Like the ISP: never larger than the sensor. */
    if (stream->width > source->model->width) {
        stream->width = source->model->width;
    }
    if (stream->height > source->model->height) {
        stream->height = source->model->height;
    }
    cam_stream_layout(stream, STRIDE_ALIGN);
    return KEEL_OK;
}

static void fill(const test_source *source, uint8_t *data, uint64_t n)
{
    const cam_stream *s = &source->stream;
    uint32_t x;
    uint32_t y;
    uint32_t seq = (uint32_t)(n & 0xFFu);
    memset(data, 0xA5, s->frame_bytes);
    for (y = 0; y < s->height; ++y) {
        uint32_t sy = s->vflip ? s->height - 1u - y : y;
        uint8_t *row = data + (size_t)y * s->stride[0];
        for (x = 0; x < s->width; ++x) {
            uint32_t sx = s->hflip ? s->width - 1u - x : x;
            if (s->format == KEEL_CAMERA_FORMAT_I420) {
                row[x] = (uint8_t)((7u * sx + 3u * sy + seq) & 0xFFu);
            } else {
                uint8_t r = (uint8_t)((sx + seq) & 0xFFu);
                uint8_t g = (uint8_t)((sy + seq) & 0xFFu);
                uint8_t b = (uint8_t)((sx + sy) & 0xFFu);
                uint8_t *pixel = row + 3u * x;
                pixel[0] = s->format == KEEL_CAMERA_FORMAT_BGR888 ? b : r;
                pixel[1] = g;
                pixel[2] = s->format == KEEL_CAMERA_FORMAT_BGR888 ? r : b;
            }
        }
    }
    if (s->format != KEEL_CAMERA_FORMAT_I420) {
        return;
    }
    for (y = 0; y < s->height / 2u; ++y) {
        uint32_t sy = s->vflip ? s->height / 2u - 1u - y : y;
        uint8_t *u = data + s->offset[1] + (size_t)y * s->stride[1];
        uint8_t *v = data + s->offset[2] + (size_t)y * s->stride[2];
        for (x = 0; x < s->width / 2u; ++x) {
            uint32_t sx = s->hflip ? s->width / 2u - 1u - x : x;
            u[x] = (uint8_t)((5u * sx + seq) & 0xFFu);
            v[x] = (uint8_t)((5u * sy + 2u * seq) & 0xFFu);
        }
    }
}

static void *test_main(void *arg)
{
    test_source *source = arg;
    struct timespec next;
    /* Stills ask for no frame rate: they run at 30 fps. */
    uint32_t framerate = source->stream.framerate_milli != 0 ? source->stream.framerate_milli : 30000u;
    uint64_t period_ns = 1000000000000ull / framerate;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        uint32_t i;
        cam_frame *frame = NULL;
        uint64_t n;
        uint64_t at = (uint64_t)next.tv_sec * 1000000000u + (uint64_t)next.tv_nsec + period_ns;
        next.tv_sec = (time_t)(at / 1000000000u);
        next.tv_nsec = (long)(at % 1000000000u);
        pthread_mutex_lock(&source->lock);
        while (!source->stop &&
               pthread_cond_timedwait(&source->cond, &source->lock, &next) != ETIMEDOUT) {
        }
        if (source->stop) {
            pthread_mutex_unlock(&source->lock);
            break;
        }
        n = source->sequence++;
        if ((cam_test_fault("stall") || cam_test_fault("capture-fail")) && n >= FAULT_AFTER) {
            pthread_mutex_unlock(&source->lock);
            if (cam_test_fault("capture-fail") && n == FAULT_AFTER) {
                cam_error error;
                cam_error_clear(&error);
                cam_fail(&error, KEEL_E_PERIPHERAL, KEEL_CAMERA_RESOURCE_CAMERA, EIO, NULL,
                         "the camera stopped delivering frames (test fault)");
                source->failure(source->context, &error);
            }
            continue;
        }
        for (i = 0; i < source->buffer_count; ++i) {
            if (!source->busy[i]) {
                source->busy[i] = 1;
                frame = &source->frames[i];
                break;
            }
        }
        pthread_mutex_unlock(&source->lock);
        if (frame == NULL) {
            continue; /* every buffer is held: the sensor drops this frame */
        }
        fill(source, frame->data, n);
        frame->sequence = n;
        frame->timestamp_ns = cam_now_ns();
        cam_frame_deliver(frame);
        source->sink(source->context, frame);
    }
    return NULL;
}

static int32_t test_start(cam_source *base, const cam_stream *stream, const cam_controls *controls,
                          cam_frame_sink sink, cam_failure_sink failure, void *context, cam_error *error)
{
    test_source *source = (test_source *)base;
    uint32_t i;
    int rc;
    source->stream = *stream;
    source->controls = *controls;
    source->sink = sink;
    source->failure = failure;
    source->context = context;
    source->buffer_count = stream->buffer_count < TEST_BUFFERS_MAX ? stream->buffer_count : TEST_BUFFERS_MAX;
    source->stop = 0;
    for (i = 0; i < source->buffer_count; ++i) {
        source->memory[i] = malloc(stream->frame_bytes);
        if (source->memory[i] == NULL) {
            source->buffer_count = i;
            source->base.ops->free_buffers(base);
            return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
        }
        source->frames[i].source = base;
        source->frames[i].index = i;
        source->frames[i].data = source->memory[i];
        source->frames[i].bytes = stream->frame_bytes;
        source->frames[i].fd = -1;
        source->frames[i].fd_length = 0;
        source->busy[i] = 0;
    }
    rc = cam_thread_start(&source->thread, "keel-cam-test", test_main, source);
    if (rc != 0) {
        source->base.ops->free_buffers(base);
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_THREAD, rc, NULL,
                        "cannot start the capture thread: %s", strerror(rc));
    }
    source->thread_started = 1;
    return KEEL_OK;
}

static void test_release(cam_source *base, cam_frame *frame)
{
    test_source *source = (test_source *)base;
    pthread_mutex_lock(&source->lock);
    source->busy[frame->index] = 0;
    pthread_mutex_unlock(&source->lock);
}

static int32_t test_autofocus(cam_source *base, uint32_t timeout_ms, uint32_t *state, cam_error *error)
{
    test_source *source = (test_source *)base;
    struct timespec pause = {0, 50000000L};
    if (!source->model->autofocus || source->controls.af_mode == KEEL_CAMERA_AF_MANUAL) {
        return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_CAMERA, 0, NULL,
                        source->model->autofocus ? "the lens is in manual mode" : "this camera has no autofocus");
    }
    if (timeout_ms < 50u) {
        *state = KEEL_CAMERA_AF_STATE_SCANNING;
        return KEEL_OK;
    }
    nanosleep(&pause, NULL); /* a scan takes 50 ms here */
    *state = KEEL_CAMERA_AF_STATE_FOCUSED;
    return KEEL_OK;
}

static void test_stop(cam_source *base)
{
    test_source *source = (test_source *)base;
    if (!source->thread_started) {
        return;
    }
    pthread_mutex_lock(&source->lock);
    source->stop = 1;
    pthread_cond_broadcast(&source->cond);
    pthread_mutex_unlock(&source->lock);
    pthread_join(source->thread, NULL);
    source->thread_started = 0;
}

static void test_free_buffers(cam_source *base)
{
    test_source *source = (test_source *)base;
    uint32_t i;
    for (i = 0; i < source->buffer_count; ++i) {
        free(source->memory[i]);
        source->memory[i] = NULL;
    }
    source->buffer_count = 0;
}

static void test_close(cam_source *base)
{
    test_source *source = (test_source *)base;
    test_stop(base);
    test_free_buffers(base);
    pthread_mutex_lock(&acquired_lock);
    acquired[source->index] = 0;
    pthread_mutex_unlock(&acquired_lock);
    pthread_cond_destroy(&source->cond);
    pthread_mutex_destroy(&source->lock);
    free(source);
}

const cam_source_ops cam_test_source = {
    "test", test_list, test_open, test_configure, test_start, test_release,
    test_autofocus, test_stop, test_free_buffers, test_close,
};
