/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Container files and streaming protocols through FFmpeg's libavformat,
 * an optional build feature (KEEL_CAMERA_LIBAV, SPEC section 26.7): .mp4,
 * .mkv, .mov and .ts files, rtsp://, rtmp:// and srt:// URLs. The encoded
 * chunks are muxed as they are (no transcoding). Without libavformat these
 * destinations are KEEL_E_UNSUPPORTED.
 *
 * The file is created or the connection opened in keel_camera_output_start();
 * the container header is written at the first keyframe, whose parameter sets
 * it needs, and the trailer (a container's index) when the output stops.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "camera/camera_internal.h"

#ifdef KEEL_CAMERA_HAVE_LIBAV

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

typedef struct libav_state {
    AVFormatContext *context;
    AVStream *stream;
    AVPacket *packet;
    int header_written;
    int64_t first_us;
} libav_state;

int cam_libav_available(void)
{
    return 1;
}

static void fail_locked(cam_output *output, int av_error, const char *what)
{
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(av_error, text, sizeof(text));
    cam_output_failed_locked(output, KEEL_E_PERIPHERAL, av_error < 0 ? AVUNERROR(av_error) : 0, "%s: %s: %s",
                             output->destination.path, what, text);
}

static int write_libav(cam_output *output, const cam_packet *chunk)
{
    libav_state *state = output->kind_state;
    int64_t us = (int64_t)(chunk->timestamp_ns / 1000u);
    int rc;
    if (!state->header_written) {
        if (!(chunk->flags & KEEL_CAMERA_ENCODED_KEYFRAME)) {
            return 0;
        }
        if (output->codec == KEEL_CAMERA_CODEC_H264 && chunk->header_bytes > 0) {
            AVCodecParameters *parameters = state->stream->codecpar;
            parameters->extradata = av_mallocz((size_t)chunk->header_bytes + AV_INPUT_BUFFER_PADDING_SIZE);
            if (parameters->extradata != NULL) {
                memcpy(parameters->extradata, chunk->data, chunk->header_bytes);
                parameters->extradata_size = (int)chunk->header_bytes;
            }
        }
        rc = avformat_write_header(state->context, NULL);
        if (rc < 0) {
            pthread_mutex_lock(&output->lock);
            fail_locked(output, rc, "the container header could not be written");
            pthread_mutex_unlock(&output->lock);
            return -1;
        }
        state->header_written = 1;
        state->first_us = us;
    }
    av_packet_unref(state->packet);
    state->packet->data = chunk->data;
    state->packet->size = (int)chunk->bytes;
    state->packet->stream_index = 0;
    state->packet->pts = av_rescale_q(us - state->first_us, (AVRational){1, 1000000}, state->stream->time_base);
    state->packet->dts = state->packet->pts;
    state->packet->duration = 0;
    state->packet->flags = (chunk->flags & KEEL_CAMERA_ENCODED_KEYFRAME) ? AV_PKT_FLAG_KEY : 0;
    rc = av_write_frame(state->context, state->packet);
    state->packet->data = NULL;
    state->packet->size = 0;
    if (rc < 0) {
        pthread_mutex_lock(&output->lock);
        fail_locked(output, rc, "writing failed");
        pthread_mutex_unlock(&output->lock);
        return -1;
    }
    return 0;
}

static void finish_libav(cam_output *output)
{
    libav_state *state = output->kind_state;
    if (state == NULL) {
        return;
    }
    if (state->header_written) {
        int rc = av_write_trailer(state->context);
        if (rc < 0) {
            pthread_mutex_lock(&output->lock);
            fail_locked(output, rc, "the container could not be finished");
            pthread_mutex_unlock(&output->lock);
        }
    }
    if (!(state->context->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&state->context->pb);
    }
    av_packet_free(&state->packet);
    avformat_free_context(state->context);
    state->context = NULL;
}

static const cam_output_ops libav_ops = {write_libav, finish_libav};

int32_t cam_output_libav_open(cam_output *output, cam_error *error)
{
    const char *url = output->destination.path;
    libav_state *state = calloc(1, sizeof(*state));
    AVCodecParameters *parameters;
    int rc;
    if (state == NULL) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    rc = avformat_alloc_output_context2(&state->context, NULL, output->destination.libav_format, url);
    state->packet = av_packet_alloc();
    if (rc < 0 || state->context == NULL || state->packet == NULL) {
        av_packet_free(&state->packet);
        avformat_free_context(state->context);
        free(state);
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_LIBAV, 0, url,
                        "%s: libavformat has no %s muxer", url, output->destination.libav_format);
    }
    state->stream = avformat_new_stream(state->context, NULL);
    if (state->stream == NULL) {
        av_packet_free(&state->packet);
        avformat_free_context(state->context);
        free(state);
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    parameters = state->stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = output->codec == KEEL_CAMERA_CODEC_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_MJPEG;
    parameters->width = (int)output->width;
    parameters->height = (int)output->height;
    parameters->color_range = AVCOL_RANGE_JPEG;
    state->stream->time_base = (AVRational){1, 90000}; /* the usual video clock; muxers may change it */
    state->stream->avg_frame_rate = (AVRational){(int)output->framerate_milli, 1000};
    if (!(state->context->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open2(&state->context->pb, url, AVIO_FLAG_WRITE, NULL, NULL);
        if (rc < 0) {
            char text[AV_ERROR_MAX_STRING_SIZE];
            int os_error = AVUNERROR(rc);
            av_strerror(rc, text, sizeof(text));
            av_packet_free(&state->packet);
            avformat_free_context(state->context);
            free(state);
            return cam_fail(error, os_error == ECONNREFUSED ? KEEL_E_NOT_FOUND : cam_status_for_errno(os_error),
                            KEEL_CAMERA_RESOURCE_LIBAV, os_error, url, "cannot open %s: %s", url, text);
        }
    }
    output->kind_state = state;
    output->ops = &libav_ops;
    return KEEL_OK;
}

#else

int cam_libav_available(void)
{
    return 0;
}

int32_t cam_output_libav_open(cam_output *output, cam_error *error)
{
    return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_LIBAV, 0, output->destination.path,
                    "%s needs FFmpeg's libavformat, which this library was built without; write .h264 or "
                    ".mjpeg, stream over tcp:// or udp://, or pipe the stream into ffmpeg",
                    output->destination.path);
}

#endif
