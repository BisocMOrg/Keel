/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * One open camera (SPEC section 26.4): its capture source, frame queue,
 * encoder, packet queue, outputs and stills. camera_api.c validates every
 * argument, owns the handle tokens and calls these functions on the owning
 * thread only, except cam_session_wake(), which a closing thread calls.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#ifndef KEEL_CAMERA_SESSION_H
#define KEEL_CAMERA_SESSION_H

#include "camera/camera_internal.h"

typedef struct cam_session cam_session;

/* Image and stream settings resolved from keel_camera_options. */
typedef struct cam_session_config {
    uint32_t index;
    cam_stream stream;        /* width, height, format, framerate, flips */
    cam_controls controls;
    uint32_t frame_queue;     /* 1..KEEL_CAMERA_FRAME_QUEUE_MAX */
} cam_session_config;

/* Encoder settings with every default resolved. */
typedef struct cam_session_encoder {
    uint32_t codec;
    uint32_t bitrate;
    uint32_t intra_period;
    uint32_t profile;
    uint32_t flags;
    uint32_t quality;
    uint32_t encoded_queue;
} cam_session_encoder;

CAM_INTERNAL int32_t cam_session_open(const cam_source_ops *source_ops, const cam_encoder_ops *encoder_ops,
                                      const cam_session_config *config, cam_session **out, cam_error *error);
/* Returns KEEL_OK, or KEEL_E_PERIPHERAL when an output did not finish cleanly. */
CAM_INTERNAL int32_t cam_session_close(cam_session *session);
/* Wakes blocked reads for a close from another thread; they return
 * KEEL_E_INVALID_HANDLE. */
CAM_INTERNAL void cam_session_wake(cam_session *session);

CAM_INTERNAL const cam_camera_desc *cam_session_desc(const cam_session *session);
/* The configuration as opened, with the stream the camera accepted. */
CAM_INTERNAL const cam_session_config *cam_session_config_of(const cam_session *session);
CAM_INTERNAL void cam_session_stream_info(cam_session *session, keel_camera_stream_info *out);
CAM_INTERNAL void cam_session_stats(cam_session *session, keel_camera_stats *out);
/* Copies a capture failure not reported before; returns 1 if there was one. */
CAM_INTERNAL int cam_session_take_failure(cam_session *session, cam_error *out);
CAM_INTERNAL int32_t cam_session_set_encoder(cam_session *session, const cam_session_encoder *encoder,
                                             cam_error *error);
CAM_INTERNAL int32_t cam_session_start(cam_session *session, cam_error *error);
CAM_INTERNAL void cam_session_stop(cam_session *session);
CAM_INTERNAL int32_t cam_session_read_frame(cam_session *session, uint8_t *buffer, uint32_t size,
                                            uint32_t timeout_ms, keel_camera_frame_info *out, cam_error *error);
CAM_INTERNAL int32_t cam_session_read_encoded(cam_session *session, uint8_t *buffer, uint32_t size,
                                              uint32_t timeout_ms, keel_camera_encoded_info *out,
                                              cam_error *error);
CAM_INTERNAL int32_t cam_session_autofocus(cam_session *session, uint32_t timeout_ms, uint32_t *state,
                                           cam_error *error);
CAM_INTERNAL int32_t cam_session_output_start(cam_session *session, const char *url,
                                              const keel_camera_output_options *options, uint32_t *out_id,
                                              cam_error *error);
CAM_INTERNAL int32_t cam_session_output_stop(cam_session *session, uint32_t id, cam_error *error);
CAM_INTERNAL int32_t cam_session_output_stats(cam_session *session, uint32_t id, keel_camera_output_stats *out,
                                              cam_error *error);

/* A still: into a buffer (fd < 0) or a file descriptor. */
typedef struct cam_still_request {
    uint32_t width;           /* 0: full resolution */
    uint32_t height;
    uint32_t quality;         /* resolved */
    uint32_t timeout_ms;      /* resolved */
    uint8_t *buffer;
    uint32_t size;
    int fd;
} cam_still_request;

CAM_INTERNAL int32_t cam_session_still(cam_session *session, const cam_still_request *request,
                                       keel_camera_still_info *out, cam_error *error);

#endif /* KEEL_CAMERA_SESSION_H */
