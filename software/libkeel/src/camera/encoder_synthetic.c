/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * The synthetic encoder of test builds (KEEL_CAMERA_TEST_HOOKS=ON), used
 * with the synthetic camera (SPEC section 26.9). Test-only. It behaves like
 * the hardware encoders towards the pipeline (its own thread, at most
 * CAM_ENCODER_PENDING waiting frames, the same limits) and writes bitstreams
 * whose structure tests can check:
 *   H.264  a keyframe every intra period (a forced keyframe starts a new
 *          period): a real SPS and PPS for the frame size, then an IDR
 *          slice NAL; other frames a non-IDR slice NAL. Slice payloads are 0x88 followed by the frame
 *          sequence number, 7 bits per byte with the top bit set (no byte is
 *          ever 00).
 *   MJPEG  SOI, COM "keel-test frame N WxH", DQT, SOF0 (the frame size),
 *          SOS, entropy-coded bytes with an FF 00 stuffing and an FF D0
 *          restart, EOI.
 * It refuses frames above 1920x1920, like the hardware.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera/camera_internal.h"

enum {
    TEST_MAX = KEEL_CAMERA_ENCODER_SIZE_MAX,
    CHUNK_MAX = 1024,
    FAIL_AFTER = 3
};

typedef struct test_encoder {
    cam_encoder base;
    cam_encoder_config config;
    cam_packet_sink sink;
    cam_encoder_failure failure;
    void *context;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int stop;
    cam_frame *pending[CAM_ENCODER_PENDING];
    uint32_t pending_count;
    _Atomic int force_keyframe;
    uint64_t encoded;
    uint32_t since_keyframe;
} test_encoder;

/* ---- bit writer for the SPS and PPS ------------------------------------- */

typedef struct bits {
    uint8_t *out;
    uint32_t bytes; /* bytes of out written, emulation prevention included */
    uint32_t zeros; /* consecutive zero bytes written */
    uint32_t value;
    uint32_t count;
} bits;

static void put_byte(bits *b, uint8_t byte)
{
    if (b->zeros >= 2 && byte <= 3) {
        b->out[b->bytes++] = 3; /* emulation prevention */
        b->zeros = 0;
    }
    b->out[b->bytes++] = byte;
    b->zeros = byte == 0 ? b->zeros + 1u : 0u;
}

static void put_bits(bits *b, uint32_t value, uint32_t count)
{
    while (count-- > 0) {
        b->value = (b->value << 1) | ((value >> count) & 1u);
        if (++b->count == 8) {
            put_byte(b, (uint8_t)b->value);
            b->value = 0;
            b->count = 0;
        }
    }
}

static void put_ue(bits *b, uint32_t value)
{
    uint32_t coded = value + 1u;
    uint32_t length = 0;
    while ((coded >> length) > 1u) {
        ++length;
    }
    put_bits(b, 0, length);
    put_bits(b, coded, length + 1u);
}

static void put_trailing(bits *b)
{
    put_bits(b, 1, 1);
    while (b->count != 0) {
        put_bits(b, 0, 1);
    }
}

static uint32_t write_sps_pps(uint8_t *out, uint32_t width, uint32_t height)
{
    static const uint8_t start[4] = {0, 0, 0, 1};
    uint32_t mb_width = (width + 15u) / 16u;
    uint32_t mb_height = (height + 15u) / 16u;
    uint32_t crop_right = (mb_width * 16u - width) / 2u;
    uint32_t crop_bottom = (mb_height * 16u - height) / 2u;
    bits b;
    memset(&b, 0, sizeof(b));
    b.out = out;
    memcpy(out, start, 4);
    b.bytes = 4;
    put_bits(&b, 0x67, 8);     /* SPS */
    put_bits(&b, 66, 8);       /* Baseline */
    put_bits(&b, 0xC0, 8);     /* constrained */
    put_bits(&b, 40, 8);       /* level 4.0 */
    put_ue(&b, 0);             /* seq_parameter_set_id */
    put_ue(&b, 0);             /* log2_max_frame_num_minus4 */
    put_ue(&b, 2);             /* pic_order_cnt_type */
    put_ue(&b, 1);             /* max_num_ref_frames */
    put_bits(&b, 0, 1);        /* gaps_in_frame_num_value_allowed_flag */
    put_ue(&b, mb_width - 1u);
    put_ue(&b, mb_height - 1u);
    put_bits(&b, 1, 1);        /* frame_mbs_only_flag */
    put_bits(&b, 1, 1);        /* direct_8x8_inference_flag */
    put_bits(&b, crop_right != 0 || crop_bottom != 0, 1);
    if (crop_right != 0 || crop_bottom != 0) {
        put_ue(&b, 0);
        put_ue(&b, crop_right);
        put_ue(&b, 0);
        put_ue(&b, crop_bottom);
    }
    put_bits(&b, 0, 1);        /* vui_parameters_present_flag */
    put_trailing(&b);
    memcpy(out + b.bytes, start, 4);
    b.bytes += 4;
    b.zeros = 0;
    put_bits(&b, 0x68, 8);     /* PPS */
    put_ue(&b, 0);             /* pic_parameter_set_id */
    put_ue(&b, 0);             /* seq_parameter_set_id */
    put_bits(&b, 0, 1);        /* entropy_coding_mode_flag */
    put_bits(&b, 0, 1);        /* bottom_field_pic_order_in_frame_present_flag */
    put_ue(&b, 0);             /* num_slice_groups_minus1 */
    put_ue(&b, 0);             /* num_ref_idx_l0_default_active_minus1 */
    put_ue(&b, 0);             /* num_ref_idx_l1_default_active_minus1 */
    put_bits(&b, 0, 1);        /* weighted_pred_flag */
    put_bits(&b, 0, 2);        /* weighted_bipred_idc */
    put_ue(&b, 0);             /* pic_init_qp_minus26 (se 0) */
    put_ue(&b, 0);             /* pic_init_qs_minus26 */
    put_ue(&b, 0);             /* chroma_qp_index_offset */
    put_bits(&b, 1, 1);        /* deblocking_filter_control_present_flag */
    put_bits(&b, 0, 1);        /* constrained_intra_pred_flag */
    put_bits(&b, 0, 1);        /* redundant_pic_cnt_present_flag */
    put_trailing(&b);
    return b.bytes;
}

static uint32_t write_slice(uint8_t *out, int keyframe, uint64_t sequence)
{
    uint32_t i;
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 1;
    out[4] = keyframe ? 0x65 : 0x41;
    out[5] = 0x88; /* first_mb_in_slice = 0 */
    for (i = 0; i < 10; ++i) {
        out[6 + i] = (uint8_t)(0x80u | ((sequence >> (7u * i)) & 0x7Fu));
    }
    return 16;
}

static uint32_t cam_test_jpeg(uint8_t *out, uint32_t width, uint32_t height, uint64_t sequence)
{
    char comment[64];
    uint32_t n = 0;
    uint32_t length;
    uint32_t i;
    out[n++] = 0xFF;
    out[n++] = 0xD8;
    length = (uint32_t)snprintf(comment, sizeof(comment), "keel-test frame %llu %ux%u",
                                (unsigned long long)sequence, width, height);
    out[n++] = 0xFF;
    out[n++] = 0xFE;
    out[n++] = (uint8_t)((length + 2u) >> 8);
    out[n++] = (uint8_t)(length + 2u);
    memcpy(out + n, comment, length);
    n += length;
    out[n++] = 0xFF;
    out[n++] = 0xDB;
    out[n++] = 0x00;
    out[n++] = 0x43;
    out[n++] = 0x00;
    for (i = 0; i < 64; ++i) {
        out[n++] = 1;
    }
    out[n++] = 0xFF;
    out[n++] = 0xC0;
    out[n++] = 0x00;
    out[n++] = 0x0B;
    out[n++] = 8;
    out[n++] = (uint8_t)(height >> 8);
    out[n++] = (uint8_t)height;
    out[n++] = (uint8_t)(width >> 8);
    out[n++] = (uint8_t)width;
    out[n++] = 1;
    out[n++] = 1;
    out[n++] = 0x11;
    out[n++] = 0;
    out[n++] = 0xFF;
    out[n++] = 0xDA;
    out[n++] = 0x00;
    out[n++] = 0x08;
    out[n++] = 1;
    out[n++] = 1;
    out[n++] = 0x00;
    out[n++] = 0;
    out[n++] = 63;
    out[n++] = 0;
    out[n++] = 0x12;
    out[n++] = 0xFF;
    out[n++] = 0x00;
    out[n++] = 0x34;
    out[n++] = 0xFF;
    out[n++] = 0xD0;
    out[n++] = 0x56;
    out[n++] = 0xFF;
    out[n++] = 0xD9;
    return n;
}

static void encode(test_encoder *encoder, const cam_frame *frame)
{
    uint8_t chunk[CHUNK_MAX];
    uint32_t bytes = 0;
    int keyframe = 1;
    if (encoder->config.codec == KEEL_CAMERA_CODEC_H264) {
        /* Like the hardware: a forced keyframe starts a new intra period. */
        int forced = atomic_exchange(&encoder->force_keyframe, 0) != 0;
        keyframe = forced || encoder->encoded == 0 || encoder->since_keyframe >= encoder->config.intra_period;
        if (keyframe) {
            encoder->since_keyframe = 0;
            bytes = write_sps_pps(chunk, encoder->config.width, encoder->config.height);
        }
        encoder->since_keyframe++;
        bytes += write_slice(chunk + bytes, keyframe, frame->sequence);
    } else {
        bytes = cam_test_jpeg(chunk, encoder->config.width, encoder->config.height, frame->sequence);
    }
    encoder->encoded++;
    encoder->sink(encoder->context, chunk, bytes, keyframe, frame->sequence, frame->timestamp_ns);
}

static void *test_encoder_main(void *arg)
{
    test_encoder *encoder = arg;
    pthread_mutex_lock(&encoder->lock);
    for (;;) {
        cam_frame *frame;
        while (encoder->pending_count == 0 && !encoder->stop) {
            pthread_cond_wait(&encoder->cond, &encoder->lock);
        }
        if (encoder->pending_count == 0) { /* stopping, and every waiting frame is encoded */
            break;
        }
        frame = encoder->pending[0];
        memmove(&encoder->pending[0], &encoder->pending[1],
                (CAM_ENCODER_PENDING - 1u) * sizeof(encoder->pending[0]));
        encoder->pending_count--;
        pthread_mutex_unlock(&encoder->lock);
        if (cam_test_fault("encoder-fail") && encoder->encoded >= FAIL_AFTER) {
            cam_error error;
            cam_error_clear(&error);
            cam_fail(&error, KEEL_E_PERIPHERAL, KEEL_CAMERA_RESOURCE_ENCODER, EIO, NULL,
                     "the hardware encoder failed (test fault)");
            cam_frame_unref(frame);
            encoder->failure(encoder->context, &error);
            pthread_mutex_lock(&encoder->lock);
            break;
        }
        encode(encoder, frame);
        cam_frame_unref(frame);
        pthread_mutex_lock(&encoder->lock);
    }
    pthread_mutex_unlock(&encoder->lock);
    return NULL;
}

static int32_t test_probe(uint32_t codec, uint32_t *max_width, uint32_t *max_height, cam_error *error)
{
    if (cam_test_fault("encoder-missing")) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_ENCODER, ENODEV, NULL,
                        "no hardware %s encoder (test fault)", codec == KEEL_CAMERA_CODEC_H264 ? "H.264" : "JPEG");
    }
    *max_width = TEST_MAX;
    *max_height = TEST_MAX;
    return KEEL_OK;
}

static int32_t test_open(const cam_encoder_config *config, cam_packet_sink sink, cam_encoder_failure failure,
                         void *context, cam_encoder **out, cam_error *error)
{
    test_encoder *encoder;
    int rc;
    *out = NULL;
    if (config->width > TEST_MAX || config->height > TEST_MAX) {
        return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_ENCODER, 0, NULL,
                        "the hardware encoder takes at most %ux%u", (unsigned)TEST_MAX, (unsigned)TEST_MAX);
    }
    encoder = calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    encoder->base.ops = &cam_test_encoder;
    encoder->config = *config;
    encoder->sink = sink;
    encoder->failure = failure;
    encoder->context = context;
    pthread_mutex_init(&encoder->lock, NULL);
    pthread_cond_init(&encoder->cond, NULL);
    rc = cam_thread_start(&encoder->thread, "keel-cam-encode", test_encoder_main, encoder);
    if (rc != 0) {
        pthread_cond_destroy(&encoder->cond);
        pthread_mutex_destroy(&encoder->lock);
        free(encoder);
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_THREAD, rc, NULL,
                        "cannot start the encoder thread: %s", strerror(rc));
    }
    *out = &encoder->base;
    return KEEL_OK;
}

static int test_submit(cam_encoder *base, cam_frame *frame)
{
    test_encoder *encoder = (test_encoder *)base;
    cam_frame *dropped = NULL;
    cam_frame_ref(frame);
    pthread_mutex_lock(&encoder->lock);
    if (encoder->pending_count == CAM_ENCODER_PENDING) {
        dropped = encoder->pending[0];
        memmove(&encoder->pending[0], &encoder->pending[1],
                (CAM_ENCODER_PENDING - 1u) * sizeof(encoder->pending[0]));
        encoder->pending_count--;
    }
    encoder->pending[encoder->pending_count++] = frame;
    pthread_cond_signal(&encoder->cond);
    pthread_mutex_unlock(&encoder->lock);
    if (dropped != NULL) {
        cam_frame_unref(dropped);
        return 1;
    }
    return 0;
}

static void test_force_keyframe(cam_encoder *base)
{
    atomic_store(&((test_encoder *)base)->force_keyframe, 1);
}

static void test_close(cam_encoder *base)
{
    test_encoder *encoder = (test_encoder *)base;
    uint32_t i;
    pthread_mutex_lock(&encoder->lock);
    encoder->stop = 1;
    pthread_cond_signal(&encoder->cond);
    pthread_mutex_unlock(&encoder->lock);
    pthread_join(encoder->thread, NULL);
    for (i = 0; i < encoder->pending_count; ++i) {
        cam_frame_unref(encoder->pending[i]);
    }
    pthread_cond_destroy(&encoder->cond);
    pthread_mutex_destroy(&encoder->lock);
    free(encoder);
}

static int32_t test_encode_jpeg(const cam_frame *frame, const cam_stream *stream, uint32_t quality, uint8_t **out,
                                uint32_t *out_bytes, cam_error *error)
{
    (void)quality;
    if (cam_test_fault("encoder-missing")) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_ENCODER, ENODEV, NULL,
                        "no hardware JPEG encoder (test fault)");
    }
    *out = malloc(CHUNK_MAX);
    if (*out == NULL) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    *out_bytes = cam_test_jpeg(*out, stream->width, stream->height, frame->sequence);
    return KEEL_OK;
}

const cam_encoder_ops cam_test_encoder = {
    "test", test_probe, test_open, test_submit, test_force_keyframe, test_close, test_encode_jpeg,
};
