/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * The Raspberry Pi 4 hardware encoders through the kernel's V4L2
 * memory-to-memory interface (SPEC section 26.4): H.264 on the
 * bcm2835-codec "encode" node, JPEG and MJPEG on its "encode_image" node.
 * Nodes are found by driver and card name, never by number.
 *
 * Frames from libcamera are imported as dma-bufs (no copy). Frames without
 * one, and frames whose layout differs from the encoder's (the image
 * encoder pads its buffers to a multiple of 16 lines), are copied into the
 * encoder's own buffers. One thread per encoder queues raw frames, dequeues
 * encoded buffers and hands each chunk to the pipeline. At most
 * CAM_ENCODER_PENDING frames wait for a free encoder buffer; an older
 * waiting frame is dropped for a newer one.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "camera/camera_internal.h"

enum {
    OUTPUT_BUFFERS = 6,
    CAPTURE_BUFFERS = 12,
    H264_CAPTURE_BYTES = 768 * 1024,
    TIMESTAMPS = 32,     /* frames in flight whose sequence numbers are remembered */
    STILL_TIMEOUT_MS = 5000,
    DRAIN_MS = 300       /* longest wait for encoded frames when stopping */
};

#define CODEC_DRIVER "bcm2835-codec"

/* Layout of the encoder's input buffers. The image encoder rounds the
 * buffer height up to 16 lines and encodes only the frame's own lines (its
 * crop), so its buffers can differ from the ISP's frames: those frames are
 * then copied (cam_frame_copy_coded), never imported. */
typedef struct coded_layout {
    uint32_t stride;
    uint32_t height;
    uint32_t memory;     /* V4L2_MEMORY_DMABUF or V4L2_MEMORY_MMAP */
} coded_layout;

typedef struct timestamp_entry {
    uint64_t timestamp_us;
    uint64_t timestamp_ns;
    uint64_t sequence;
} timestamp_entry;

typedef struct v4l2_encoder {
    cam_encoder base;
    cam_encoder_config config;
    int fd;
    int wake_fd;
    cam_packet_sink sink;
    cam_encoder_failure failure;
    void *context;
    pthread_t thread;
    int thread_started;
    pthread_mutex_t lock;
    int stop;                                  /* under lock */
    cam_frame *pending[CAM_ENCODER_PENDING];   /* under lock */
    uint32_t pending_count;                    /* under lock */
    _Atomic int force_keyframe;
    cam_frame *slots[OUTPUT_BUFFERS];          /* frames the encoder holds (dma-buf mode) */
    int slot_busy[OUTPUT_BUFFERS];
    coded_layout coded;                        /* the encoder's input buffer layout */
    uint32_t output_count;
    void *output_map[OUTPUT_BUFFERS];          /* copy mode */
    uint32_t output_length[OUTPUT_BUFFERS];
    uint32_t capture_count;
    void *capture_map[CAPTURE_BUFFERS];
    uint32_t capture_length[CAPTURE_BUFFERS];
    timestamp_entry stamps[TIMESTAMPS];
    uint32_t next_stamp;
} v4l2_encoder;

static int xioctl(int fd, unsigned long request, void *arg)
{
    int rc;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc != 0 && errno == EINTR);
    return rc;
}

static const char *card_for(uint32_t codec)
{
    return codec == KEEL_CAMERA_CODEC_H264 ? "bcm2835-codec-encode" : "bcm2835-codec-encode_image";
}

static uint32_t v4l2_format(uint32_t format)
{
    switch (format) {
    case KEEL_CAMERA_FORMAT_RGB888:
        return V4L2_PIX_FMT_RGB24;
    case KEEL_CAMERA_FORMAT_BGR888:
        return V4L2_PIX_FMT_BGR24;
    default:
        return V4L2_PIX_FMT_YUV420;
    }
}

/* Opens the codec node with the card name for codec; returns the fd or a
 * negative status with error filled in. */
static int open_node(uint32_t codec, cam_error *error)
{
    const char *card = card_for(codec);
    DIR *dir = opendir("/dev");
    struct dirent *entry;
    char denied[KEEL_CAMERA_PATH_MAX] = "";
    int denied_errno = 0;
    if (dir == NULL) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_ENCODER, errno, "/dev", "cannot read /dev");
    }
    while ((entry = readdir(dir)) != NULL) {
        struct v4l2_capability capability;
        char path[KEEL_CAMERA_PATH_MAX + 8];
        int fd;
        if (strncmp(entry->d_name, "video", 5) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if ((errno == EACCES || errno == EPERM) && denied[0] == '\0') {
                cam_copy_string(denied, sizeof(denied), path);
                denied_errno = errno;
            }
            continue;
        }
        memset(&capability, 0, sizeof(capability));
        if (xioctl(fd, VIDIOC_QUERYCAP, &capability) == 0 &&
            strcmp((const char *)capability.driver, CODEC_DRIVER) == 0 &&
            strcmp((const char *)capability.card, card) == 0) {
            closedir(dir);
            return fd;
        }
        close(fd);
    }
    closedir(dir);
    if (denied[0] != '\0') {
        return cam_fail_permission(error, KEEL_CAMERA_RESOURCE_ENCODER, denied_errno, denied);
    }
    return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_ENCODER, ENODEV, NULL,
                    "no hardware %s encoder (V4L2 card %s of driver " CODEC_DRIVER "): it needs a Raspberry Pi 4 "
                    "with the bcm2835-codec kernel module",
                    codec == KEEL_CAMERA_CODEC_H264 ? "H.264" : "JPEG", card);
}

static int32_t v4l2_probe(uint32_t codec, uint32_t *max_width, uint32_t *max_height, cam_error *error)
{
    struct v4l2_frmsizeenum size;
    int fd = open_node(codec, error);
    if (fd < 0) {
        return fd;
    }
    memset(&size, 0, sizeof(size));
    size.pixel_format = V4L2_PIX_FMT_YUV420;
    if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0 && size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
        *max_width = size.stepwise.max_width;
        *max_height = size.stepwise.max_height;
    } else {
        *max_width = KEEL_CAMERA_ENCODER_SIZE_MAX;
        *max_height = KEEL_CAMERA_ENCODER_SIZE_MAX;
    }
    close(fd);
    return KEEL_OK;
}

static int set_control(int fd, uint32_t id, int32_t value)
{
    struct v4l2_control control;
    control.id = id;
    control.value = value;
    return xioctl(fd, VIDIOC_S_CTRL, &control);
}

static int32_t h264_level(const cam_encoder_config *config)
{
    uint64_t macroblocks = (uint64_t)((config->width + 15u) / 16u) * ((config->height + 15u) / 16u);
    uint64_t per_second = macroblocks * config->framerate_milli / 1000u;
    if (macroblocks <= 8192u && per_second <= 245760u) {
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
    }
    return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
}

static int32_t h264_profile(uint32_t profile)
{
    switch (profile) {
    case KEEL_CAMERA_H264_PROFILE_BASELINE:
        return V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
    case KEEL_CAMERA_H264_PROFILE_MAIN:
        return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
    default:
        return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
    }
}

static int32_t fail_ioctl(cam_error *error, const char *what)
{
    int os_error = errno;
    return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_ENCODER, os_error, NULL,
                    "the hardware encoder refused %s: %s", what, strerror(os_error));
}

/* Formats, controls and buffers of an open node; streaming started. coded
 * gives the input layout; its memory asks for DMABUF or MMAP input buffers
 * and becomes MMAP when the encoder's layout differs from the frames'. */
static int32_t configure_node(int fd, const cam_encoder_config *config, coded_layout *coded,
                              uint32_t output_buffers, uint32_t capture_buffers, void **output_map,
                              uint32_t *output_length, void **capture_map, uint32_t *capture_length,
                              cam_error *error)
{
    struct v4l2_format format;
    struct v4l2_streamparm parm;
    struct v4l2_requestbuffers request;
    uint32_t row_bytes = config->format == KEEL_CAMERA_FORMAT_I420 ? config->width : config->width * 3u;
    uint32_t output_memory;
    uint32_t i;
    int type;
    if (config->codec == KEEL_CAMERA_CODEC_H264) {
        if (set_control(fd, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)config->bitrate) != 0 ||
            set_control(fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE, h264_profile(config->profile)) != 0 ||
            set_control(fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, h264_level(config)) != 0 ||
            set_control(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, (int32_t)config->intra_period) != 0 ||
            set_control(fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1) != 0) {
            return fail_ioctl(error, "the H.264 settings");
        }
    } else if (set_control(fd, V4L2_CID_JPEG_COMPRESSION_QUALITY, (int32_t)config->quality) != 0) {
        return fail_ioctl(error, "the JPEG quality");
    }
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = v4l2_format(config->format);
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].bytesperline = config->stride;
    if (xioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        return fail_ioctl(error, "the input format");
    }
    coded->stride = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    coded->height = format.fmt.pix_mp.height;
    if (format.fmt.pix_mp.width != config->width || coded->stride < row_bytes ||
        (coded->height != config->height && coded->height != (config->height + 15u) / 16u * 16u)) {
        return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_ENCODER, 0, NULL,
                        "the hardware encoder does not take %ux%u frames (it offers %ux%u)", config->width,
                        config->height, format.fmt.pix_mp.width, coded->height);
    }
    if (coded->height != config->height) {
        /* The image encoder pads its buffers and encodes the crop, which
         * must be the frame. */
        struct v4l2_selection selection;
        memset(&selection, 0, sizeof(selection));
        selection.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        selection.target = V4L2_SEL_TGT_CROP;
        selection.r.width = config->width;
        selection.r.height = config->height;
        if (xioctl(fd, VIDIOC_S_SELECTION, &selection) != 0) {
            return fail_ioctl(error, "the crop");
        }
        if (selection.r.width != config->width || selection.r.height != config->height) {
            return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_ENCODER, 0, NULL,
                            "the hardware encoder cannot encode %ux%u (it crops to %ux%u)", config->width,
                            config->height, selection.r.width, selection.r.height);
        }
    }
    if (coded->stride != config->stride || coded->height != config->height) {
        coded->memory = V4L2_MEMORY_MMAP;
    }
    output_memory = coded->memory;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = config->codec == KEEL_CAMERA_CODEC_H264 ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_JPEG;
    format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    format.fmt.pix_mp.num_planes = 1;
    if (config->codec == KEEL_CAMERA_CODEC_H264) {
        format.fmt.pix_mp.plane_fmt[0].sizeimage = H264_CAPTURE_BYTES;
    }
    if (xioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        return fail_ioctl(error, "the output format");
    }
    if (config->framerate_milli != 0) {
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        parm.parm.output.timeperframe.numerator = 1000;
        parm.parm.output.timeperframe.denominator = config->framerate_milli;
        (void)xioctl(fd, VIDIOC_S_PARM, &parm);
    }
    memset(&request, 0, sizeof(request));
    request.count = output_buffers;
    request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    request.memory = output_memory;
    if (xioctl(fd, VIDIOC_REQBUFS, &request) != 0 || request.count < output_buffers) {
        return fail_ioctl(error, "input buffers");
    }
    for (i = 0; output_memory == V4L2_MEMORY_MMAP && i < output_buffers; ++i) {
        struct v4l2_buffer buffer;
        struct v4l2_plane plane;
        memset(&buffer, 0, sizeof(buffer));
        memset(&plane, 0, sizeof(plane));
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            return fail_ioctl(error, "an input buffer");
        }
        output_map[i] = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
        if (output_map[i] == MAP_FAILED) {
            output_map[i] = NULL;
            return fail_ioctl(error, "mapping an input buffer");
        }
        output_length[i] = plane.length;
    }
    memset(&request, 0, sizeof(request));
    request.count = capture_buffers;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 1) {
        return fail_ioctl(error, "output buffers");
    }
    for (i = 0; i < capture_buffers && i < request.count; ++i) {
        struct v4l2_buffer buffer;
        struct v4l2_plane plane;
        memset(&buffer, 0, sizeof(buffer));
        memset(&plane, 0, sizeof(plane));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            return fail_ioctl(error, "an output buffer");
        }
        capture_map[i] = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
        if (capture_map[i] == MAP_FAILED) {
            capture_map[i] = NULL;
            return fail_ioctl(error, "mapping an output buffer");
        }
        capture_length[i] = plane.length;
        if (xioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            return fail_ioctl(error, "queueing an output buffer");
        }
    }
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        return fail_ioctl(error, "starting its input");
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        return fail_ioctl(error, "starting its output");
    }
    return KEEL_OK;
}

static void unmap_all(void **maps, uint32_t *lengths, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        if (maps[i] != NULL) {
            munmap(maps[i], lengths[i]);
            maps[i] = NULL;
        }
    }
}

static void stream_off(int fd)
{
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);
}

static void report_failure(v4l2_encoder *encoder, const char *what)
{
    cam_error error;
    int os_error = errno;
    cam_error_clear(&error);
    cam_fail(&error, KEEL_E_PERIPHERAL, KEEL_CAMERA_RESOURCE_ENCODER, os_error, NULL,
             "the hardware encoder failed (%s): %s", what, strerror(os_error));
    encoder->failure(encoder->context, &error);
}

/* Copies a frame into an input buffer of length bytes in the encoder's
 * layout and sets the bytes it needs; returns -1 when they do not fit. */
static int copy_input(const cam_encoder_config *config, const coded_layout *coded, const cam_frame *frame,
                      void *buffer, uint32_t length, uint32_t *bytesused)
{
    int same = coded->stride == config->stride && coded->height == config->height;
    *bytesused = same ? frame->bytes : cam_coded_frame_bytes(config->format, coded->stride, coded->height);
    if (*bytesused > length) {
        return -1;
    }
    if (same) {
        cam_frame_copy(frame, buffer);
    } else {
        cam_frame_copy_coded(frame, config->format, config->stride, config->height, buffer, coded->stride,
                             coded->height);
    }
    return 0;
}

/* Queues one frame into a free input buffer; returns 0, 1 when none is free,
 * -1 on failure. The frame reference passes to the encoder. */
static int queue_frame(v4l2_encoder *encoder, cam_frame *frame)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane plane;
    uint32_t slot;
    timestamp_entry *stamp;
    for (slot = 0; slot < encoder->output_count; ++slot) {
        if (!encoder->slot_busy[slot]) {
            break;
        }
    }
    if (slot == encoder->output_count) {
        return 1;
    }
    if (atomic_exchange(&encoder->force_keyframe, 0)) {
        set_control(encoder->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);
    }
    memset(&buffer, 0, sizeof(buffer));
    memset(&plane, 0, sizeof(plane));
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffer.index = slot;
    buffer.field = V4L2_FIELD_NONE;
    buffer.length = 1;
    buffer.m.planes = &plane;
    buffer.timestamp.tv_sec = (time_t)(frame->timestamp_ns / 1000000000u);
    buffer.timestamp.tv_usec = (suseconds_t)(frame->timestamp_ns % 1000000000u / 1000u);
    plane.bytesused = frame->bytes;
    if (encoder->config.use_dmabuf) {
        buffer.memory = V4L2_MEMORY_DMABUF;
        plane.m.fd = frame->fd;
        plane.length = frame->fd_length;
    } else {
        buffer.memory = V4L2_MEMORY_MMAP;
        plane.length = encoder->output_length[slot];
        if (copy_input(&encoder->config, &encoder->coded, frame, encoder->output_map[slot], plane.length,
                       &plane.bytesused) != 0) {
            errno = EOVERFLOW;
            cam_frame_unref(frame);
            return -1;
        }
    }
    stamp = &encoder->stamps[encoder->next_stamp++ % TIMESTAMPS];
    stamp->timestamp_us = frame->timestamp_ns / 1000u;
    stamp->timestamp_ns = frame->timestamp_ns;
    stamp->sequence = frame->sequence;
    if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) != 0) {
        cam_frame_unref(frame);
        return -1;
    }
    encoder->slot_busy[slot] = 1;
    if (encoder->config.use_dmabuf) {
        encoder->slots[slot] = frame;
    } else {
        cam_frame_unref(frame); /* copied */
    }
    return 0;
}

/* Dequeues finished input buffers; -1 on failure. */
static int reclaim_inputs(v4l2_encoder *encoder)
{
    for (;;) {
        struct v4l2_buffer buffer;
        struct v4l2_plane plane;
        memset(&buffer, 0, sizeof(buffer));
        memset(&plane, 0, sizeof(plane));
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buffer.memory = encoder->config.use_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) != 0) {
            return errno == EAGAIN ? 0 : -1;
        }
        if (buffer.index < encoder->output_count) {
            encoder->slot_busy[buffer.index] = 0;
            if (encoder->slots[buffer.index] != NULL) {
                cam_frame_unref(encoder->slots[buffer.index]);
                encoder->slots[buffer.index] = NULL;
            }
        }
    }
}

static void lookup_stamp(const v4l2_encoder *encoder, uint64_t timestamp_us, uint64_t *sequence,
                         uint64_t *timestamp_ns)
{
    uint32_t i;
    *sequence = 0;
    *timestamp_ns = timestamp_us * 1000u;
    for (i = 0; i < TIMESTAMPS; ++i) {
        if (encoder->stamps[i].timestamp_us == timestamp_us) {
            *sequence = encoder->stamps[i].sequence;
            *timestamp_ns = encoder->stamps[i].timestamp_ns;
            return;
        }
    }
}

/* Dequeues encoded buffers, hands them on and requeues them; -1 on failure. */
static int deliver_outputs(v4l2_encoder *encoder)
{
    for (;;) {
        struct v4l2_buffer buffer;
        struct v4l2_plane plane;
        uint64_t sequence;
        uint64_t timestamp_ns;
        memset(&buffer, 0, sizeof(buffer));
        memset(&plane, 0, sizeof(plane));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) != 0) {
            return errno == EAGAIN ? 0 : -1;
        }
        lookup_stamp(encoder, (uint64_t)buffer.timestamp.tv_sec * 1000000u + (uint64_t)buffer.timestamp.tv_usec,
                     &sequence, &timestamp_ns);
        if (plane.bytesused > 0 && buffer.index < encoder->capture_count) {
            int keyframe = encoder->config.codec != KEEL_CAMERA_CODEC_H264 ||
                           (buffer.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
            encoder->sink(encoder->context, (const uint8_t *)encoder->capture_map[buffer.index] + plane.data_offset,
                          plane.bytesused - plane.data_offset, keyframe, sequence, timestamp_ns);
        }
        if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) != 0) {
            return -1;
        }
    }
}

static void *encoder_main(void *arg)
{
    v4l2_encoder *encoder = arg;
    uint64_t drain_until = 0;
    for (;;) {
        struct pollfd fds[2];
        cam_frame *frames[CAM_ENCODER_PENDING];
        uint32_t count;
        uint32_t i;
        pthread_mutex_lock(&encoder->lock);
        if (encoder->stop) {
            /* Stopping: the frames the encoder already has still come out
             * (the end of a recording), for at most DRAIN_MS. */
            uint32_t busy = 0;
            for (i = 0; i < encoder->output_count; ++i) {
                busy += encoder->slot_busy[i] ? 1u : 0u;
            }
            if (drain_until == 0) {
                drain_until = cam_now_ns() + (uint64_t)DRAIN_MS * 1000000u;
            }
            if (busy == 0 || cam_now_ns() >= drain_until) {
                pthread_mutex_unlock(&encoder->lock);
                break;
            }
        }
        /* While draining, no new frame goes in. */
        count = encoder->stop ? 0u : encoder->pending_count;
        memcpy(frames, encoder->pending, sizeof(frames));
        if (!encoder->stop) {
            encoder->pending_count = 0;
        }
        pthread_mutex_unlock(&encoder->lock);
        for (i = 0; i < count; ++i) {
            int rc = queue_frame(encoder, frames[i]);
            if (rc < 0) {
                report_failure(encoder, "queueing a frame");
                while (++i < count) {
                    cam_frame_unref(frames[i]);
                }
                return NULL;
            }
            if (rc == 1) {
                /* Every input buffer is busy: the frame waits, unless a
                 * newer one arrived meanwhile. */
                pthread_mutex_lock(&encoder->lock);
                if (encoder->pending_count < CAM_ENCODER_PENDING) {
                    memmove(&encoder->pending[1], &encoder->pending[0],
                            encoder->pending_count * sizeof(encoder->pending[0]));
                    encoder->pending[0] = frames[i];
                    encoder->pending_count++;
                    frames[i] = NULL;
                }
                pthread_mutex_unlock(&encoder->lock);
                if (frames[i] != NULL) {
                    cam_frame_unref(frames[i]);
                }
            }
        }
        fds[0].fd = encoder->fd;
        fds[0].events = POLLIN | POLLOUT;
        fds[1].fd = encoder->wake_fd;
        fds[1].events = POLLIN;
        if (poll(fds, 2, 200) < 0 && errno != EINTR) {
            report_failure(encoder, "poll");
            return NULL;
        }
        if (fds[1].revents & POLLIN) {
            uint64_t value;
            (void)!read(encoder->wake_fd, &value, sizeof(value));
        }
        if (reclaim_inputs(encoder) != 0) {
            report_failure(encoder, "dequeueing an input buffer");
            return NULL;
        }
        if (deliver_outputs(encoder) != 0) {
            report_failure(encoder, "dequeueing an output buffer");
            return NULL;
        }
    }
    return NULL;
}

static void encoder_free(v4l2_encoder *encoder)
{
    uint32_t i;
    if (encoder->fd >= 0) {
        struct v4l2_requestbuffers request;
        stream_off(encoder->fd);
        unmap_all(encoder->output_map, encoder->output_length, OUTPUT_BUFFERS);
        unmap_all(encoder->capture_map, encoder->capture_length, CAPTURE_BUFFERS);
        memset(&request, 0, sizeof(request));
        request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        request.memory = encoder->config.use_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        xioctl(encoder->fd, VIDIOC_REQBUFS, &request);
        memset(&request, 0, sizeof(request));
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        request.memory = V4L2_MEMORY_MMAP;
        xioctl(encoder->fd, VIDIOC_REQBUFS, &request);
        close(encoder->fd);
    }
    /* After STREAMOFF the encoder holds no frame. */
    for (i = 0; i < OUTPUT_BUFFERS; ++i) {
        if (encoder->slots[i] != NULL) {
            cam_frame_unref(encoder->slots[i]);
        }
    }
    for (i = 0; i < encoder->pending_count; ++i) {
        cam_frame_unref(encoder->pending[i]);
    }
    if (encoder->wake_fd >= 0) {
        close(encoder->wake_fd);
    }
    pthread_mutex_destroy(&encoder->lock);
    free(encoder);
}

static int32_t v4l2_open(const cam_encoder_config *config, cam_packet_sink sink, cam_encoder_failure failure,
                         void *context, cam_encoder **out, cam_error *error)
{
    v4l2_encoder *encoder = calloc(1, sizeof(*encoder));
    int32_t rc;
    int thread_rc;
    *out = NULL;
    if (encoder == NULL) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    encoder->base.ops = &cam_v4l2_encoder;
    encoder->config = *config;
    encoder->sink = sink;
    encoder->failure = failure;
    encoder->context = context;
    encoder->output_count = OUTPUT_BUFFERS;
    encoder->capture_count = CAPTURE_BUFFERS;
    encoder->wake_fd = -1;
    pthread_mutex_init(&encoder->lock, NULL);
    encoder->fd = open_node(config->codec, error);
    if (encoder->fd < 0) {
        rc = encoder->fd;
        encoder->fd = -1;
        encoder_free(encoder);
        return rc;
    }
    encoder->coded.memory = config->use_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    rc = configure_node(encoder->fd, config, &encoder->coded, OUTPUT_BUFFERS, CAPTURE_BUFFERS, encoder->output_map,
                        encoder->output_length, encoder->capture_map, encoder->capture_length, error);
    encoder->config.use_dmabuf = encoder->coded.memory == V4L2_MEMORY_DMABUF;
    if (rc == KEEL_OK) {
        encoder->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (encoder->wake_fd < 0) {
            rc = fail_ioctl(error, "an eventfd");
        }
    }
    if (rc != KEEL_OK) {
        encoder_free(encoder);
        return rc;
    }
    thread_rc = cam_thread_start(&encoder->thread, "keel-cam-encode", encoder_main, encoder);
    if (thread_rc != 0) {
        encoder_free(encoder);
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_THREAD, thread_rc, NULL,
                        "cannot start the encoder thread: %s", strerror(thread_rc));
    }
    encoder->thread_started = 1;
    *out = &encoder->base;
    return KEEL_OK;
}

static int v4l2_submit(cam_encoder *base, cam_frame *frame)
{
    v4l2_encoder *encoder = (v4l2_encoder *)base;
    cam_frame *dropped = NULL;
    uint64_t one = 1;
    cam_frame_ref(frame);
    pthread_mutex_lock(&encoder->lock);
    if (encoder->pending_count == CAM_ENCODER_PENDING) {
        dropped = encoder->pending[0];
        memmove(&encoder->pending[0], &encoder->pending[1],
                (CAM_ENCODER_PENDING - 1u) * sizeof(encoder->pending[0]));
        encoder->pending_count--;
    }
    encoder->pending[encoder->pending_count++] = frame;
    pthread_mutex_unlock(&encoder->lock);
    (void)!write(encoder->wake_fd, &one, sizeof(one));
    if (dropped != NULL) {
        cam_frame_unref(dropped);
        return 1;
    }
    return 0;
}

static void v4l2_force_keyframe(cam_encoder *base)
{
    v4l2_encoder *encoder = (v4l2_encoder *)base;
    atomic_store(&encoder->force_keyframe, 1);
}

static void v4l2_close(cam_encoder *base)
{
    v4l2_encoder *encoder = (v4l2_encoder *)base;
    uint64_t one = 1;
    pthread_mutex_lock(&encoder->lock);
    encoder->stop = 1;
    pthread_mutex_unlock(&encoder->lock);
    (void)!write(encoder->wake_fd, &one, sizeof(one));
    if (encoder->thread_started) {
        pthread_join(encoder->thread, NULL);
    }
    encoder_free(encoder);
}

static int32_t v4l2_encode_jpeg(const cam_frame *frame, const cam_stream *stream, uint32_t quality, uint8_t **out,
                                uint32_t *out_bytes, cam_error *error)
{
    cam_encoder_config config;
    coded_layout coded;
    void *output_map[1] = {NULL};
    uint32_t output_length[1] = {0};
    void *capture_map[1] = {NULL};
    uint32_t capture_length[1] = {0};
    struct v4l2_buffer buffer;
    struct v4l2_plane plane;
    struct pollfd pfd;
    int32_t rc;
    int fd = open_node(KEEL_CAMERA_CODEC_MJPEG, error);
    *out = NULL;
    if (fd < 0) {
        return fd;
    }
    memset(&config, 0, sizeof(config));
    config.codec = KEEL_CAMERA_CODEC_MJPEG;
    config.width = stream->width;
    config.height = stream->height;
    config.format = stream->format;
    config.stride = stream->stride[0];
    config.frame_bytes = stream->frame_bytes;
    config.quality = quality;
    coded.memory = V4L2_MEMORY_MMAP;
    rc = configure_node(fd, &config, &coded, 1, 1, output_map, output_length, capture_map, capture_length, error);
    memset(&plane, 0, sizeof(plane));
    if (rc == KEEL_OK && copy_input(&config, &coded, frame, output_map[0], output_length[0], &plane.bytesused) != 0) {
        rc = cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_ENCODER, EOVERFLOW, NULL,
                      "the still (%u bytes) exceeds the encoder's input buffer (%u bytes)", plane.bytesused,
                      output_length[0]);
    }
    if (rc == KEEL_OK) {
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = 0;
        buffer.field = V4L2_FIELD_NONE;
        buffer.length = 1;
        buffer.m.planes = &plane;
        plane.length = output_length[0];
        if (xioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            rc = fail_ioctl(error, "the still");
        }
    }
    if (rc == KEEL_OK) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, STILL_TIMEOUT_MS) <= 0) {
            rc = cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_ENCODER, ETIMEDOUT, NULL,
                          "the hardware JPEG encoder did not finish within 5 s");
        }
    }
    if (rc == KEEL_OK) {
        memset(&buffer, 0, sizeof(buffer));
        memset(&plane, 0, sizeof(plane));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (xioctl(fd, VIDIOC_DQBUF, &buffer) != 0) {
            rc = fail_ioctl(error, "returning the JPEG image");
        } else {
            uint32_t bytes = plane.bytesused - plane.data_offset;
            *out = malloc(bytes);
            if (*out == NULL) {
                rc = cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
            } else {
                memcpy(*out, (const uint8_t *)capture_map[0] + plane.data_offset, bytes);
                *out_bytes = bytes;
            }
        }
    }
    stream_off(fd);
    unmap_all(output_map, output_length, 1);
    unmap_all(capture_map, capture_length, 1);
    close(fd);
    return rc;
}

const cam_encoder_ops cam_v4l2_encoder = {
    "v4l2", v4l2_probe, v4l2_open, v4l2_submit, v4l2_force_keyframe, v4l2_close, v4l2_encode_jpeg,
};
