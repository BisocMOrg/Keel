/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * White-box units of libkeel-camera, compiled from its sources: destination
 * parsing, H.264 scanning and colour signalling (against the parameter sets
 * the Raspberry Pi 4 encoder writes), the packet and frame queues' drop
 * policies, stream layout, model names, the default bit rate and the
 * diagnosis of an empty camera list on fake device trees (SPEC section 26).
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "camera/camera_internal.h"
#include "check.h"

/* ---- destinations ------------------------------------------------------- */

static void destinations(void)
{
    static const struct {
        const char *url;
        uint32_t codec;
        uint32_t kind;
        const char *host;
        uint16_t port;
        const char *format;
    } good[] = {
        {"tcp://192.168.1.5:5000", KEEL_CAMERA_CODEC_H264, CAM_DEST_TCP, "192.168.1.5", 5000, NULL},
        {"tcp://laptop.local:1", KEEL_CAMERA_CODEC_MJPEG, CAM_DEST_TCP, "laptop.local", 1, NULL},
        {"tcp://[fe80::1]:65535", KEEL_CAMERA_CODEC_H264, CAM_DEST_TCP, "fe80::1", 65535, NULL},
        {"tcp://:5000?listen", KEEL_CAMERA_CODEC_H264, CAM_DEST_TCP_LISTEN, "", 5000, NULL},
        {"tcp://0.0.0.0:5000?listen=1", KEEL_CAMERA_CODEC_H264, CAM_DEST_TCP_LISTEN, "0.0.0.0", 5000, NULL},
        {"tcp://[::]:8000?listen", KEEL_CAMERA_CODEC_MJPEG, CAM_DEST_TCP_LISTEN, "::", 8000, NULL},
        {"udp://10.0.0.2:5004", KEEL_CAMERA_CODEC_H264, CAM_DEST_UDP, "10.0.0.2", 5004, NULL},
        {"rtsp://server:8554/robot", KEEL_CAMERA_CODEC_H264, CAM_DEST_LIBAV, "", 0, "rtsp"},
        {"rtmps://live/key", KEEL_CAMERA_CODEC_H264, CAM_DEST_LIBAV, "", 0, "flv"},
        {"srt://server:9000", KEEL_CAMERA_CODEC_H264, CAM_DEST_LIBAV, "", 0, "mpegts"},
        {"/tmp/clip.h264", KEEL_CAMERA_CODEC_H264, CAM_DEST_FILE, "", 0, NULL},
        {"clip.264", KEEL_CAMERA_CODEC_H264, CAM_DEST_FILE, "", 0, NULL},
        {"CLIP.MJPG", KEEL_CAMERA_CODEC_MJPEG, CAM_DEST_FILE, "", 0, NULL},
        {"a.mjpeg", KEEL_CAMERA_CODEC_MJPEG, CAM_DEST_FILE, "", 0, NULL},
        {"a.mp4", KEEL_CAMERA_CODEC_H264, CAM_DEST_LIBAV, "", 0, "mp4"},
        {"a.mov", KEEL_CAMERA_CODEC_H264, CAM_DEST_LIBAV, "", 0, "mov"},
        {"a.ts", KEEL_CAMERA_CODEC_H264, CAM_DEST_LIBAV, "", 0, "mpegts"},
        {"a.mkv", KEEL_CAMERA_CODEC_MJPEG, CAM_DEST_LIBAV, "", 0, "matroska"},
    };
    static const struct {
        const char *url;
        uint32_t codec;
    } bad[] = {
        {"", KEEL_CAMERA_CODEC_H264},           {"-", KEEL_CAMERA_CODEC_H264},
        {"tcp://host", KEEL_CAMERA_CODEC_H264},  {"tcp://host:0", KEEL_CAMERA_CODEC_H264},
        {"tcp://host:65536", KEEL_CAMERA_CODEC_H264}, {"tcp://host:12ab", KEEL_CAMERA_CODEC_H264},
        {"tcp://::1:80", KEEL_CAMERA_CODEC_H264}, {"tcp://[::1]80", KEEL_CAMERA_CODEC_H264},
        {"tcp://a/b:80", KEEL_CAMERA_CODEC_H264}, {"tcp://:80", KEEL_CAMERA_CODEC_H264},
        {"tcp://h:80?x", KEEL_CAMERA_CODEC_H264}, {"udp://h:80?listen", KEEL_CAMERA_CODEC_H264},
        {"udp://:80", KEEL_CAMERA_CODEC_H264},   {"rtsp://s/x", KEEL_CAMERA_CODEC_MJPEG},
        {"ftp://s/x", KEEL_CAMERA_CODEC_H264},   {"x.h264", KEEL_CAMERA_CODEC_MJPEG},
        {"x.mjpeg", KEEL_CAMERA_CODEC_H264},     {"x.mp4", KEEL_CAMERA_CODEC_MJPEG},
        {"x.avi", KEEL_CAMERA_CODEC_H264},       {"h264", KEEL_CAMERA_CODEC_H264},
    };
    cam_destination d;
    cam_error error;
    size_t i;
    for (i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        cam_error_clear(&error);
        if (cam_destination_parse(good[i].url, good[i].codec, &d, &error) != KEEL_OK) {
            fprintf(stderr, "%s: %s\n", good[i].url, error.message);
            exit(1);
        }
        CHECK_EQ(d.kind, good[i].kind);
        CHECK(strcmp(d.host, good[i].host) == 0);
        CHECK_EQ(d.port, good[i].port);
        CHECK(good[i].format == NULL ? d.libav_format == NULL : strcmp(d.libav_format, good[i].format) == 0);
        if (d.kind == CAM_DEST_FILE || d.kind == CAM_DEST_LIBAV) {
            CHECK(strcmp(d.path, good[i].url) == 0);
        }
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        cam_error_clear(&error);
        if (cam_destination_parse(bad[i].url, bad[i].codec, &d, &error) != KEEL_E_INVALID_ARGUMENT) {
            fprintf(stderr, "%s was accepted\n", bad[i].url);
            exit(1);
        }
        CHECK_EQ(error.resource, KEEL_CAMERA_RESOURCE_OPTIONS);
        CHECK(error.message[0] != '\0');
    }
}

/* ---- H.264 -------------------------------------------------------------- */

/* The SPS and PPS the Raspberry Pi 4 encoder wrote at 640x480 (High
 * profile, level 4.0, VUI with timing and HRD but no signal type). */
static const uint8_t hardware_headers[] = {
    0x00, 0x00, 0x00, 0x01, 0x27, 0x64, 0x00, 0x28, 0xac, 0x2b, 0x40, 0x50, 0x1e, 0xd0, 0x80, 0x00,
    0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x70, 0x50, 0x00, 0x29, 0xf6, 0x00, 0x00, 0xbe, 0xbc,
    0xde, 0xf7, 0x00, 0xf1, 0x22, 0x6a, 0x00, 0x00, 0x00, 0x01, 0x28, 0xee, 0x02, 0x5c, 0xb0,
};

typedef struct bit_reader {
    uint8_t data[512];
    uint32_t bits;
    uint32_t position;
} bit_reader;

/* Loads a NAL unit's payload without emulation prevention bytes. */
static void load(bit_reader *r, const uint8_t *nal, uint32_t bytes)
{
    uint32_t zeros = 0;
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < bytes; ++i) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        r->data[n++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1u : 0u;
    }
    r->bits = n * 8u;
    r->position = 0;
}

static uint32_t bits(bit_reader *r, uint32_t count)
{
    uint32_t value = 0;
    while (count-- > 0) {
        CHECK(r->position < r->bits);
        value = (value << 1) | (((uint32_t)r->data[r->position >> 3] >> (7u - (r->position & 7u))) & 1u);
        r->position++;
    }
    return value;
}

static uint32_t ue(bit_reader *r)
{
    uint32_t zeros = 0;
    while (bits(r, 1) == 0) {
        zeros++;
    }
    return zeros == 0 ? 0u : ((1u << zeros) - 1u) + bits(r, zeros);
}

/* Reads an SPS (payload after the NAL header) up to its VUI and checks the
 * video signal type the library adds. Returns the reader at the field
 * after the signal type. */
static void check_signal_type(bit_reader *r)
{
    uint32_t profile = bits(r, 8);
    bits(r, 16);
    ue(r);
    if (profile == 100) {
        CHECK_EQ(ue(r), 1); /* 4:2:0 */
        ue(r);
        ue(r);
        bits(r, 1);
        CHECK_EQ(bits(r, 1), 0);
    }
    ue(r);
    CHECK_EQ(ue(r), 2); /* pic_order_cnt_type of both encoders */
    ue(r);
    bits(r, 1);
    ue(r);
    ue(r);
    CHECK_EQ(bits(r, 1), 1);
    bits(r, 1);
    if (bits(r, 1)) {
        ue(r);
        ue(r);
        ue(r);
        ue(r);
    }
    CHECK_EQ(bits(r, 1), 1); /* vui_parameters_present_flag */
    CHECK_EQ(bits(r, 1), 0); /* aspect ratio */
    CHECK_EQ(bits(r, 1), 0); /* overscan */
    CHECK_EQ(bits(r, 1), 1); /* video_signal_type_present_flag */
    CHECK_EQ(bits(r, 3), 5); /* unspecified video format */
    CHECK_EQ(bits(r, 1), 1); /* full range */
    CHECK_EQ(bits(r, 1), 1); /* colour description */
    CHECK_EQ(bits(r, 8), 1); /* BT.709 primaries */
    CHECK_EQ(bits(r, 8), 13); /* sRGB transfer */
    CHECK_EQ(bits(r, 8), 6); /* BT.601 matrix */
}

static void h264_colour_hardware(void)
{
    uint8_t out[256];
    bit_reader r;
    uint32_t bytes = cam_h264_signal_full_range(hardware_headers, sizeof(hardware_headers), out, sizeof(out));
    uint32_t header_bytes;
    CHECK(bytes > sizeof(hardware_headers));
    CHECK(bytes <= sizeof(hardware_headers) + 8u);
    /* The PPS is copied unchanged, after the rewritten SPS. */
    CHECK(memcmp(out + bytes - 9, hardware_headers + sizeof(hardware_headers) - 9, 9) == 0);
    CHECK_EQ(cam_h264_scan(out, bytes, &header_bytes), KEEL_CAMERA_ENCODED_HEADERS);
    load(&r, out + 5, bytes - 5u - 9u);
    check_signal_type(&r);
    /* The rest of the VUI follows unchanged: no chroma location, then the
     * timing information (1 / 60). */
    CHECK_EQ(bits(&r, 1), 0);
    CHECK_EQ(bits(&r, 1), 1);
    CHECK_EQ(bits(&r, 32), 1);
    CHECK_EQ(bits(&r, 32), 60);
    /* Rewriting again changes nothing: the signal type is there. */
    {
        uint8_t again[256];
        uint32_t second = cam_h264_signal_full_range(out, bytes, again, sizeof(again));
        CHECK_EQ(second, bytes);
        CHECK(memcmp(again, out, bytes) == 0);
    }
    /* Too small an output buffer. */
    CHECK_EQ(cam_h264_signal_full_range(hardware_headers, sizeof(hardware_headers), out, 20), 0);
}

static void h264_colour_without_vui(void)
{
    /* A Baseline SPS without VUI for 640x480, as the synthetic encoder
     * writes it, and the start of an IDR slice. */
    static const uint8_t sps[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x28, 0xda, 0x02, 0x80,
                                  0xf6, 0x40, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x80};
    uint8_t out[128];
    bit_reader r;
    uint32_t bytes = cam_h264_signal_full_range(sps, sizeof(sps), out, sizeof(out));
    CHECK(bytes > sizeof(sps));
    /* The slice is copied unchanged. */
    CHECK(memcmp(out + bytes - 8, sps + sizeof(sps) - 8, 8) == 0);
    load(&r, out + 5, bytes - 5u - 8u);
    check_signal_type(&r);
    /* The added VUI ends there: no chroma location, timing, HRD or restriction. */
    CHECK_EQ(bits(&r, 7), 0);
    CHECK_EQ(bits(&r, 1), 1); /* rbsp_stop_one_bit */
}

static void h264_scan(void)
{
    static const uint8_t idr[] = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 0x88, 9};
    static const uint8_t p_slice[] = {0, 0, 1, 0x41, 0x9a, 7};
    static const uint8_t sei_first[] = {0, 0, 0, 1, 0x06, 5, 0, 0, 0, 1, 0x67, 1, 0, 0, 0, 1, 0x65, 2};
    uint32_t header_bytes = 99;
    CHECK_EQ(cam_h264_scan(idr, sizeof(idr), &header_bytes),
             KEEL_CAMERA_ENCODED_KEYFRAME | KEEL_CAMERA_ENCODED_HEADERS);
    CHECK_EQ(header_bytes, 12); /* SPS and PPS, up to the IDR's four-byte start code */
    CHECK_EQ(cam_h264_scan(p_slice, sizeof(p_slice), &header_bytes), 0);
    CHECK_EQ(header_bytes, 0);
    /* Parameter sets after another NAL unit are not a strippable prefix. */
    CHECK_EQ(cam_h264_scan(sei_first, sizeof(sei_first), &header_bytes),
             KEEL_CAMERA_ENCODED_KEYFRAME | KEEL_CAMERA_ENCODED_HEADERS);
    CHECK_EQ(header_bytes, 0);
}

/* ---- queues ------------------------------------------------------------- */

static cam_packet *packet(cam_packet_pool *pool, uint32_t flags, uint64_t sequence)
{
    cam_packet *p = cam_packet_get(pool, 16);
    CHECK(p != NULL);
    p->flags = flags;
    p->sequence = sequence;
    p->bytes = 16;
    return p;
}

static void push(cam_packet_queue *queue, cam_packet_pool *pool, uint32_t flags, uint64_t sequence, int queued)
{
    cam_packet *p = packet(pool, flags, sequence);
    CHECK_EQ(cam_packet_queue_push(queue, p), (unsigned)queued);
    cam_packet_unref(p);
}

static void packet_queues(void)
{
    cam_packet_pool pool;
    cam_packet_queue queue;
    cam_packet *p;
    uint64_t i;
    const uint32_t key = KEEL_CAMERA_ENCODED_KEYFRAME;
    cam_packet_pool_init(&pool);
    /* H.264: starts at a keyframe. */
    CHECK(cam_packet_queue_init(&queue, 3, KEEL_CAMERA_CODEC_H264) == 0);
    push(&queue, &pool, 0, 1, 0);
    push(&queue, &pool, key, 2, 1);
    push(&queue, &pool, 0, 3, 1);
    push(&queue, &pool, 0, 4, 1);
    CHECK_EQ(queue.dropped, 1);
    /* Full: everything goes, and so does every frame until a keyframe. */
    push(&queue, &pool, 0, 5, 0);
    CHECK_EQ(queue.count, 0);
    CHECK_EQ(queue.dropped, 5);
    push(&queue, &pool, 0, 6, 0);
    push(&queue, &pool, key, 7, 1);
    CHECK_EQ(queue.dropped, 6);
    p = cam_packet_queue_pop(&queue);
    CHECK_EQ(p->sequence, 7);
    cam_packet_unref(p);
    /* A keyframe arriving at a full queue replaces the backlog. */
    push(&queue, &pool, 0, 8, 1);
    push(&queue, &pool, 0, 9, 1);
    push(&queue, &pool, 0, 10, 1);
    push(&queue, &pool, key, 11, 1);
    CHECK_EQ(queue.count, 1);
    CHECK_EQ(cam_packet_queue_peek(&queue)->sequence, 11);
    cam_packet_queue_free(&queue);
    /* MJPEG: the oldest goes. */
    CHECK(cam_packet_queue_init(&queue, 2, KEEL_CAMERA_CODEC_MJPEG) == 0);
    for (i = 1; i <= 5; ++i) {
        push(&queue, &pool, key, i, 1);
    }
    CHECK_EQ(queue.dropped, 3);
    p = cam_packet_queue_pop(&queue);
    CHECK_EQ(p->sequence, 4);
    cam_packet_unref(p);
    cam_packet_queue_free(&queue);
    /* Every packet came back to the pool, which recycles them. */
    CHECK_EQ(atomic_load(&pool.live), 0);
    CHECK(pool.free_count > 0);
    p = cam_packet_get(&pool, 200000);
    CHECK(p != NULL && p->capacity >= 200000);
    cam_packet_unref(p);
    cam_packet_pool_close(&pool);
    CHECK_EQ(pool.free_count, 0);
    pthread_mutex_destroy(&pool.lock);
}

static int released;

static void count_release(cam_source *source, cam_frame *frame)
{
    (void)source;
    (void)frame;
    released++;
}

static void frame_queue(void)
{
    static const cam_source_ops ops = {"count", NULL, NULL, NULL, NULL, count_release, NULL, NULL, NULL, NULL};
    cam_source source = {&ops};
    cam_frame frames[4];
    cam_frame_queue queue;
    cam_frame *evicted;
    int i;
    memset(&queue, 0, sizeof(queue));
    queue.capacity = 2;
    for (i = 0; i < 4; ++i) {
        memset(&frames[i], 0, sizeof(frames[i]));
        frames[i].source = &source;
        frames[i].sequence = (uint64_t)i;
        cam_frame_deliver(&frames[i]);
    }
    CHECK(cam_frame_queue_push(&queue, &frames[0]) == NULL);
    CHECK(cam_frame_queue_push(&queue, &frames[1]) == NULL);
    evicted = cam_frame_queue_push(&queue, &frames[2]);
    CHECK(evicted == &frames[0]);
    cam_frame_unref(evicted);
    for (i = 0; i < 4; ++i) {
        cam_frame_unref(&frames[i]); /* the sources' own references */
    }
    CHECK_EQ(released, 2); /* frames 0 and 3; 1 and 2 are still queued */
    cam_frame_unref(cam_frame_queue_pop(&queue));
    cam_frame_unref(cam_frame_queue_pop(&queue));
    CHECK_EQ(released, 4);
    CHECK(cam_frame_queue_pop(&queue) == NULL);
}

/* A 64x50 I420 frame (stride 64) into the image encoder's layout: stride
 * 96, 64 lines per luma plane. Rows past the frame repeat its last row. */
static void coded_copy(void)
{
    enum { W = 64, H = 50, STRIDE = 64, DST_STRIDE = 96, DST_H = 64 };
    static uint8_t src[STRIDE * H * 3 / 2];
    static uint8_t dst[DST_STRIDE * DST_H * 3 / 2];
    cam_frame frame;
    uint32_t x;
    uint32_t y;
    for (x = 0; x < sizeof(src); ++x) {
        src[x] = (uint8_t)(x * 7u + 1u);
    }
    memset(dst, 0xEE, sizeof(dst));
    memset(&frame, 0, sizeof(frame));
    frame.data = src;
    frame.bytes = sizeof(src);
    frame.fd = -1;
    CHECK_EQ(cam_coded_frame_bytes(KEEL_CAMERA_FORMAT_I420, DST_STRIDE, DST_H), sizeof(dst));
    CHECK_EQ(cam_coded_frame_bytes(KEEL_CAMERA_FORMAT_RGB888, 192, 16), 192 * 16);
    cam_frame_copy_coded(&frame, KEEL_CAMERA_FORMAT_I420, STRIDE, H, dst, DST_STRIDE, DST_H);
    for (y = 0; y < DST_H; ++y) {
        uint32_t from = y < H ? y : H - 1u;
        CHECK(memcmp(dst + y * DST_STRIDE, src + from * STRIDE, W) == 0);
        CHECK_EQ(dst[y * DST_STRIDE + W], 0xEE); /* row padding is not written */
    }
    for (y = 0; y < DST_H / 2u; ++y) {
        uint32_t from = y < H / 2u ? y : H / 2u - 1u;
        const uint8_t *src_u = src + STRIDE * H;
        const uint8_t *src_v = src_u + (STRIDE / 2) * (H / 2);
        const uint8_t *dst_u = dst + DST_STRIDE * DST_H;
        const uint8_t *dst_v = dst_u + (DST_STRIDE / 2) * (DST_H / 2);
        CHECK(memcmp(dst_u + y * (DST_STRIDE / 2u), src_u + from * (STRIDE / 2u), W / 2) == 0);
        CHECK(memcmp(dst_v + y * (DST_STRIDE / 2u), src_v + from * (STRIDE / 2u), W / 2) == 0);
    }
    /* Packed RGB: one plane. */
    memset(dst, 0xEE, sizeof(dst));
    frame.bytes = 192 * 10;
    cam_frame_copy_coded(&frame, KEEL_CAMERA_FORMAT_RGB888, 192, 10, dst, 192, 16);
    for (y = 0; y < 16; ++y) {
        CHECK(memcmp(dst + y * 192, src + (y < 10 ? y : 9u) * 192, 192) == 0);
    }
}

/* ---- layout, names, defaults -------------------------------------------- */

static void layout_and_names(void)
{
    cam_stream s;
    memset(&s, 0, sizeof(s));
    s.width = 1000;
    s.height = 750;
    s.format = KEEL_CAMERA_FORMAT_I420;
    cam_stream_layout(&s, 64);
    CHECK_EQ(s.plane_count, 3);
    CHECK_EQ(s.stride[0], 1024);
    CHECK_EQ(s.stride[1], 512);
    CHECK_EQ(s.offset[1], 1024 * 750);
    CHECK_EQ(s.offset[2], 1024 * 750 + 512 * 375);
    CHECK_EQ(s.frame_bytes, 1024 * 750 + 2 * 512 * 375);
    s.format = KEEL_CAMERA_FORMAT_RGB888;
    s.width = 100;
    cam_stream_layout(&s, 64);
    CHECK_EQ(s.plane_count, 1);
    CHECK_EQ(s.stride[0], 320);
    CHECK_EQ(s.offset[1], 0);
    CHECK(strcmp(cam_model_name("imx219"), "Camera Module 2") == 0);
    CHECK(strcmp(cam_model_name("imx708"), "Camera Module 3") == 0);
    CHECK(strcmp(cam_model_name("imx708_wide"), "Camera Module 3 Wide") == 0);
    CHECK(strcmp(cam_model_name("imx708_noir"), "Camera Module 3 NoIR") == 0);
    CHECK(strcmp(cam_model_name("imx708_wide_noir"), "Camera Module 3 Wide NoIR") == 0);
    CHECK(strcmp(cam_model_name("imx477"), "HQ Camera") == 0);
    CHECK(strcmp(cam_model_name("ov9281"), "ov9281") == 0);
    CHECK_EQ(cam_default_bitrate(640, 480, 30000), 1382400);
    CHECK_EQ(cam_default_bitrate(1920, 1080, 30000), 9331200);
    CHECK_EQ(cam_default_bitrate(64, 64, 1000), KEEL_CAMERA_BITRATE_MIN);
    CHECK_EQ(cam_default_bitrate(1920, 1920, 240000), KEEL_CAMERA_BITRATE_MAX);
}

/* ---- diagnosis of an empty list ----------------------------------------- */

static void make_tree(char *root, size_t size)
{
    const char *dir = getenv("TMPDIR");
    snprintf(root, size, "%s/keel-camera-dev-XXXXXX", dir != NULL && dir[0] != '\0' ? dir : "/tmp");
    CHECK(mkdtemp(root) != NULL);
}

static void touch(const char *root, const char *name, mode_t mode)
{
    char path[512];
    int fd;
    snprintf(path, sizeof(path), "%s/%s", root, name);
    fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, mode);
    CHECK(fd >= 0);
    close(fd);
    CHECK(chmod(path, mode) == 0);
}

static void remove_tree(const char *root)
{
    char command[600];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    CHECK(system(command) == 0);
}

static void diagnose(void)
{
    char root[256];
    char path[512];
    cam_error error;
    /* No device nodes at all: no camera. */
    make_tree(root, sizeof(root));
    cam_error_clear(&error);
    CHECK_EQ(cam_diagnose_no_camera(root, NULL, &error), KEEL_OK);
    /* Media nodes that are not camera receivers: no camera. */
    touch(root, "media0", 0600);
    touch(root, "video0", 0600);
    CHECK_EQ(cam_diagnose_no_camera(root, "/nonexistent", &error), KEEL_OK);
    /* A media node the user cannot open: the video group. Root opens
     * anything, so this part needs an ordinary user. */
    if (geteuid() != 0) {
        touch(root, "media3", 0000);
        cam_error_clear(&error);
        CHECK_EQ(cam_diagnose_no_camera(root, NULL, &error), KEEL_E_PERMISSION);
        snprintf(path, sizeof(path), "%s/media3", root);
        CHECK(strcmp(error.path, path) == 0);
        CHECK_EQ(error.os_error, EACCES);
        CHECK_EQ(error.resource, KEEL_CAMERA_RESOURCE_DEVICE_NODE);
        CHECK(strstr(error.message, "video group") != NULL);
        CHECK(strstr(error.message, "keel-setup --user") != NULL);
    }
    remove_tree(root);
    /* A missing directory is no camera too. */
    CHECK_EQ(cam_diagnose_no_camera("/nonexistent-keel-dev", NULL, &error), KEEL_OK);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"destinations", destinations},
        {"h264_colour_hardware", h264_colour_hardware},
        {"h264_colour_without_vui", h264_colour_without_vui},
        {"h264_scan", h264_scan},
        {"packet_queues", packet_queues},
        {"frame_queue", frame_queue},
        {"coded_copy", coded_copy},
        {"layout_and_names", layout_and_names},
        {"diagnose", diagnose},
    };
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
