/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * The runtime of one open camera (SPEC section 26.4).
 *
 * Data flow: the capture source calls on_frame() on its thread with one
 * reference to each frame. The frame goes into the program's frame queue
 * (the oldest frame drops when it is full) and to the encoder, which keeps
 * at most CAM_ENCODER_PENDING frames. The encoder calls on_packet() on its
 * thread; the packet goes into the program's encoded queue and into each
 * output's queue. Nothing here waits for the program or a destination.
 *
 * Locks: session->lock protects the queues, the state, the counters and the
 * output list; an output's own lock nests inside it (cam_output_push), never
 * the other way. A listening output asks for a keyframe through an atomic
 * flag, which on_frame() passes to the encoder, so no output thread takes
 * the session lock.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "camera/camera_session.h"

enum {
    H264_CHUNK_MAX = 1024 * 1024,
    H264_SIGNAL_ROOM = 64,       /* bytes a rewritten SPS may grow by */
    STILL_SETTLE_FRAMES = 8,     /* frames for exposure and white balance to settle */
    STILL_FOCUS_MS = 1500,       /* longest autofocus wait before a still */
    STILL_BUFFERS = 2
};

struct cam_session {
    pthread_mutex_t lock;
    pthread_cond_t frame_cond;
    pthread_cond_t encoded_cond;
    const cam_source_ops *source_ops;
    const cam_encoder_ops *encoder_ops;
    cam_source *source;
    cam_camera_desc desc;
    cam_session_config config;
    uint32_t state;              /* keel_camera_state */
    int closing;
    int stopping;                /* frames are refused; the encoder drains */
    cam_frame_queue frames;
    cam_session_encoder encoder_settings;
    uint32_t encoded_bytes_max;
    cam_encoder *encoder;
    _Atomic int keyframe_wanted;
    cam_packet_pool pool;
    cam_packet_queue encoded;
    int program_headers_sent;
    cam_output *outputs[KEEL_CAMERA_OUTPUTS_MAX];
    uint32_t next_output_id;
    keel_camera_stats stats;
    cam_error failure;           /* why the state is FAILED */
    int failure_reported;        /* keel_camera_last_error() returned it */
};

/* ---- capture callbacks -------------------------------------------------- */

static void on_frame(void *context, cam_frame *frame)
{
    cam_session *session = context;
    cam_frame *evicted = NULL;
    cam_encoder *encoder;
    pthread_mutex_lock(&session->lock);
    if (session->state != KEEL_CAMERA_STATE_RUNNING || session->stopping) {
        pthread_mutex_unlock(&session->lock);
        cam_frame_unref(frame);
        return;
    }
    session->stats.frames_captured++;
    evicted = cam_frame_queue_push(&session->frames, frame);
    if (evicted != NULL) {
        session->stats.frames_dropped++;
    }
    pthread_cond_broadcast(&session->frame_cond);
    encoder = session->encoder;
    if (encoder != NULL) {
        if (atomic_exchange(&session->keyframe_wanted, 0)) {
            encoder->ops->force_keyframe(encoder);
        }
        if (encoder->ops->submit(encoder, frame)) {
            session->stats.encoder_frames_dropped++;
        }
    }
    pthread_mutex_unlock(&session->lock);
    if (evicted != NULL) {
        cam_frame_unref(evicted);
    }
    cam_frame_unref(frame);
}

static void on_failure(void *context, const cam_error *error)
{
    cam_session *session = context;
    pthread_mutex_lock(&session->lock);
    if (session->state == KEEL_CAMERA_STATE_RUNNING) {
        session->state = KEEL_CAMERA_STATE_FAILED;
        session->failure = *error;
        session->failure_reported = 0;
        pthread_cond_broadcast(&session->frame_cond);
        pthread_cond_broadcast(&session->encoded_cond);
    }
    pthread_mutex_unlock(&session->lock);
}

static void on_packet(void *context, const uint8_t *data, uint32_t bytes, int keyframe, uint64_t sequence,
                      uint64_t timestamp_ns)
{
    cam_session *session = context;
    /* Room for the video signal type added to a sequence parameter set. */
    cam_packet *packet = cam_packet_get(&session->pool, bytes + H264_SIGNAL_ROOM);
    uint32_t i;
    if (packet == NULL) {
        pthread_mutex_lock(&session->lock);
        session->stats.chunks_dropped++;
        pthread_mutex_unlock(&session->lock);
        return;
    }
    packet->codec = session->encoder_settings.codec;
    packet->bytes = 0;
    if (packet->codec == KEEL_CAMERA_CODEC_H264) {
        packet->bytes = cam_h264_signal_full_range(data, bytes, packet->data, packet->capacity);
    }
    if (packet->bytes == 0) {
        memcpy(packet->data, data, bytes);
        packet->bytes = bytes;
    }
    bytes = packet->bytes;
    packet->sequence = sequence;
    packet->timestamp_ns = timestamp_ns;
    if (packet->codec == KEEL_CAMERA_CODEC_H264) {
        packet->flags = cam_h264_scan(packet->data, bytes, &packet->header_bytes);
        if (keyframe) {
            packet->flags |= KEEL_CAMERA_ENCODED_KEYFRAME;
        }
    } else {
        packet->flags = KEEL_CAMERA_ENCODED_KEYFRAME;
    }
    pthread_mutex_lock(&session->lock);
    if (session->state == KEEL_CAMERA_STATE_RUNNING) {
        session->stats.chunks_encoded++;
        if (bytes <= session->encoded_bytes_max) {
            cam_packet_queue_push(&session->encoded, packet);
            pthread_cond_broadcast(&session->encoded_cond);
        } else {
            session->stats.chunks_dropped++;
        }
        for (i = 0; i < KEEL_CAMERA_OUTPUTS_MAX; ++i) {
            if (session->outputs[i] != NULL) {
                cam_output_push(session->outputs[i], packet);
            }
        }
    }
    pthread_mutex_unlock(&session->lock);
    cam_packet_unref(packet);
}

static void request_keyframe(void *context)
{
    cam_session *session = context;
    atomic_store(&session->keyframe_wanted, 1);
}

/* ---- lifetime ----------------------------------------------------------- */

int32_t cam_session_open(const cam_source_ops *source_ops, const cam_encoder_ops *encoder_ops,
                         const cam_session_config *config, cam_session **out, cam_error *error)
{
    cam_session *session = calloc(1, sizeof(*session));
    int32_t rc;
    *out = NULL;
    if (session == NULL) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    session->source_ops = source_ops;
    session->encoder_ops = encoder_ops;
    session->config = *config;
    session->config.stream.buffer_count = config->frame_queue + CAM_EXTRA_BUFFERS;
    session->config.stream.still = 0;
    session->frames.capacity = config->frame_queue;
    session->next_output_id = 1;
    pthread_mutex_init(&session->lock, NULL);
    cam_cond_init(&session->frame_cond);
    cam_cond_init(&session->encoded_cond);
    cam_packet_pool_init(&session->pool);
    rc = source_ops->open(config->index, &session->source, &session->desc, error);
    if (rc == KEEL_OK) {
        const keel_camera_info *info = &session->desc.info;
        const cam_controls *c = &config->controls;
        if (!(info->flags & KEEL_CAMERA_INFO_AUTOFOCUS) &&
            (c->af_mode != KEEL_CAMERA_AF_DEFAULT || c->af_range != KEEL_CAMERA_AF_RANGE_DEFAULT)) {
            rc = cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_CAMERA, 0, NULL,
                          "camera %u (%s, %s) has a fixed-focus lens: leave af_mode, af_range and "
                          "lens_position_milli at 0", config->index, info->model, info->sensor);
        }
    }
    if (rc == KEEL_OK) {
        rc = source_ops->configure(session->source, &session->config.stream, error);
    }
    if (rc != KEEL_OK) {
        if (session->source != NULL) {
            source_ops->close(session->source);
        }
        cam_packet_pool_close(&session->pool);
        free(session);
        return rc;
    }
    *out = session;
    return KEEL_OK;
}

void cam_session_wake(cam_session *session)
{
    pthread_mutex_lock(&session->lock);
    session->closing = 1;
    pthread_cond_broadcast(&session->frame_cond);
    pthread_cond_broadcast(&session->encoded_cond);
    pthread_mutex_unlock(&session->lock);
}

int32_t cam_session_close(cam_session *session)
{
    int32_t status = KEEL_OK;
    uint32_t i;
    cam_session_stop(session);
    for (i = 0; i < KEEL_CAMERA_OUTPUTS_MAX; ++i) {
        cam_output *output;
        pthread_mutex_lock(&session->lock);
        output = session->outputs[i];
        session->outputs[i] = NULL;
        pthread_mutex_unlock(&session->lock);
        if (output != NULL && cam_output_close(output) != KEEL_OK) {
            status = KEEL_E_PERIPHERAL;
        }
    }
    cam_packet_queue_free(&session->encoded);
    cam_packet_pool_close(&session->pool);
    session->source_ops->close(session->source);
    pthread_cond_destroy(&session->encoded_cond);
    pthread_cond_destroy(&session->frame_cond);
    pthread_mutex_destroy(&session->lock);
    free(session);
    return status;
}

const cam_camera_desc *cam_session_desc(const cam_session *session)
{
    return &session->desc;
}

const cam_session_config *cam_session_config_of(const cam_session *session)
{
    return &session->config;
}

void cam_session_stream_info(cam_session *session, keel_camera_stream_info *out)
{
    const cam_stream *s = &session->config.stream;
    out->format = s->format;
    out->width = s->width;
    out->height = s->height;
    out->framerate_milli = s->framerate_milli;
    out->plane_count = s->plane_count;
    out->stride0 = s->stride[0];
    out->stride1 = s->stride[1];
    out->stride2 = s->stride[2];
    out->offset0 = s->offset[0];
    out->offset1 = s->offset[1];
    out->offset2 = s->offset[2];
    out->frame_bytes = s->frame_bytes;
    out->codec = session->encoder_settings.codec;
    out->encoded_bytes_max = session->encoded_bytes_max;
}

void cam_session_stats(cam_session *session, keel_camera_stats *out)
{
    uint32_t i;
    pthread_mutex_lock(&session->lock);
    out->state = session->state;
    out->frames_captured = session->stats.frames_captured;
    out->frames_read = session->stats.frames_read;
    out->frames_dropped = session->stats.frames_dropped;
    out->encoder_frames_dropped = session->stats.encoder_frames_dropped;
    out->chunks_encoded = session->stats.chunks_encoded;
    out->chunks_read = session->stats.chunks_read;
    out->chunks_dropped = session->stats.chunks_dropped + session->encoded.dropped;
    out->outputs = 0;
    for (i = 0; i < KEEL_CAMERA_OUTPUTS_MAX; ++i) {
        out->outputs += session->outputs[i] != NULL ? 1u : 0u;
    }
    pthread_mutex_unlock(&session->lock);
}

int cam_session_take_failure(cam_session *session, cam_error *out)
{
    int fresh;
    pthread_mutex_lock(&session->lock);
    fresh = session->state == KEEL_CAMERA_STATE_FAILED && !session->failure_reported;
    if (fresh) {
        *out = session->failure;
        session->failure_reported = 1;
    }
    pthread_mutex_unlock(&session->lock);
    return fresh;
}

static uint32_t output_count(const cam_session *session)
{
    uint32_t i;
    uint32_t count = 0;
    for (i = 0; i < KEEL_CAMERA_OUTPUTS_MAX; ++i) {
        count += session->outputs[i] != NULL ? 1u : 0u;
    }
    return count;
}

int32_t cam_session_set_encoder(cam_session *session, const cam_session_encoder *encoder, cam_error *error)
{
    const cam_stream *s = &session->config.stream;
    uint32_t max_width = 0;
    uint32_t max_height = 0;
    int32_t rc;
    if (session->state != KEEL_CAMERA_STATE_STOPPED || output_count(session) > 0) {
        return cam_fail(error, KEEL_E_STATE, KEEL_CAMERA_RESOURCE_ENCODER, 0, NULL,
                        session->state != KEEL_CAMERA_STATE_STOPPED
                            ? "the encoder can change only while stopped: call keel_camera_stop() first"
                            : "the encoder can change only without outputs: stop them first");
    }
    if (encoder->codec != KEEL_CAMERA_CODEC_NONE) {
        rc = session->encoder_ops->probe(encoder->codec, &max_width, &max_height, error);
        if (rc != KEEL_OK) {
            return rc;
        }
        if (s->width > max_width || s->height > max_height) {
            return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_ENCODER, 0, NULL,
                            "the hardware encoder takes at most %ux%u; the stream is %ux%u: open the camera "
                            "at a smaller size", max_width, max_height, s->width, s->height);
        }
    }
    cam_packet_queue_free(&session->encoded);
    session->encoder_settings = *encoder;
    session->encoded_bytes_max = 0;
    if (encoder->codec == KEEL_CAMERA_CODEC_NONE) {
        return KEEL_OK;
    }
    if (cam_packet_queue_init(&session->encoded, encoder->encoded_queue, encoder->codec) != 0) {
        session->encoder_settings.codec = KEEL_CAMERA_CODEC_NONE;
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    session->encoded_bytes_max = encoder->codec == KEEL_CAMERA_CODEC_H264
                                     ? H264_CHUNK_MAX
                                     : s->width * s->height * 2u + 65536u;
    return KEEL_OK;
}

int32_t cam_session_start(cam_session *session, cam_error *error)
{
    const cam_stream *s = &session->config.stream;
    cam_encoder *encoder = NULL;
    int32_t rc;
    if (session->state != KEEL_CAMERA_STATE_STOPPED) {
        return cam_fail(error, KEEL_E_STATE, KEEL_CAMERA_RESOURCE_CAMERA, 0, NULL,
                        session->state == KEEL_CAMERA_STATE_RUNNING
                            ? "capture is already running"
                            : "capture failed: call keel_camera_stop(), then keel_camera_start() again");
    }
    if (session->encoder_settings.codec != KEEL_CAMERA_CODEC_NONE) {
        cam_encoder_config config;
        memset(&config, 0, sizeof(config));
        config.codec = session->encoder_settings.codec;
        config.width = s->width;
        config.height = s->height;
        config.format = s->format;
        config.stride = s->stride[0];
        config.frame_bytes = s->frame_bytes;
        config.framerate_milli = s->framerate_milli;
        config.bitrate = session->encoder_settings.bitrate;
        config.intra_period = session->encoder_settings.intra_period;
        config.profile = session->encoder_settings.profile;
        config.quality = session->encoder_settings.quality;
        config.use_dmabuf = session->source_ops == &cam_libcamera_source;
        rc = session->encoder_ops->open(&config, on_packet, on_failure, session, &encoder, error);
        if (rc != KEEL_OK) {
            return rc;
        }
        session->encoded.need_keyframe = session->encoded.codec == KEEL_CAMERA_CODEC_H264;
        session->program_headers_sent = 0;
    }
    pthread_mutex_lock(&session->lock);
    session->encoder = encoder;
    session->state = KEEL_CAMERA_STATE_RUNNING;
    cam_error_clear(&session->failure);
    pthread_mutex_unlock(&session->lock);
    rc = session->source_ops->start(session->source, s, &session->config.controls, on_frame, on_failure, session,
                                    error);
    if (rc != KEEL_OK) {
        pthread_mutex_lock(&session->lock);
        session->state = KEEL_CAMERA_STATE_STOPPED;
        session->encoder = NULL;
        pthread_mutex_unlock(&session->lock);
        if (encoder != NULL) {
            encoder->ops->close(encoder);
        }
        return rc;
    }
    return KEEL_OK;
}

void cam_session_stop(cam_session *session)
{
    cam_encoder *encoder;
    cam_frame *frame;
    pthread_mutex_lock(&session->lock);
    if (session->state == KEEL_CAMERA_STATE_STOPPED) {
        pthread_mutex_unlock(&session->lock);
        return;
    }
    session->stopping = 1;
    pthread_mutex_unlock(&session->lock);
    /* No frame arrives after this. The encoder then hands over what it
     * holds (the end of a recording reaches the outputs) and closes. */
    session->source_ops->stop(session->source);
    pthread_mutex_lock(&session->lock);
    encoder = session->encoder;
    session->encoder = NULL;
    pthread_mutex_unlock(&session->lock);
    if (encoder != NULL) {
        encoder->ops->close(encoder);
    }
    pthread_mutex_lock(&session->lock);
    session->state = KEEL_CAMERA_STATE_STOPPED;
    session->stopping = 0;
    while ((frame = cam_frame_queue_pop(&session->frames)) != NULL) {
        pthread_mutex_unlock(&session->lock);
        cam_frame_unref(frame);
        pthread_mutex_lock(&session->lock);
    }
    if (session->encoded.items != NULL) {
        cam_packet_queue_clear(&session->encoded);
    }
    pthread_mutex_unlock(&session->lock);
    session->source_ops->free_buffers(session->source);
}

/* ---- reading ------------------------------------------------------------ */

/* Waits on cond until ready() or the deadline; returns the status a read
 * without an item reports (KEEL_OK when an item is there). */
static int32_t wait_for(cam_session *session, pthread_cond_t *cond, const uint32_t *count, uint32_t timeout_ms,
                        cam_error *error)
{
    struct timespec deadline;
    cam_deadline(&deadline, timeout_ms);
    while (*count == 0 && session->state == KEEL_CAMERA_STATE_RUNNING && !session->closing) {
        if (pthread_cond_timedwait(cond, &session->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    if (session->closing) {
        return cam_fail(error, KEEL_E_INVALID_HANDLE, KEEL_CAMERA_RESOURCE_NONE, 0, NULL,
                        "the camera was closed by another thread");
    }
    if (*count > 0) {
        return KEEL_OK;
    }
    if (session->state == KEEL_CAMERA_STATE_FAILED) {
        *error = session->failure;
        session->failure_reported = 1;
        return session->failure.status;
    }
    if (session->state == KEEL_CAMERA_STATE_STOPPED) {
        return cam_fail(error, KEEL_E_STATE, KEEL_CAMERA_RESOURCE_CAMERA, 0, NULL,
                        "capture is not running: call keel_camera_start() first");
    }
    return cam_fail(error, KEEL_E_AGAIN, KEEL_CAMERA_RESOURCE_CAMERA, ETIMEDOUT, NULL,
                    "no frame arrived within %u ms", timeout_ms);
}

int32_t cam_session_read_frame(cam_session *session, uint8_t *buffer, uint32_t size, uint32_t timeout_ms,
                               keel_camera_frame_info *out, cam_error *error)
{
    const cam_stream *s = &session->config.stream;
    cam_frame *frame;
    int32_t rc;
    pthread_mutex_lock(&session->lock);
    rc = wait_for(session, &session->frame_cond, &session->frames.count, timeout_ms, error);
    if (rc != KEEL_OK) {
        pthread_mutex_unlock(&session->lock);
        return rc;
    }
    frame = session->frames.items[session->frames.head];
    if (size < frame->bytes) {
        pthread_mutex_unlock(&session->lock);
        return cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, NULL,
                        "the buffer has %u bytes; a frame needs %u (keel_camera_stream_info.frame_bytes)", size,
                        frame->bytes);
    }
    (void)cam_frame_queue_pop(&session->frames);
    session->stats.frames_read++;
    pthread_mutex_unlock(&session->lock);
    cam_frame_copy(frame, buffer);
    out->format = s->format;
    out->width = s->width;
    out->height = s->height;
    out->plane_count = s->plane_count;
    out->stride0 = s->stride[0];
    out->stride1 = s->stride[1];
    out->stride2 = s->stride[2];
    out->offset0 = s->offset[0];
    out->offset1 = s->offset[1];
    out->offset2 = s->offset[2];
    out->bytes = frame->bytes;
    out->sequence = frame->sequence;
    out->timestamp_ns = frame->timestamp_ns;
    cam_frame_unref(frame);
    return KEEL_OK;
}

int32_t cam_session_read_encoded(cam_session *session, uint8_t *buffer, uint32_t size, uint32_t timeout_ms,
                                 keel_camera_encoded_info *out, cam_error *error)
{
    cam_packet *packet;
    uint32_t skip = 0;
    int32_t rc;
    if (session->encoder_settings.codec == KEEL_CAMERA_CODEC_NONE) {
        return cam_fail(error, KEEL_E_STATE, KEEL_CAMERA_RESOURCE_ENCODER, 0, NULL,
                        "no encoder is set: call keel_camera_set_encoder() first");
    }
    pthread_mutex_lock(&session->lock);
    rc = wait_for(session, &session->encoded_cond, &session->encoded.count, timeout_ms, error);
    if (rc == KEEL_E_AGAIN) {
        cam_fail(error, KEEL_E_AGAIN, KEEL_CAMERA_RESOURCE_ENCODER, ETIMEDOUT, NULL,
                 "no encoded chunk arrived within %u ms", timeout_ms);
    }
    if (rc != KEEL_OK) {
        pthread_mutex_unlock(&session->lock);
        return rc;
    }
    packet = cam_packet_queue_peek(&session->encoded);
    /* Without KEEL_CAMERA_INLINE_HEADERS the parameter sets come only once. */
    if (!(session->encoder_settings.flags & KEEL_CAMERA_INLINE_HEADERS) && session->program_headers_sent &&
        packet->header_bytes > 0) {
        skip = packet->header_bytes;
    }
    if (size < packet->bytes - skip) {
        pthread_mutex_unlock(&session->lock);
        return cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, NULL,
                        "the buffer has %u bytes; this chunk needs %u (keel_camera_stream_info.encoded_bytes_max "
                        "always suffices)", size, packet->bytes - skip);
    }
    (void)cam_packet_queue_pop(&session->encoded);
    if (packet->flags & KEEL_CAMERA_ENCODED_HEADERS) {
        session->program_headers_sent = 1;
    }
    session->stats.chunks_read++;
    pthread_mutex_unlock(&session->lock);
    memcpy(buffer, packet->data + skip, packet->bytes - skip);
    out->codec = packet->codec;
    out->flags = skip > 0 ? packet->flags & ~(uint32_t)KEEL_CAMERA_ENCODED_HEADERS : packet->flags;
    out->bytes = packet->bytes - skip;
    out->sequence = packet->sequence;
    out->timestamp_ns = packet->timestamp_ns;
    cam_packet_unref(packet);
    return KEEL_OK;
}

int32_t cam_session_autofocus(cam_session *session, uint32_t timeout_ms, uint32_t *state, cam_error *error)
{
    if (!(session->desc.info.flags & KEEL_CAMERA_INFO_AUTOFOCUS)) {
        return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_CAMERA, 0, NULL,
                        "camera %u (%s, %s) has a fixed-focus lens", session->desc.info.index,
                        session->desc.info.model, session->desc.info.sensor);
    }
    if (session->state != KEEL_CAMERA_STATE_RUNNING) {
        return cam_fail(error, KEEL_E_STATE, KEEL_CAMERA_RESOURCE_CAMERA, 0, NULL,
                        "autofocus needs capture running: call keel_camera_start() first");
    }
    return session->source_ops->autofocus(session->source, timeout_ms, state, error);
}

/* ---- outputs ------------------------------------------------------------ */

int32_t cam_session_output_start(cam_session *session, const char *url, const keel_camera_output_options *options,
                                 uint32_t *out_id, cam_error *error)
{
    const cam_stream *s = &session->config.stream;
    cam_destination destination;
    keel_camera_output_options resolved = *options;
    cam_output *output;
    uint32_t slot;
    int32_t rc;
    if (session->encoder_settings.codec == KEEL_CAMERA_CODEC_NONE) {
        return cam_fail(error, KEEL_E_STATE, KEEL_CAMERA_RESOURCE_ENCODER, 0, url,
                        "outputs carry the encoded stream: call keel_camera_set_encoder() first");
    }
    for (slot = 0; slot < KEEL_CAMERA_OUTPUTS_MAX; ++slot) {
        if (session->outputs[slot] == NULL) {
            break;
        }
    }
    if (slot == KEEL_CAMERA_OUTPUTS_MAX) {
        return cam_fail(error, KEEL_E_AGAIN, KEEL_CAMERA_RESOURCE_NETWORK, 0, url,
                        "%u outputs are running: stop one first", (unsigned)KEEL_CAMERA_OUTPUTS_MAX);
    }
    rc = cam_destination_parse(url, session->encoder_settings.codec, &destination, error);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (resolved.max_clients != 0 && destination.kind != CAM_DEST_TCP_LISTEN) {
        return cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, url,
                        "max_clients applies only to tcp://...?listen outputs");
    }
    if (resolved.datagram_bytes != 0 && destination.kind != CAM_DEST_UDP) {
        return cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, url,
                        "datagram_bytes applies only to udp:// outputs");
    }
    if (resolved.queue == 0) {
        resolved.queue = KEEL_CAMERA_OUTPUT_QUEUE_DEFAULT;
    }
    if (resolved.max_clients == 0) {
        resolved.max_clients = KEEL_CAMERA_CLIENTS_DEFAULT;
    }
    if (resolved.datagram_bytes == 0) {
        resolved.datagram_bytes = KEEL_CAMERA_DATAGRAM_DEFAULT;
    }
    rc = cam_output_open(&destination, session->encoder_settings.codec, &resolved, s->width, s->height,
                         s->framerate_milli, request_keyframe, session, &output, error);
    if (rc != KEEL_OK) {
        return rc;
    }
    output->id = session->next_output_id++;
    pthread_mutex_lock(&session->lock);
    session->outputs[slot] = output;
    pthread_mutex_unlock(&session->lock);
    /* A receiver joining a running stream needs a keyframe now. */
    atomic_store(&session->keyframe_wanted, 1);
    *out_id = output->id;
    return KEEL_OK;
}

static int find_output(const cam_session *session, uint32_t id)
{
    int i;
    for (i = 0; i < KEEL_CAMERA_OUTPUTS_MAX; ++i) {
        if (session->outputs[i] != NULL && session->outputs[i]->id == id) {
            return i;
        }
    }
    return -1;
}

int32_t cam_session_output_stop(cam_session *session, uint32_t id, cam_error *error)
{
    cam_output *output;
    cam_error failure;
    int32_t status;
    int slot = find_output(session, id);
    if (slot < 0) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_OPTIONS, 0, NULL, "there is no output %u", id);
    }
    pthread_mutex_lock(&session->lock);
    output = session->outputs[slot];
    session->outputs[slot] = NULL;
    pthread_mutex_unlock(&session->lock);
    pthread_mutex_lock(&output->lock);
    failure = output->failure;
    pthread_mutex_unlock(&output->lock);
    status = cam_output_close(output);
    if (status != KEEL_OK) {
        if (failure.status == KEEL_OK) {
            cam_fail(error, KEEL_E_PERIPHERAL, KEEL_CAMERA_RESOURCE_NETWORK, 0, NULL,
                     "output %u could not finish cleanly", id);
        } else {
            *error = failure;
        }
        error->status = KEEL_E_PERIPHERAL;
        return KEEL_E_PERIPHERAL;
    }
    return KEEL_OK;
}

int32_t cam_session_output_stats(cam_session *session, uint32_t id, keel_camera_output_stats *out,
                                 cam_error *error)
{
    int slot = find_output(session, id);
    if (slot < 0) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_OPTIONS, 0, NULL, "there is no output %u", id);
    }
    cam_output_stats(session->outputs[slot], out);
    return KEEL_OK;
}

/* ---- stills ------------------------------------------------------------- */

typedef struct still_capture {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint32_t seen;
    uint32_t settle;
    int hold;            /* keep the next frame */
    cam_frame *frame;
    cam_error failure;
    int failed;
} still_capture;

static void still_frame(void *context, cam_frame *frame)
{
    still_capture *still = context;
    pthread_mutex_lock(&still->lock);
    still->seen++;
    if (still->hold && still->frame == NULL && still->seen > still->settle) {
        still->frame = frame;
        frame = NULL;
    }
    pthread_cond_broadcast(&still->cond);
    pthread_mutex_unlock(&still->lock);
    if (frame != NULL) {
        cam_frame_unref(frame);
    }
}

static void still_failure(void *context, const cam_error *error)
{
    still_capture *still = context;
    pthread_mutex_lock(&still->lock);
    if (!still->failed) {
        still->failed = 1;
        still->failure = *error;
    }
    pthread_cond_broadcast(&still->cond);
    pthread_mutex_unlock(&still->lock);
}

/* Waits until predicate holds; 0, or -1 on failure or timeout. */
static int still_wait(still_capture *still, const struct timespec *deadline, int (*ready)(const still_capture *))
{
    int rc = 0;
    pthread_mutex_lock(&still->lock);
    while (!ready(still) && !still->failed) {
        if (pthread_cond_timedwait(&still->cond, &still->lock, deadline) == ETIMEDOUT) {
            rc = -1;
            break;
        }
    }
    if (still->failed) {
        rc = -1;
    }
    pthread_mutex_unlock(&still->lock);
    return rc;
}

static int settled(const still_capture *still)
{
    return still->seen >= still->settle;
}

static int captured(const still_capture *still)
{
    return still->frame != NULL;
}

int32_t cam_session_still(cam_session *session, const cam_still_request *request, keel_camera_still_info *out,
                          cam_error *error)
{
    const keel_camera_info *info = &session->desc.info;
    cam_stream stream;
    still_capture still;
    struct timespec deadline;
    uint32_t max_width = 0;
    uint32_t max_height = 0;
    uint8_t *jpeg = NULL;
    uint32_t jpeg_bytes = 0;
    int software;
    int32_t rc;
    if (session->state != KEEL_CAMERA_STATE_STOPPED) {
        return cam_fail(error, KEEL_E_STATE, KEEL_CAMERA_RESOURCE_CAMERA, 0, NULL,
                        "a still needs capture stopped: call keel_camera_stop() first");
    }
    memset(&stream, 0, sizeof(stream));
    stream.width = request->width != 0 ? request->width : info->pixel_array_width;
    stream.height = request->height != 0 ? request->height : info->pixel_array_height;
    stream.format = KEEL_CAMERA_FORMAT_I420;
    stream.hflip = session->config.stream.hflip;
    stream.vflip = session->config.stream.vflip;
    stream.buffer_count = STILL_BUFFERS;
    stream.still = 1;
    rc = session->source_ops->configure(session->source, &stream, error);
    if (rc != KEEL_OK) {
        return rc;
    }
    /* The hardware image encoder, unless the still is larger than it takes
     * or it is missing; then libjpeg. */
    rc = session->encoder_ops->probe(KEEL_CAMERA_CODEC_MJPEG, &max_width, &max_height, error);
    software = rc != KEEL_OK || stream.width > max_width || stream.height > max_height;
    if (software && !cam_jpeg_software_available()) {
        if (rc != KEEL_OK) {
            return rc;
        }
        return cam_jpeg_software(NULL, &stream, request->quality, &jpeg, &jpeg_bytes, error);
    }
    cam_error_clear(error);
    memset(&still, 0, sizeof(still));
    pthread_mutex_init(&still.lock, NULL);
    cam_cond_init(&still.cond);
    still.settle = STILL_SETTLE_FRAMES;
    cam_deadline(&deadline, request->timeout_ms);
    rc = session->source_ops->start(session->source, &stream, &session->config.controls, still_frame,
                                    still_failure, &still, error);
    if (rc == KEEL_OK) {
        if (still_wait(&still, &deadline, settled) == 0 && (info->flags & KEEL_CAMERA_INFO_AUTOFOCUS) &&
            session->config.controls.af_mode != KEEL_CAMERA_AF_MANUAL) {
            uint32_t focus;
            cam_error ignored;
            (void)session->source_ops->autofocus(session->source, STILL_FOCUS_MS, &focus, &ignored);
        }
        pthread_mutex_lock(&still.lock);
        still.hold = 1;
        still.settle = still.seen;
        pthread_mutex_unlock(&still.lock);
        if (still_wait(&still, &deadline, captured) != 0 && !still.failed) {
            rc = cam_fail(error, KEEL_E_AGAIN, KEEL_CAMERA_RESOURCE_CAMERA, ETIMEDOUT, NULL,
                          "no still frame within %u ms", request->timeout_ms);
        }
        session->source_ops->stop(session->source);
        if (still.failed) {
            *error = still.failure;
            rc = still.failure.status;
        }
        if (rc == KEEL_OK) {
            rc = software ? cam_jpeg_software(still.frame, &stream, request->quality, &jpeg, &jpeg_bytes, error)
                          : session->encoder_ops->encode_jpeg(still.frame, &stream, request->quality, &jpeg,
                                                              &jpeg_bytes, error);
        }
        if (rc == KEEL_OK) {
            out->width = stream.width;
            out->height = stream.height;
            out->bytes = jpeg_bytes;
            out->flags = software ? KEEL_CAMERA_STILL_SOFTWARE : 0u;
            out->timestamp_ns = still.frame->timestamp_ns;
        }
        if (still.frame != NULL) {
            cam_frame_unref(still.frame);
        }
        session->source_ops->free_buffers(session->source);
    }
    pthread_cond_destroy(&still.cond);
    pthread_mutex_destroy(&still.lock);
    if (rc == KEEL_OK) {
        if (request->fd >= 0) {
            int written = cam_write_all(request->fd, jpeg, jpeg_bytes);
            if (written != 0) {
                rc = cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_FILE, written, NULL,
                              "writing the still failed: %s", strerror(written));
            }
        } else if (jpeg_bytes > request->size) {
            rc = cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, NULL,
                          "the buffer has %u bytes; this still needs %u", request->size, jpeg_bytes);
        } else {
            memcpy(request->buffer, jpeg, jpeg_bytes);
        }
    }
    free(jpeg);
    return rc;
}
