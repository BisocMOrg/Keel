/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Internal interfaces of libkeel-camera (SPEC section 26). The public API in
 * camera_api.c validates arguments and owns the handles; behind it:
 *
 *   capture source  a camera: libcamera (capture_libcamera.cpp) or, in test
 *                   builds, the synthetic camera (capture_synthetic.c)
 *   encoder         the V4L2 memory-to-memory encoder (encoder_v4l2.c) or,
 *                   in test builds, the synthetic encoder (encoder_synthetic.c)
 *   pipeline        frames, packets and their bounded queues (pipeline.c)
 *   outputs         files and sockets (output.c), libavformat (output_libav.c)
 *   stills          one JPEG: the still request (camera_session.c), on the
 *                   hardware encoder (encoder_v4l2.c) or libjpeg (jpeg_software.c)
 *
 * Frames are buffers of the capture source, reference-counted while the
 * program's queue and the encoder use them; the last reference returns the
 * buffer to the camera. Packets are encoded frames, reference-counted while
 * the program's queue and the outputs hold them, and recycled by a pool.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#ifndef KEEL_CAMERA_INTERNAL_H
#define KEEL_CAMERA_INTERNAL_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* The libcamera glue is C++: the same structs, with std::atomic, which has
 * the size and alignment of C11 _Atomic for these integer types. */
#ifdef __cplusplus
#include <atomic>
#define CAM_ATOMIC(type) std::atomic<type>
#else
#include <stdatomic.h>
#define CAM_ATOMIC(type) _Atomic type
#endif

#include "keel/camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Functions shared between the library's translation units. */
#if defined(__GNUC__) || defined(__clang__)
#define CAM_INTERNAL __attribute__((visibility("hidden")))
#define CAM_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define CAM_INTERNAL
#define CAM_PRINTF(f, a)
#endif

enum {
    CAM_MODES_MAX = 32,     /* sensor modes kept per camera */
    CAM_CAMERAS_MAX = 16,   /* cameras kept per listing */
    CAM_PLANES_MAX = 3,
    CAM_EXTRA_BUFFERS = 4,  /* capture buffers beyond the program's frame queue */
    CAM_ENCODER_PENDING = 2 /* frames waiting for the encoder */
};

/* ---- errors ------------------------------------------------------------- */

typedef struct cam_error {
    int32_t status;
    uint32_t resource;
    int32_t os_error;
    char path[KEEL_CAMERA_PATH_MAX];
    char message[KEEL_CAMERA_MESSAGE_MAX];
} cam_error;

/* Records a failure and returns status. path may be NULL. */
CAM_INTERNAL int32_t cam_fail(cam_error *error, int32_t status, uint32_t resource, int os_error,
                              const char *path, const char *format, ...) CAM_PRINTF(6, 7);
CAM_INTERNAL void cam_error_clear(cam_error *error);
/* Status for an errno from open() of a device node or a file. */
CAM_INTERNAL int32_t cam_status_for_errno(int os_error);
/* Copies text into a fixed field, always NUL-terminated (cut if needed). */
CAM_INTERNAL void cam_copy_string(char *out, size_t size, const char *text);
/* CLOCK_MONOTONIC in nanoseconds. */
CAM_INTERNAL uint64_t cam_now_ns(void);
/* Absolute CLOCK_MONOTONIC deadline timeout_ms from now, for pthread_cond_timedwait. */
CAM_INTERNAL void cam_deadline(struct timespec *out, uint32_t timeout_ms);
/* Message for a permission failure on a device node. */
CAM_INTERNAL int32_t cam_fail_permission(cam_error *error, uint32_t resource, int os_error,
                                         const char *path);
/* Creates a joinable thread with every signal blocked (the library never
 * handles signals) and a short name; returns an errno. */
CAM_INTERNAL int cam_thread_start(pthread_t *thread, const char *name, void *(*run)(void *), void *arg);
/* Initialises a condition variable on CLOCK_MONOTONIC. */
CAM_INTERNAL void cam_cond_init(pthread_cond_t *cond);

/* ---- frames and capture sources ----------------------------------------- */

typedef struct cam_source cam_source;

/* One capture buffer while the pipeline uses it. Planes are contiguous in
 * one buffer, at the offsets of the stream configuration. */
typedef struct cam_frame {
    cam_source *source;
    uint32_t index;          /* buffer index within the source */
    CAM_ATOMIC(uint32_t) refs;   /* pipeline references; the last returns it */
    uint8_t *data;           /* CPU mapping of the whole frame */
    uint32_t bytes;          /* bytes of the frame */
    int fd;                  /* dma-buf of the frame, or -1 */
    uint32_t fd_length;      /* bytes of the dma-buf */
    uint64_t sequence;
    uint64_t timestamp_ns;
} cam_frame;

/* A camera as a source reports it, with its modes. */
typedef struct cam_camera_desc {
    keel_camera_info info;
    keel_camera_mode modes[CAM_MODES_MAX];
} cam_camera_desc;

/* Requested stream; configure() fills in the result. */
typedef struct cam_stream {
    uint32_t width;
    uint32_t height;
    uint32_t format;          /* keel_camera_format */
    uint32_t framerate_milli;
    uint32_t hflip;           /* orientation after rotation is folded in */
    uint32_t vflip;
    uint32_t buffer_count;
    uint32_t still;           /* 1: the still-capture role */
    /* results */
    uint32_t plane_count;
    uint32_t stride[CAM_PLANES_MAX];
    uint32_t offset[CAM_PLANES_MAX];
    uint32_t frame_bytes;
} cam_stream;

/* Image controls, already validated against the camera. */
typedef struct cam_controls {
    uint32_t af_mode;
    uint32_t af_range;
    uint32_t lens_position_milli;
    uint32_t exposure_us;
    uint32_t gain_milli;
    int32_t ev_milli;
    uint32_t awb_mode;
    uint32_t red_gain_milli;
    uint32_t blue_gain_milli;
} cam_controls;

/* Receives each captured frame with one reference, on a source thread. */
typedef void (*cam_frame_sink)(void *context, cam_frame *frame);
/* Reports a capture failure on a source thread. */
typedef void (*cam_failure_sink)(void *context, const cam_error *error);

typedef struct cam_source_ops {
    const char *name;
    /* Lists the cameras (at most capacity). OK with *count 0 when there is
     * none; a failure explains a camera that exists but cannot be used. */
    int32_t (*list)(cam_camera_desc *cameras, uint32_t capacity, uint32_t *count, cam_error *error);
    /* Acquires a camera for this process. */
    int32_t (*open)(uint32_t index, cam_source **out, cam_camera_desc *desc, cam_error *error);
    /* Validates a stream and fills in its layout; allocates nothing. */
    int32_t (*configure)(cam_source *source, cam_stream *stream, cam_error *error);
    /* Configures the camera with a validated stream, allocates buffers and
     * starts delivering frames to sink. */
    int32_t (*start)(cam_source *source, const cam_stream *stream, const cam_controls *controls,
                     cam_frame_sink sink, cam_failure_sink failure, void *context, cam_error *error);
    /* Returns a frame to the camera; any thread. */
    void (*release)(cam_source *source, cam_frame *frame);
    /* Waits for an autofocus result (triggering a scan in AUTO mode). */
    int32_t (*autofocus)(cam_source *source, uint32_t timeout_ms, uint32_t *state, cam_error *error);
    /* Stops delivery; frames still referenced are returned later. */
    void (*stop)(cam_source *source);
    /* Frees the buffers once every frame is back (after stop). */
    void (*free_buffers)(cam_source *source);
    /* Releases the camera. */
    void (*close)(cam_source *source);
} cam_source_ops;

struct cam_source {
    const cam_source_ops *ops;
};

/* Gives a frame its first reference, before a source passes it to the sink. */
CAM_INTERNAL void cam_frame_deliver(cam_frame *frame);
CAM_INTERNAL void cam_frame_ref(cam_frame *frame);
CAM_INTERNAL void cam_frame_unref(cam_frame *frame);
/* Copies a frame to dst (bytes of the frame), with dma-buf CPU sync. */
CAM_INTERNAL void cam_frame_copy(const cam_frame *frame, uint8_t *dst);
/* Bytes of a frame whose planes have height lines of stride bytes (the
 * chroma planes of I420 half of each). */
CAM_INTERNAL uint32_t cam_coded_frame_bytes(uint32_t format, uint32_t stride, uint32_t height);
/* Copies a frame laid out with stride and height (the ISP layout) into dst,
 * laid out with dst_stride and dst_height, as an encoder that pads its
 * buffers needs; the extra lines of each plane repeat its last line. */
CAM_INTERNAL void cam_frame_copy_coded(const cam_frame *frame, uint32_t format, uint32_t stride, uint32_t height,
                                       uint8_t *dst, uint32_t dst_stride, uint32_t dst_height);

/* Fills in plane_count, stride, offset and frame_bytes for a stream whose
 * rows are padded to align bytes (the Raspberry Pi ISP layout). */
CAM_INTERNAL void cam_stream_layout(cam_stream *stream, uint32_t align);

/* Product name for a sensor ("Camera Module 3 Wide" for imx708_wide). */
CAM_INTERNAL const char *cam_model_name(const char *sensor);

extern CAM_INTERNAL const cam_source_ops cam_libcamera_source;
/* libcamera's version ("v0.7.0"), for keel_camera_version_string(). */
CAM_INTERNAL const char *cam_libcamera_version(void);
#ifdef KEEL_CAMERA_TEST_HOOKS
extern CAM_INTERNAL const cam_source_ops cam_test_source;
/* The synthetic camera and encoder are selected (KEEL_CAMERA_TEST_SOURCE=1). */
CAM_INTERNAL int cam_test_selected(void);
/* The fault KEEL_CAMERA_TEST_FAULT names, or "" (see capture_synthetic.c). */
CAM_INTERNAL int cam_test_fault(const char *name);
#endif

/* Explains an empty camera list from the device nodes under dev_root
 * ("/dev"): KEEL_E_PERMISSION for an inaccessible node, KEEL_E_NOT_FOUND
 * when a sensor is attached but libcamera did not register it, KEEL_OK
 * when no camera is connected. ipa_dir is the directory of libcamera's IPA
 * modules, or NULL when unknown. */
CAM_INTERNAL int32_t cam_diagnose_no_camera(const char *dev_root, const char *ipa_dir, cam_error *error);

/* ---- packets ------------------------------------------------------------ */

typedef struct cam_packet_pool cam_packet_pool;

/* One encoded chunk, shared by the queues that hold it. */
typedef struct cam_packet {
    CAM_ATOMIC(uint32_t) refs;
    uint32_t flags;          /* keel_camera_encoded_flag */
    uint32_t bytes;
    uint32_t header_bytes;   /* leading SPS and PPS bytes (H.264), else 0 */
    uint32_t capacity;
    uint32_t codec;
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint8_t *data;
    cam_packet_pool *pool;
    struct cam_packet *next_free;
} cam_packet;

struct cam_packet_pool {
    pthread_mutex_t lock;
    cam_packet *free_list;
    uint32_t free_count;
    CAM_ATOMIC(uint32_t) live;   /* packets handed out and not yet back */
    int closed;              /* the pool frees packets as they come back */
};

CAM_INTERNAL void cam_packet_pool_init(cam_packet_pool *pool);
/* Takes a packet with room for bytes (refs 1), or NULL. */
CAM_INTERNAL cam_packet *cam_packet_get(cam_packet_pool *pool, uint32_t bytes);
CAM_INTERNAL void cam_packet_ref(cam_packet *packet);
CAM_INTERNAL void cam_packet_unref(cam_packet *packet);
/* Frees the idle packets and closes the pool: a packet still referenced is
 * freed by its last unref instead of returning to the pool, so the pool must
 * outlive every packet. */
CAM_INTERNAL void cam_packet_pool_close(cam_packet_pool *pool);

/* Scans H.264 Annex-B data: keyframe and header flags, and the length of
 * the leading SPS/PPS NAL units. */
CAM_INTERNAL uint32_t cam_h264_scan(const uint8_t *data, uint32_t bytes, uint32_t *header_bytes);
/* Copies H.264 Annex-B data, adding a full-range BT.601 video signal type
 * to every SPS without one; returns the length, or 0 if out is too small. */
CAM_INTERNAL uint32_t cam_h264_signal_full_range(const uint8_t *in, uint32_t bytes, uint8_t *out,
                                                 uint32_t capacity);

/* Bounded packet queue. H.264 queues drop everything and resume at the next
 * keyframe; MJPEG queues drop the oldest packet. */
typedef struct cam_packet_queue {
    cam_packet **items;
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
    uint32_t codec;
    int need_keyframe;
    uint64_t dropped;
} cam_packet_queue;

CAM_INTERNAL int cam_packet_queue_init(cam_packet_queue *queue, uint32_t capacity, uint32_t codec);
/* Takes a reference when queued; returns 1 if queued, 0 if dropped. */
CAM_INTERNAL int cam_packet_queue_push(cam_packet_queue *queue, cam_packet *packet);
/* Returns the oldest packet (the caller owns its reference) or NULL. */
CAM_INTERNAL cam_packet *cam_packet_queue_pop(cam_packet_queue *queue);
CAM_INTERNAL cam_packet *cam_packet_queue_peek(const cam_packet_queue *queue);
CAM_INTERNAL void cam_packet_queue_clear(cam_packet_queue *queue);
CAM_INTERNAL void cam_packet_queue_free(cam_packet_queue *queue);

/* Bounded frame queue that drops the oldest frame. */
typedef struct cam_frame_queue {
    cam_frame *items[KEEL_CAMERA_FRAME_QUEUE_MAX + CAM_ENCODER_PENDING];
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
} cam_frame_queue;

/* Queues a frame (taking a reference); returns the frame it pushed out or NULL. */
CAM_INTERNAL cam_frame *cam_frame_queue_push(cam_frame_queue *queue, cam_frame *frame);
CAM_INTERNAL cam_frame *cam_frame_queue_pop(cam_frame_queue *queue);

/* ---- encoders ----------------------------------------------------------- */

typedef struct cam_encoder_ops cam_encoder_ops;

typedef struct cam_encoder {
    const cam_encoder_ops *ops;
} cam_encoder;

typedef struct cam_encoder_config {
    uint32_t codec;
    uint32_t width;
    uint32_t height;
    uint32_t format;          /* keel_camera_format of the input frames */
    uint32_t stride;          /* bytes per row of plane 0 */
    uint32_t frame_bytes;
    uint32_t framerate_milli;
    uint32_t bitrate;         /* H.264, resolved (never 0) */
    uint32_t intra_period;    /* H.264, resolved (never 0) */
    uint32_t profile;         /* keel_camera_h264_profile */
    uint32_t quality;         /* MJPEG, resolved (never 0) */
    uint32_t use_dmabuf;      /* frames carry dma-bufs the encoder imports */
} cam_encoder_config;

/* Receives each encoded chunk on the encoder thread. */
typedef void (*cam_packet_sink)(void *context, const uint8_t *data, uint32_t bytes, int keyframe,
                                uint64_t sequence, uint64_t timestamp_ns);
/* Reports an encoder failure on the encoder thread. */
typedef void (*cam_encoder_failure)(void *context, const cam_error *error);

struct cam_encoder_ops {
    const char *name;
    /* Finds the encoder for codec; reports its largest frame. */
    int32_t (*probe)(uint32_t codec, uint32_t *max_width, uint32_t *max_height, cam_error *error);
    /* Opens and starts it, with its own thread. */
    int32_t (*open)(const cam_encoder_config *config, cam_packet_sink sink, cam_encoder_failure failure,
                    void *context, cam_encoder **out, cam_error *error);
    /* Hands over a frame (the encoder takes a reference); never blocks.
     * Returns 1 when an older pending frame had to be dropped. */
    int (*submit)(cam_encoder *encoder, cam_frame *frame);
    /* Makes the next frame a keyframe. */
    void (*force_keyframe)(cam_encoder *encoder);
    /* Stops the thread and returns every frame it holds. */
    void (*close)(cam_encoder *encoder);
    /* Encodes one frame to a JPEG image synchronously (stills). */
    int32_t (*encode_jpeg)(const cam_frame *frame, const cam_stream *stream, uint32_t quality,
                           uint8_t **out, uint32_t *out_bytes, cam_error *error);
};

extern CAM_INTERNAL const cam_encoder_ops cam_v4l2_encoder;
#ifdef KEEL_CAMERA_TEST_HOOKS
extern CAM_INTERNAL const cam_encoder_ops cam_test_encoder;
#endif

/* Default H.264 bit rate: about 0.1 bit per pixel and frame. */
CAM_INTERNAL uint32_t cam_default_bitrate(uint32_t width, uint32_t height, uint32_t framerate_milli);

/* Software JPEG of an I420 frame (libjpeg); KEEL_E_UNSUPPORTED without it. */
CAM_INTERNAL int32_t cam_jpeg_software(const cam_frame *frame, const cam_stream *stream, uint32_t quality,
                                       uint8_t **out, uint32_t *out_bytes, cam_error *error);
CAM_INTERNAL int cam_jpeg_software_available(void);

/* ---- outputs ------------------------------------------------------------ */

enum cam_destination_kind {
    CAM_DEST_FILE = 1,       /* raw elementary stream */
    CAM_DEST_TCP = 2,        /* connect */
    CAM_DEST_TCP_LISTEN = 3, /* accept receivers */
    CAM_DEST_UDP = 4,
    CAM_DEST_LIBAV = 5       /* container file or protocol URL */
};

typedef struct cam_destination {
    uint32_t kind;
    char host[KEEL_CAMERA_PATH_MAX];  /* tcp/udp host or listen address ("" = any) */
    uint16_t port;
    char path[KEEL_CAMERA_PATH_MAX];  /* file path or libav URL */
    const char *libav_format;         /* libavformat muxer name */
} cam_destination;

/* Parses a destination for a codec. */
CAM_INTERNAL int32_t cam_destination_parse(const char *url, uint32_t codec, cam_destination *out, cam_error *error);

typedef struct cam_output cam_output;

/* Asks the encoder for a keyframe when a receiver joins (any thread). */
typedef void (*cam_keyframe_request)(void *context);

typedef struct cam_output_ops {
    /* Writes one packet (stream kinds); returns 0, or sets the failure. */
    int (*write)(cam_output *output, const cam_packet *packet);
    /* Runs after the output thread ended: trailers, then close. */
    void (*finish)(cam_output *output);
} cam_output_ops;

struct cam_output {
    const cam_output_ops *ops;
    uint32_t id;
    uint32_t codec;
    cam_destination destination;
    keel_camera_output_options options;
    uint32_t width;
    uint32_t height;
    uint32_t framerate_milli;
    cam_keyframe_request request_keyframe;
    void *request_context;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    int thread_started;
    int thread_done;
    int stopping;
    cam_packet_queue queue;    /* stream kinds */
    int fd;                    /* file or socket */
    int wake_fd;               /* eventfd of the listening output */
    void *kind_state;          /* listening clients, UDP address, libavformat */
    /* counters, under lock */
    uint32_t state;            /* keel_camera_output_state */
    uint32_t clients;
    uint64_t bytes;
    uint64_t chunks;
    uint64_t chunks_dropped;
    cam_error failure;
};

/* Opens the destination (file, connect or bind) and starts the output's thread. */
CAM_INTERNAL int32_t cam_output_open(const cam_destination *destination, uint32_t codec,
                                     const keel_camera_output_options *options, uint32_t width,
                                     uint32_t height, uint32_t framerate_milli,
                                     cam_keyframe_request request_keyframe, void *request_context,
                                     cam_output **out, cam_error *error);
/* Queues a packet for the output; never blocks. */
CAM_INTERNAL void cam_output_push(cam_output *output, cam_packet *packet);
/* Fills the public counters. */
CAM_INTERNAL void cam_output_stats(cam_output *output, keel_camera_output_stats *out);
/* Stops and frees the output; returns KEEL_OK or the status that failed it. */
CAM_INTERNAL int32_t cam_output_close(cam_output *output);
/* Marks the output failed (lock held by the caller). */
CAM_INTERNAL void cam_output_failed_locked(cam_output *output, int32_t status, int os_error,
                                           const char *format, ...) CAM_PRINTF(4, 5);

/* Sets output->ops and kind_state for a libavformat destination. */
CAM_INTERNAL int32_t cam_output_libav_open(cam_output *output, cam_error *error);
CAM_INTERNAL int cam_libav_available(void);

/* Sends all bytes on a socket with MSG_NOSIGNAL; returns 0 or an errno. */
CAM_INTERNAL int cam_send_all(int fd, const uint8_t *data, size_t bytes);
/* Writes all bytes to a file; returns 0 or an errno. */
CAM_INTERNAL int cam_write_all(int fd, const uint8_t *data, size_t bytes);

/* ---- conversion --------------------------------------------------------- */

/* The I420 to packed RGB conversion behind keel_camera_convert_i420(). */
CAM_INTERNAL void cam_convert_i420(const uint8_t *y, uint32_t y_stride, const uint8_t *u, uint32_t u_stride,
                                   const uint8_t *v, uint32_t v_stride, uint32_t width, uint32_t height,
                                   uint8_t *out, uint32_t out_stride, int bgr);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_CAMERA_INTERNAL_H */
