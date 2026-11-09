/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file camera.h
 * @brief libkeel-camera API, ABI 1.0: Raspberry Pi cameras on the robot's
 *        Raspberry Pi 4, for C programs and the Python and Java bindings.
 *
 * The camera connects to the Raspberry Pi's camera connector, not to the Keel
 * board, so this is a library of its own: @c libkeel-camera.so.1. It needs
 * neither @c libkeel.so nor a device opened with keel_open(); it shares only
 * the status codes and macros of keel.h. Camera Module 2 (IMX219, fixed
 * focus) and Camera Module 3 (IMX708, autofocus; also its wide and NoIR
 * variants) are supported; any other camera libcamera drives works for
 * capture. The runtime dependencies are libcamera with its Raspberry Pi IPA
 * module and the kernel's V4L2 devices. Larger-than-1920 stills also use
 * libjpeg, and container and streaming-protocol outputs FFmpeg's
 * libavformat, when the library was built with them.
 *
 * @par Data flow
 * One handle drives one camera. Its frames feed, at the same time:
 * - the raw frame queue, read with keel_camera_read_frame();
 * - one hardware encoder (H.264 or MJPEG, keel_camera_set_encoder()), whose
 *   chunks feed the encoded queue (keel_camera_read_encoded()) and every
 *   output the library drives itself (keel_camera_output_start(): files,
 *   TCP, UDP and, with libavformat, containers and streaming protocols).
 * Every queue is bounded and drops instead of growing; the counters of
 * keel_camera_get_stats() and keel_camera_get_output_stats() count each
 * drop. Raw frames drop the oldest frame. H.264 queues drop everything queued
 * and resume at the next keyframe, so a reader always gets a decodable
 * stream; MJPEG queues drop the oldest chunk.
 *
 * @par Conventions (those of keel.h)
 * - Functions return a ::keel_status value as @c int32_t, unless they are
 *   metadata queries that cannot fail.
 * - Fields and parameters are fixed-width integers, fixed-size @c char arrays
 *   or pointers to them; fractional values are integers with a stated scale
 *   (@c _milli = value x 1000). Every struct is padded explicitly.
 * - Every constant is an enumerator; only the version numbers are macros.
 * - Every struct starts with <tt>uint32_t struct_size</tt> and follows the
 *   struct-versioning rule of keel.h: input structs larger than the library
 *   knows are accepted only with a zero tail (else ::KEEL_E_UNSUPPORTED, and
 *   ::KEEL_E_INVALID_ARGUMENT from keel_camera_open()); output structs are
 *   written up to the known size and zero-filled beyond it, and left
 *   untouched when a call returns ::KEEL_E_INVALID_ARGUMENT,
 *   ::KEEL_E_INVALID_HANDLE or ::KEEL_E_NOT_OWNER.
 * - Data goes into caller buffers with explicit sizes. A buffer that is too
 *   small is ::KEEL_E_INVALID_ARGUMENT with no effect: the frame or chunk
 *   stays queued. keel_camera_get_stream() gives the sizes that always fit.
 * - Strings returned are static, NUL-terminated UTF-8 and never NULL; strings
 *   passed in are copied during the call.
 * - No callbacks cross the ABI.
 *
 * @par Errors
 * Status codes keep the meaning and class keel.h gives them. Every failure
 * also records a ::keel_camera_error with the resource involved, the OS
 * error, the path and a one-line message that names the remedy, for example
 * "permission denied on /dev/media0: add the user to the video group (sudo
 * keel-setup --user NAME), then log in again". keel_camera_open() and
 * keel_camera_list_open() write it to their @c error argument;
 * keel_camera_last_error() returns the latest one of a handle. The codes:
 * - ::KEEL_E_NOT_FOUND: no camera, a camera index past the last one, the
 *   libcamera IPA module missing, the hardware encoder missing, or a network
 *   destination that refuses or does not answer.
 * - ::KEEL_E_IN_USE: the camera is open in another process or handle, or a
 *   listening port is taken.
 * - ::KEEL_E_PERMISSION: a camera, encoder or frame-buffer device node is not
 *   accessible (the video group), or a file cannot be created.
 * - ::KEEL_E_INVALID_ARGUMENT: a bad option or argument.
 * - ::KEEL_E_UNSUPPORTED: a feature this camera or build lacks: autofocus on
 *   a fixed-focus camera, a container or protocol output without
 *   libavformat, a still above the hardware encoder's size without libjpeg.
 * - ::KEEL_E_STATE: not allowed now, for example reading before
 *   keel_camera_start().
 * - ::KEEL_E_AGAIN: nothing arrived within the timeout; call again.
 * - ::KEEL_E_CONFLICT: called from a thread that does not own the handle.
 * - ::KEEL_E_SETUP_IO and ::KEEL_E_PERIPHERAL: the camera, the encoder or an
 *   output failed while starting or while running.
 * - ::KEEL_E_NO_MEMORY: an allocation failed.
 *
 * @par Threading
 * The thread that opened a handle owns it: every call on the handle from
 * another thread returns ::KEEL_E_CONFLICT with no effect, except
 * keel_camera_close(), which any thread may call. A close from another thread
 * wakes the owner's blocked reads, which return ::KEEL_E_INVALID_HANDLE.
 * Call it from an OS thread that stays the same (never a Java virtual
 * thread). Separate handles are independent; list handles have the same
 * rule. Calls that need no handle work from any thread. The library's own
 * threads (capture, encoder and one per output) all end in keel_camera_stop()
 * or keel_camera_close(), which also finish files and close sockets. The
 * library installs no signal handlers, opens every file descriptor
 * close-on-exec and sends with @c MSG_NOSIGNAL, so a receiver that goes away
 * never raises @c SIGPIPE. A forked child gets ::KEEL_E_NOT_OWNER from every
 * call on an inherited handle.
 *
 * @par Frames
 * Raw frames come straight from the image signal processor in the pixel
 * format asked for, so no CPU conversion happens. Their colour space is
 * full-range BT.601 (sYCC), which JPEG also uses. Rows may be padded: use the
 * stride and offset of each plane from ::keel_camera_frame_info. Timestamps
 * are the sensor's CLOCK_MONOTONIC start-of-exposure times in nanoseconds;
 * sequence numbers come from the sensor, so a gap means a frame the camera
 * dropped.
 *
 * @par Test source (test builds only)
 * A build configured with @c KEEL_CAMERA_TEST_HOOKS=ON (the CMake presets
 * do; packages and install.sh do not) honours the environment variable
 * @c KEEL_CAMERA_TEST_SOURCE=1, which replaces libcamera and the hardware
 * encoder with a synthetic camera 0 (an IMX219 Camera Module 2) and camera 1
 * (an IMX708 Camera Module 3) and a synthetic encoder. They exist for the
 * test suites of this library and its bindings; docs/spec.md section 26
 * describes them.
 */
#ifndef KEEL_CAMERA_H
#define KEEL_CAMERA_H

#include <stdint.h>

#include "keel.h"

/**
 * @defgroup keel_camera_macros Build macros
 * @brief Version numbers of this header, for @c \#if checks.
 * @{
 */

/** @brief Major version of camera.h; equals the ABI major and the SOVERSION of libkeel-camera. */
#define KEEL_CAMERA_VERSION_MAJOR 1
/** @brief Minor version of camera.h; equals the ABI minor (additive changes only). */
#define KEEL_CAMERA_VERSION_MINOR 0
/** @brief Patch version of camera.h; never affects the ABI. */
#define KEEL_CAMERA_VERSION_PATCH 0

/** @} */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/**
 * @defgroup keel_camera_constants Constants
 * @brief Enumerations. Values travel through the ABI as @c uint32_t or
 *        @c int32_t; the enum types themselves never appear in signatures.
 * @{
 */

/** @brief ABI identification, for comparison with keel_camera_abi_version(). */
enum keel_camera_abi {
    /** ABI major: incompatible changes. Equals the shared-library SOVERSION. */
    KEEL_CAMERA_ABI_MAJOR = KEEL_CAMERA_VERSION_MAJOR,
    /** ABI minor: additive changes (new functions, appended fields and values). */
    KEEL_CAMERA_ABI_MINOR = KEEL_CAMERA_VERSION_MINOR,
    /** Packed value returned by keel_camera_abi_version(): (major << 16) | minor. */
    KEEL_CAMERA_ABI_VERSION = (KEEL_CAMERA_VERSION_MAJOR << 16) | KEEL_CAMERA_VERSION_MINOR
};

/** @brief Pixel formats of raw frames (keel_camera_options::format). */
enum keel_camera_format {
    /** Planar YUV 4:2:0: a Y plane, then U (Cb) and V (Cr) planes of half
     *  the width and half the height. The default. */
    KEEL_CAMERA_FORMAT_I420 = 1,
    /** Packed 24-bit RGB, bytes R, G, B in memory (V4L2 RGB24; Python PIL
     *  and numpy RGB order). */
    KEEL_CAMERA_FORMAT_RGB888 = 2,
    /** Packed 24-bit BGR, bytes B, G, R in memory (V4L2 BGR24; OpenCV order). */
    KEEL_CAMERA_FORMAT_BGR888 = 3
};

/** @brief Bits of keel_camera_options::flags. */
enum keel_camera_flag {
    /** Mirror the image left to right. */
    KEEL_CAMERA_HFLIP = 1,
    /** Mirror the image top to bottom. */
    KEEL_CAMERA_VFLIP = 2
};

/** @brief Autofocus modes (keel_camera_options::af_mode). Only cameras with
 *  ::KEEL_CAMERA_INFO_AUTOFOCUS accept a value other than DEFAULT. */
enum keel_camera_af_mode {
    /** Continuous autofocus on a camera that has autofocus; nothing on a
     *  fixed-focus camera. */
    KEEL_CAMERA_AF_DEFAULT = 0,
    /** The lens stays at keel_camera_options::lens_position_milli. */
    KEEL_CAMERA_AF_MANUAL = 1,
    /** One focus scan when capture starts and on each
     *  keel_camera_trigger_autofocus(). */
    KEEL_CAMERA_AF_AUTO = 2,
    /** The camera refocuses whenever the scene changes. */
    KEEL_CAMERA_AF_CONTINUOUS = 3
};

/** @brief Autofocus search ranges (keel_camera_options::af_range). */
enum keel_camera_af_range {
    /** The camera's default range (NORMAL on Camera Module 3). */
    KEEL_CAMERA_AF_RANGE_DEFAULT = 0,
    /** From infinity down to the usual close distance. */
    KEEL_CAMERA_AF_RANGE_NORMAL = 1,
    /** Close objects only. */
    KEEL_CAMERA_AF_RANGE_MACRO = 2,
    /** The whole range of the lens. */
    KEEL_CAMERA_AF_RANGE_FULL = 3
};

/** @brief Result of keel_camera_trigger_autofocus(). */
enum keel_camera_af_state {
    /** No scan has run. */
    KEEL_CAMERA_AF_STATE_IDLE = 0,
    /** A scan was still running when the timeout ended. */
    KEEL_CAMERA_AF_STATE_SCANNING = 1,
    /** The scan found focus. */
    KEEL_CAMERA_AF_STATE_FOCUSED = 2,
    /** The scan ended without finding focus. */
    KEEL_CAMERA_AF_STATE_FAILED = 3
};

/** @brief White balance modes (keel_camera_options::awb_mode). */
enum keel_camera_awb {
    KEEL_CAMERA_AWB_AUTO = 0,         /**< Automatic white balance. The default. */
    KEEL_CAMERA_AWB_INCANDESCENT = 1, /**< Incandescent light. */
    KEEL_CAMERA_AWB_TUNGSTEN = 2,     /**< Tungsten light. */
    KEEL_CAMERA_AWB_FLUORESCENT = 3,  /**< Fluorescent light. */
    KEEL_CAMERA_AWB_INDOOR = 4,       /**< Indoor light. */
    KEEL_CAMERA_AWB_DAYLIGHT = 5,     /**< Daylight. */
    KEEL_CAMERA_AWB_CLOUDY = 6,       /**< Cloudy daylight. */
    /** Fixed colour gains keel_camera_options::red_gain_milli and
     *  keel_camera_options::blue_gain_milli. */
    KEEL_CAMERA_AWB_MANUAL = 7
};

/** @brief Bits of keel_camera_info::flags. */
enum keel_camera_info_flag {
    /** The camera has an autofocus lens (Camera Module 3). */
    KEEL_CAMERA_INFO_AUTOFOCUS = 1,
    /** Another process held the camera while it was listed, so the frame
     *  rates and crops of its modes are unknown (zero). */
    KEEL_CAMERA_INFO_IN_USE = 2
};

/** @brief Encoders (keel_camera_encoder_options::codec). */
enum keel_camera_codec {
    /** No encoder: raw frames only. */
    KEEL_CAMERA_CODEC_NONE = 0,
    /** H.264 in Annex-B byte-stream form, on the hardware video encoder. */
    KEEL_CAMERA_CODEC_H264 = 1,
    /** Motion JPEG: one baseline JPEG image per frame, on the hardware
     *  image encoder. */
    KEEL_CAMERA_CODEC_MJPEG = 2
};

/** @brief H.264 profiles (keel_camera_encoder_options::profile). */
enum keel_camera_h264_profile {
    /** High profile. */
    KEEL_CAMERA_H264_PROFILE_DEFAULT = 0,
    /** Constrained Baseline profile (for decoders that need it). */
    KEEL_CAMERA_H264_PROFILE_BASELINE = 1,
    /** Main profile. */
    KEEL_CAMERA_H264_PROFILE_MAIN = 2,
    /** High profile. */
    KEEL_CAMERA_H264_PROFILE_HIGH = 3
};

/** @brief Bits of keel_camera_encoder_options::flags. */
enum keel_camera_encoder_flag {
    /** H.264: repeat the sequence and picture parameter sets (SPS and PPS)
     *  before every keyframe of the chunks the program reads. Outputs the
     *  library drives always get them where a receiver can join. */
    KEEL_CAMERA_INLINE_HEADERS = 1
};

/** @brief Bits of keel_camera_encoded_info::flags. */
enum keel_camera_encoded_flag {
    /** The chunk starts a picture that decodes on its own (an H.264 IDR
     *  picture; every MJPEG chunk). */
    KEEL_CAMERA_ENCODED_KEYFRAME = 1,
    /** The chunk carries the H.264 sequence and picture parameter sets. */
    KEEL_CAMERA_ENCODED_HEADERS = 2
};

/** @brief Bits of keel_camera_still_info::flags. */
enum keel_camera_still_flag {
    /** The still was encoded with libjpeg, because it is larger than the
     *  hardware image encoder accepts. */
    KEEL_CAMERA_STILL_SOFTWARE = 1
};

/** @brief Capture state (keel_camera_stats::state). */
enum keel_camera_state {
    /** Not capturing: after keel_camera_open() or keel_camera_stop(). */
    KEEL_CAMERA_STATE_STOPPED = 0,
    /** Capturing: after keel_camera_start(). */
    KEEL_CAMERA_STATE_RUNNING = 1,
    /** The camera or the encoder failed while capturing; see
     *  keel_camera_last_error(). keel_camera_stop() returns to STOPPED. */
    KEEL_CAMERA_STATE_FAILED = 2
};

/** @brief State of an output (keel_camera_output_stats::state). */
enum keel_camera_output_state {
    /** Waiting for its first keyframe, or for a client to connect. */
    KEEL_CAMERA_OUTPUT_WAITING = 1,
    /** Writing or sending chunks. */
    KEEL_CAMERA_OUTPUT_RUNNING = 2,
    /** Stopped by an error (keel_camera_output_stats::status says which);
     *  the capture and the other outputs continue. */
    KEEL_CAMERA_OUTPUT_FAILED = 3
};

/** @brief Resource named by keel_camera_error::resource. */
enum keel_camera_resource {
    KEEL_CAMERA_RESOURCE_NONE = 0,      /**< No specific resource. */
    KEEL_CAMERA_RESOURCE_CAMERA = 1,    /**< The camera itself. */
    KEEL_CAMERA_RESOURCE_LIBCAMERA = 2, /**< libcamera (its camera manager or pipeline). */
    KEEL_CAMERA_RESOURCE_IPA = 3,       /**< libcamera's Raspberry Pi IPA module. */
    /** A device node: /dev/media*, /dev/video* or /dev/dma_heap/\*. */
    KEEL_CAMERA_RESOURCE_DEVICE_NODE = 4,
    KEEL_CAMERA_RESOURCE_ENCODER = 5,   /**< The hardware H.264 or JPEG encoder. */
    KEEL_CAMERA_RESOURCE_OPTIONS = 6,   /**< An options struct or argument. */
    KEEL_CAMERA_RESOURCE_MEMORY = 7,    /**< Allocation. */
    KEEL_CAMERA_RESOURCE_THREAD = 8,    /**< A library thread. */
    KEEL_CAMERA_RESOURCE_FILE = 9,      /**< A file being written. */
    KEEL_CAMERA_RESOURCE_NETWORK = 10,  /**< A socket, address or peer. */
    KEEL_CAMERA_RESOURCE_LIBAV = 11,    /**< FFmpeg's libavformat. */
    KEEL_CAMERA_RESOURCE_JPEG = 12      /**< libjpeg. */
};

/** @brief Fixed limits and defaults, as enumerators so that bindings can read them. */
enum keel_camera_limit {
    KEEL_CAMERA_ID_MAX = 128,              /**< Size of keel_camera_info::id, including NUL. */
    KEEL_CAMERA_MODEL_MAX = 64,            /**< Size of keel_camera_info::model, including NUL. */
    KEEL_CAMERA_SENSOR_MAX = 32,           /**< Size of keel_camera_info::sensor, including NUL. */
    KEEL_CAMERA_PATH_MAX = 256,            /**< Size of path fields and the longest URL, including NUL. */
    KEEL_CAMERA_MESSAGE_MAX = 256,         /**< Size of message fields, including NUL. */
    KEEL_CAMERA_WIDTH_DEFAULT = 640,       /**< Width when keel_camera_options::width is 0. */
    KEEL_CAMERA_HEIGHT_DEFAULT = 480,      /**< Height when keel_camera_options::height is 0. */
    KEEL_CAMERA_SIZE_MIN = 64,             /**< Smallest width and height of a stream. */
    KEEL_CAMERA_SIZE_MAX = 16384,          /**< Largest width and height accepted in options. */
    KEEL_CAMERA_FRAMERATE_DEFAULT_MILLI = 30000, /**< 30 fps, when framerate_milli is 0. */
    KEEL_CAMERA_FRAMERATE_MIN_MILLI = 1000,      /**< 1 fps. */
    KEEL_CAMERA_FRAMERATE_MAX_MILLI = 240000,    /**< 240 fps. */
    KEEL_CAMERA_FRAME_QUEUE_DEFAULT = 1,   /**< Raw frames queued when frame_queue is 0. */
    KEEL_CAMERA_FRAME_QUEUE_MAX = 8,       /**< Largest raw frame queue. */
    KEEL_CAMERA_ENCODED_QUEUE_DEFAULT = 16, /**< Chunks queued for the program when encoded_queue is 0. */
    KEEL_CAMERA_ENCODED_QUEUE_MAX = 256,   /**< Largest encoded queue (program or output). */
    KEEL_CAMERA_OUTPUT_QUEUE_DEFAULT = 64, /**< Chunks queued per output (and per client) when queue is 0. */
    KEEL_CAMERA_OUTPUTS_MAX = 8,           /**< Outputs per handle. */
    KEEL_CAMERA_CLIENTS_DEFAULT = 4,       /**< Clients of a listening TCP output when max_clients is 0. */
    KEEL_CAMERA_CLIENTS_MAX = 16,          /**< Largest max_clients. */
    KEEL_CAMERA_DATAGRAM_DEFAULT = 1472,   /**< UDP payload bytes per datagram when datagram_bytes is 0. */
    KEEL_CAMERA_DATAGRAM_MIN = 512,        /**< Smallest datagram_bytes. */
    KEEL_CAMERA_DATAGRAM_MAX = 65507,      /**< Largest datagram_bytes (the IPv4 UDP limit). */
    KEEL_CAMERA_QUALITY_DEFAULT = 85,      /**< JPEG quality when quality is 0 (1..100). */
    KEEL_CAMERA_BITRATE_MIN = 25000,       /**< Smallest H.264 bit rate, bits per second. */
    KEEL_CAMERA_BITRATE_MAX = 25000000,    /**< Largest H.264 bit rate, bits per second. */
    KEEL_CAMERA_ENCODER_SIZE_MAX = 1920,   /**< Largest width and height the Pi 4 hardware encoders take. */
    KEEL_CAMERA_TIMEOUT_MAX_MS = 60000,    /**< Largest timeout of any call. */
    KEEL_CAMERA_STILL_TIMEOUT_DEFAULT_MS = 5000 /**< Still capture limit when timeout_ms is 0. */
};

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_camera_types Types
 * @brief The opaque handles and the size-versioned structs.
 * @{
 */

/**
 * @brief Opaque camera handle, a generation-checked token issued by
 *        keel_camera_open(). The library never dereferences it: a closed,
 *        stale or forged handle yields ::KEEL_E_INVALID_HANDLE.
 */
typedef struct keel_camera keel_camera;

/**
 * @brief Opaque snapshot of the cameras present, issued by
 *        keel_camera_list_open(). Same token rules as ::keel_camera.
 */
typedef struct keel_camera_list keel_camera_list;

/**
 * @brief Detailed outcome of a failed call: written by keel_camera_open() and
 *        keel_camera_list_open() for every status once its @c struct_size is
 *        valid, and returned by keel_camera_last_error().
 */
typedef struct keel_camera_error {
    /** Caller sets sizeof(keel_camera_error). */
    uint32_t struct_size;
    /** The failing status; ::KEEL_OK when nothing failed. */
    int32_t status;
    /** ::keel_camera_resource involved; ::KEEL_CAMERA_RESOURCE_NONE if none. */
    uint32_t resource;
    /** errno (Linux) of the failing operation; 0 if none. */
    int32_t os_error;
    /** Device node, file path or URL involved, or empty
     *  (::KEEL_CAMERA_PATH_MAX bytes; a longer URL is cut). */
    char path[256];
    /** One-line description that names the remedy, or empty
     *  (::KEEL_CAMERA_MESSAGE_MAX bytes). */
    char message[256];
} keel_camera_error;

/** @brief One camera of a ::keel_camera_list. */
typedef struct keel_camera_info {
    /** Caller sets sizeof(keel_camera_info). */
    uint32_t struct_size;
    /** Index to pass in keel_camera_options::index (libcamera's order). */
    uint32_t index;
    /** Bitwise OR of ::keel_camera_info_flag values. */
    uint32_t flags;
    /** Number of sensor modes (keel_camera_list_get_mode()). */
    uint32_t mode_count;
    /** Full sensor resolution, pixels (the largest still). */
    uint32_t pixel_array_width;
    /** Full sensor resolution, lines. */
    uint32_t pixel_array_height;
    /** libcamera's camera id, e.g. "/base/soc/i2c0mux/i2c@1/imx219@10"
     *  (::KEEL_CAMERA_ID_MAX bytes). */
    char id[128];
    /** Product name: "Camera Module 2", "Camera Module 3",
     *  "Camera Module 3 Wide", "Camera Module 3 NoIR", "Camera Module 3 Wide
     *  NoIR", "Camera Module 1", "HQ Camera", "Global Shutter Camera", or the
     *  sensor name for other cameras (::KEEL_CAMERA_MODEL_MAX bytes). */
    char model[64];
    /** Sensor name as libcamera reports it, e.g. "imx219" or
     *  "imx708_wide_noir" (::KEEL_CAMERA_SENSOR_MAX bytes). */
    char sensor[32];
} keel_camera_info;

/** @brief One sensor mode of a listed camera. */
typedef struct keel_camera_mode {
    /** Caller sets sizeof(keel_camera_mode). */
    uint32_t struct_size;
    /** Output width of the sensor in this mode, pixels. */
    uint32_t width;
    /** Output height of the sensor in this mode, lines. */
    uint32_t height;
    /** Bits per pixel of the raw sensor data (8, 10 or 12). */
    uint32_t bit_depth;
    /** Highest frame rate, frames per second x 1000; 0 when unknown
     *  (::KEEL_CAMERA_INFO_IN_USE). */
    uint32_t max_framerate_milli;
    /** Left edge of the sensor area the mode reads, in full-resolution pixels. */
    uint32_t crop_x;
    /** Top edge of that area. */
    uint32_t crop_y;
    /** Width of that area; 0 when unknown. */
    uint32_t crop_width;
    /** Height of that area; 0 when unknown. */
    uint32_t crop_height;
    /** Reserved; written as zero. */
    uint32_t reserved0;
} keel_camera_mode;

/**
 * @brief Options for keel_camera_open(). Zero-initialise, set
 *        @c struct_size, then change only what you need; every zero field
 *        means "default".
 */
typedef struct keel_camera_options {
    /** Caller sets sizeof(keel_camera_options). */
    uint32_t struct_size;
    /** Camera index from keel_camera_list_get(); 0 = the first camera. */
    uint32_t index;
    /** Frame width, even, ::KEEL_CAMERA_SIZE_MIN..::KEEL_CAMERA_SIZE_MAX;
     *  0 = ::KEEL_CAMERA_WIDTH_DEFAULT. The camera may adjust it
     *  (keel_camera_get_stream() gives the result). */
    uint32_t width;
    /** Frame height, even; 0 = ::KEEL_CAMERA_HEIGHT_DEFAULT. */
    uint32_t height;
    /** Frames per second x 1000,
     *  ::KEEL_CAMERA_FRAMERATE_MIN_MILLI..::KEEL_CAMERA_FRAMERATE_MAX_MILLI;
     *  0 = ::KEEL_CAMERA_FRAMERATE_DEFAULT_MILLI. The sensor mode caps it. */
    uint32_t framerate_milli;
    /** ::keel_camera_format of raw frames; 0 = ::KEEL_CAMERA_FORMAT_I420. */
    uint32_t format;
    /** Bitwise OR of ::keel_camera_flag values. */
    uint32_t flags;
    /** Rotation in degrees: 0 or 180 (180 equals both flips). */
    uint32_t rotation;
    /** ::keel_camera_af_mode; 0 = ::KEEL_CAMERA_AF_DEFAULT. */
    uint32_t af_mode;
    /** ::keel_camera_af_range; 0 = ::KEEL_CAMERA_AF_RANGE_DEFAULT. Needs an
     *  autofocus camera unless 0. */
    uint32_t af_range;
    /** With ::KEEL_CAMERA_AF_MANUAL: lens position in dioptres x 1000
     *  (0 = infinity, 1000 = 1 m, 10000 = 10 cm), 0..32000; must be 0
     *  otherwise. */
    uint32_t lens_position_milli;
    /** Exposure time in microseconds, 1..1000000; 0 = automatic exposure. */
    uint32_t exposure_us;
    /** Analogue gain x 1000, 1000..64000; 0 = automatic gain. */
    uint32_t gain_milli;
    /** Exposure compensation of automatic exposure in EV x 1000,
     *  -8000..8000; 0 = none. */
    int32_t ev_milli;
    /** ::keel_camera_awb; 0 = ::KEEL_CAMERA_AWB_AUTO. */
    uint32_t awb_mode;
    /** With ::KEEL_CAMERA_AWB_MANUAL: red colour gain x 1000, 1..32000;
     *  must be 0 otherwise. */
    uint32_t red_gain_milli;
    /** With ::KEEL_CAMERA_AWB_MANUAL: blue colour gain x 1000, 1..32000;
     *  must be 0 otherwise. */
    uint32_t blue_gain_milli;
    /** Raw frames kept for keel_camera_read_frame(),
     *  1..::KEEL_CAMERA_FRAME_QUEUE_MAX; 0 = ::KEEL_CAMERA_FRAME_QUEUE_DEFAULT
     *  (always the newest frame). */
    uint32_t frame_queue;
} keel_camera_options;

/** @brief Layout of the configured stream, from keel_camera_get_stream(). */
typedef struct keel_camera_stream_info {
    /** Caller sets sizeof(keel_camera_stream_info). */
    uint32_t struct_size;
    /** ::keel_camera_format of raw frames. */
    uint32_t format;
    /** Frame width, pixels. */
    uint32_t width;
    /** Frame height, lines. */
    uint32_t height;
    /** Requested frames per second x 1000. */
    uint32_t framerate_milli;
    /** Planes per frame: 3 for I420, 1 for RGB888 and BGR888. */
    uint32_t plane_count;
    /** Bytes per row of plane 0 (Y, or the packed pixels). */
    uint32_t stride0;
    /** Bytes per row of plane 1 (U); 0 if absent. */
    uint32_t stride1;
    /** Bytes per row of plane 2 (V); 0 if absent. */
    uint32_t stride2;
    /** Byte offset of plane 0 in a frame (0). */
    uint32_t offset0;
    /** Byte offset of plane 1; 0 if absent. */
    uint32_t offset1;
    /** Byte offset of plane 2; 0 if absent. */
    uint32_t offset2;
    /** Bytes of one raw frame: the buffer size keel_camera_read_frame() needs. */
    uint32_t frame_bytes;
    /** ::keel_camera_codec set with keel_camera_set_encoder(). */
    uint32_t codec;
    /** Largest chunk keel_camera_read_encoded() can return: the buffer size
     *  it needs; 0 without an encoder. */
    uint32_t encoded_bytes_max;
    /** Reserved; written as zero. */
    uint32_t reserved0;
} keel_camera_stream_info;

/** @brief Description of one raw frame, written by keel_camera_read_frame(). */
typedef struct keel_camera_frame_info {
    /** Caller sets sizeof(keel_camera_frame_info). */
    uint32_t struct_size;
    /** ::keel_camera_format. */
    uint32_t format;
    /** Width, pixels. */
    uint32_t width;
    /** Height, lines. */
    uint32_t height;
    /** Planes: 3 for I420, 1 for RGB888 and BGR888. */
    uint32_t plane_count;
    /** Bytes per row of plane 0. */
    uint32_t stride0;
    /** Bytes per row of plane 1; 0 if absent. */
    uint32_t stride1;
    /** Bytes per row of plane 2; 0 if absent. */
    uint32_t stride2;
    /** Byte offset of plane 0 in the buffer. */
    uint32_t offset0;
    /** Byte offset of plane 1; 0 if absent. */
    uint32_t offset1;
    /** Byte offset of plane 2; 0 if absent. */
    uint32_t offset2;
    /** Bytes written to the buffer. */
    uint32_t bytes;
    /** Sensor frame sequence number; a gap is a frame the camera dropped. */
    uint64_t sequence;
    /** Start of exposure, CLOCK_MONOTONIC nanoseconds. */
    uint64_t timestamp_ns;
} keel_camera_frame_info;

/**
 * @brief Options for keel_camera_set_encoder(). Zero-initialise, set
 *        @c struct_size and @c codec; every other zero field means "default".
 */
typedef struct keel_camera_encoder_options {
    /** Caller sets sizeof(keel_camera_encoder_options). */
    uint32_t struct_size;
    /** ::keel_camera_codec; ::KEEL_CAMERA_CODEC_NONE removes the encoder. */
    uint32_t codec;
    /** H.264 bit rate, bits per second,
     *  ::KEEL_CAMERA_BITRATE_MIN..::KEEL_CAMERA_BITRATE_MAX; 0 = 0.15 bit
     *  per pixel and frame (1.4 Mbit/s at 640x480 and 9.3 Mbit/s at
     *  1920x1080, 30 fps). Must be 0 for MJPEG. */
    uint32_t bitrate;
    /** H.264 frames from one keyframe to the next, 1..1000; 0 = one second
     *  of frames. Must be 0 for MJPEG. */
    uint32_t intra_period;
    /** ::keel_camera_h264_profile; must be 0 for MJPEG. */
    uint32_t profile;
    /** Bitwise OR of ::keel_camera_encoder_flag values. */
    uint32_t flags;
    /** MJPEG quality 1..100; 0 = ::KEEL_CAMERA_QUALITY_DEFAULT. Must be 0
     *  for H.264. */
    uint32_t quality;
    /** Chunks kept for keel_camera_read_encoded(),
     *  1..::KEEL_CAMERA_ENCODED_QUEUE_MAX; 0 =
     *  ::KEEL_CAMERA_ENCODED_QUEUE_DEFAULT. */
    uint32_t encoded_queue;
} keel_camera_encoder_options;

/** @brief Description of one encoded chunk, written by keel_camera_read_encoded().
 *  A chunk is one encoded frame: an H.264 access unit or one JPEG image. */
typedef struct keel_camera_encoded_info {
    /** Caller sets sizeof(keel_camera_encoded_info). */
    uint32_t struct_size;
    /** ::keel_camera_codec. */
    uint32_t codec;
    /** Bitwise OR of ::keel_camera_encoded_flag values. */
    uint32_t flags;
    /** Bytes written to the buffer. */
    uint32_t bytes;
    /** Sequence number of the frame it encodes. */
    uint64_t sequence;
    /** Start of exposure of that frame, CLOCK_MONOTONIC nanoseconds. */
    uint64_t timestamp_ns;
} keel_camera_encoded_info;

/** @brief Options for keel_camera_output_start(); NULL means all defaults. */
typedef struct keel_camera_output_options {
    /** Caller sets sizeof(keel_camera_output_options). */
    uint32_t struct_size;
    /** Reserved for flags; must be 0. */
    uint32_t flags;
    /** Chunks queued for this output (for each client of a listening
     *  output), 1..::KEEL_CAMERA_ENCODED_QUEUE_MAX; 0 =
     *  ::KEEL_CAMERA_OUTPUT_QUEUE_DEFAULT. */
    uint32_t queue;
    /** Listening TCP: clients served at once, 1..::KEEL_CAMERA_CLIENTS_MAX;
     *  0 = ::KEEL_CAMERA_CLIENTS_DEFAULT. Must be 0 for other outputs. */
    uint32_t max_clients;
    /** UDP: payload bytes per datagram,
     *  ::KEEL_CAMERA_DATAGRAM_MIN..::KEEL_CAMERA_DATAGRAM_MAX; 0 =
     *  ::KEEL_CAMERA_DATAGRAM_DEFAULT. Must be 0 for other outputs. */
    uint32_t datagram_bytes;
    /** Reserved; must be 0. */
    uint32_t reserved0;
} keel_camera_output_options;

/** @brief Counters of one output, from keel_camera_get_output_stats(). */
typedef struct keel_camera_output_stats {
    /** Caller sets sizeof(keel_camera_output_stats). */
    uint32_t struct_size;
    /** ::keel_camera_output_state. */
    uint32_t state;
    /** ::KEEL_OK, or the status that failed the output. */
    int32_t status;
    /** errno of the failure; 0 if none. */
    int32_t os_error;
    /** Clients connected now (listening TCP); 1 or 0 for the others. */
    uint32_t clients;
    /** Reserved; written as zero. */
    uint32_t reserved0;
    /** Bytes written or sent. */
    uint64_t bytes;
    /** Chunks written or sent (to each client, summed). */
    uint64_t chunks;
    /** Chunks dropped because the destination was slower than the camera. */
    uint64_t chunks_dropped;
    /** Description of the failure, or empty (::KEEL_CAMERA_MESSAGE_MAX bytes). */
    char message[256];
} keel_camera_output_stats;

/** @brief Counters of a handle since keel_camera_open(), from keel_camera_get_stats(). */
typedef struct keel_camera_stats {
    /** Caller sets sizeof(keel_camera_stats). */
    uint32_t struct_size;
    /** ::keel_camera_state. */
    uint32_t state;
    /** Frames the camera delivered. */
    uint64_t frames_captured;
    /** Frames returned by keel_camera_read_frame(). */
    uint64_t frames_read;
    /** Frames dropped from the full raw frame queue without being read. */
    uint64_t frames_dropped;
    /** Frames the encoder skipped because it was still busy. */
    uint64_t encoder_frames_dropped;
    /** Chunks the encoder produced. */
    uint64_t chunks_encoded;
    /** Chunks returned by keel_camera_read_encoded(). */
    uint64_t chunks_read;
    /** Chunks dropped from the program's full encoded queue. */
    uint64_t chunks_dropped;
    /** Outputs started and not yet stopped. */
    uint32_t outputs;
    /** Reserved; written as zero. */
    uint32_t reserved0;
} keel_camera_stats;

/** @brief Options for keel_camera_capture_still() and
 *  keel_camera_capture_still_file(); NULL means all defaults. */
typedef struct keel_camera_still_options {
    /** Caller sets sizeof(keel_camera_still_options). */
    uint32_t struct_size;
    /** Width, even, ::KEEL_CAMERA_SIZE_MIN..::KEEL_CAMERA_SIZE_MAX; 0 = the
     *  full sensor resolution (keel_camera_info::pixel_array_width). Width
     *  and height are both 0 or both set. */
    uint32_t width;
    /** Height, even; 0 = the full sensor resolution. */
    uint32_t height;
    /** JPEG quality 1..100; 0 = ::KEEL_CAMERA_QUALITY_DEFAULT. */
    uint32_t quality;
    /** Longest time the capture may take, ms, up to
     *  ::KEEL_CAMERA_TIMEOUT_MAX_MS; 0 =
     *  ::KEEL_CAMERA_STILL_TIMEOUT_DEFAULT_MS. */
    uint32_t timeout_ms;
    /** Reserved; must be 0. */
    uint32_t reserved0;
} keel_camera_still_options;

/** @brief Description of a still, written by the still captures. */
typedef struct keel_camera_still_info {
    /** Caller sets sizeof(keel_camera_still_info). */
    uint32_t struct_size;
    /** Width, pixels. */
    uint32_t width;
    /** Height, lines. */
    uint32_t height;
    /** Bytes of the JPEG image. */
    uint32_t bytes;
    /** Bitwise OR of ::keel_camera_still_flag values. */
    uint32_t flags;
    /** Reserved; written as zero. */
    uint32_t reserved0;
    /** Start of exposure, CLOCK_MONOTONIC nanoseconds. */
    uint64_t timestamp_ns;
} keel_camera_still_info;

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_camera_meta Library metadata
 * @brief Calls that need no handle. All are thread-safe and never block.
 * @{
 */

/**
 * @brief ABI version of the loaded camera library.
 * @return (major << 16) | minor, i.e. ::KEEL_CAMERA_ABI_VERSION of the
 *         library's own header. A binding requires an equal major and a minor
 *         at least as high as the newest function it calls.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API uint32_t keel_camera_abi_version(void);

/**
 * @brief Human-readable build identification.
 * @return Static string such as "1.0.0 (aarch64-linux-gnu; libcamera 0.7.0;
 *         jpeg)", naming the optional features built in ("jpeg", "libav",
 *         "test-hooks"). Never NULL; do not free; at most ::KEEL_STRING_MAX
 *         bytes including the NUL.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API const char *keel_camera_version_string(void);

/**
 * @brief Convert an I420 frame to packed RGB888 or BGR888 on the CPU.
 *
 * For frames already read as I420. Opening the camera with
 * ::KEEL_CAMERA_FORMAT_RGB888 or ::KEEL_CAMERA_FORMAT_BGR888 instead lets the
 * image signal processor convert at no CPU cost. Full-range BT.601 (sYCC) in
 * 16-bit fixed point, each chroma sample covering 2x2 pixels:
 * <tt>R = Y + ((91881 * (V - 128) + 32768) >> 16)</tt>,
 * <tt>G = Y + ((-22554 * (U - 128) - 46802 * (V - 128) + 32768) >> 16)</tt>,
 * <tt>B = Y + ((116130 * (U - 128) + 32768) >> 16)</tt>, each clamped to
 * 0..255, with arithmetic shifts.
 *
 * @param[in]  frame      The I420 frame as keel_camera_read_frame() wrote it.
 * @param[in]  layout     Its frame info (format, size, strides, offsets, bytes).
 * @param[out] out        Destination buffer.
 * @param[in]  out_size   Size of @p out; at least @p out_stride x height.
 * @param[in]  out_format ::KEEL_CAMERA_FORMAT_RGB888 or ::KEEL_CAMERA_FORMAT_BGR888.
 * @param[in]  out_stride Bytes per destination row; 0 = width x 3.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL pointer, bad
 *         @c struct_size, a layout that is not I420 or does not fit in
 *         @c layout->bytes, an unknown output format, a stride below
 *         width x 3, or @p out_size too small. Nothing is written.
 * @par Thread safety
 * Any thread; pure computation.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_convert_i420(const uint8_t *frame, const keel_camera_frame_info *layout,
                                          uint8_t *out, uint32_t out_size, uint32_t out_format,
                                          uint32_t out_stride);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_camera_listing Listing
 * @brief A snapshot of the cameras present, their sensors, modes and
 *        capabilities.
 * @{
 */

/**
 * @brief Take a snapshot of the cameras present.
 *
 * Asks libcamera for its cameras. For each camera that no process holds, the
 * camera is briefly acquired and every sensor mode configured, to read its
 * frame-rate limit and crop; a held camera is listed with
 * ::KEEL_CAMERA_INFO_IN_USE instead. No camera at all is a valid snapshot with
 * a count of 0; when a camera is connected but cannot be used, the call fails
 * and says why.
 *
 * @param[out] out_list Receives the snapshot; set to NULL on failure. Must not
 *                      be NULL.
 * @param[out] error    Optional (may be NULL): written for every status once
 *                      its @c struct_size is valid.
 * @retval KEEL_OK                 Snapshot taken (possibly of no camera).
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL @p out_list or bad
 *         @c struct_size of @p error.
 * @retval KEEL_E_PERMISSION       [SETUP] A camera device node is not
 *         accessible; the message names it and the video group.
 * @retval KEEL_E_NOT_FOUND        [SETUP] A camera is connected but libcamera
 *         cannot drive it (its IPA module is missing).
 * @retval KEEL_E_SETUP_IO         [SETUP] libcamera's camera manager failed to start.
 * @retval KEEL_E_NO_MEMORY        [SETUP] Allocation failed.
 * @par Blocking
 * Up to about 2 s with a camera connected (mode probing).
 * @par Thread safety
 * Any thread; the calling thread owns the snapshot.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_list_open(keel_camera_list **out_list, keel_camera_error *error);

/**
 * @brief Number of cameras in a snapshot.
 * @param[in]  list      Snapshot handle.
 * @param[out] out_count Receives the count.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL @p out_count.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_list_count(const keel_camera_list *list, uint32_t *out_count);

/**
 * @brief Describe one camera of a snapshot.
 * @param[in]  list  Snapshot handle.
 * @param[in]  index Camera index, below the count.
 * @param[out] out   Description; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No camera with this index.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_list_get(const keel_camera_list *list, uint32_t index, keel_camera_info *out);

/**
 * @brief Describe one sensor mode of a listed camera.
 * @param[in]  list  Snapshot handle.
 * @param[in]  index Camera index, below the count.
 * @param[in]  mode  Mode index, below keel_camera_info::mode_count. Modes are
 *                   ordered by bit depth, then by size.
 * @param[out] out   Description; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No camera or mode with this index.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_list_get_mode(const keel_camera_list *list, uint32_t index, uint32_t mode,
                                           keel_camera_mode *out);

/**
 * @brief Free a snapshot.
 * @param[in] list Snapshot handle, or NULL (returns ::KEEL_OK).
 * @retval KEEL_OK                 Freed (also for NULL).
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE] Already closed, stale or forged.
 * @retval KEEL_E_NOT_OWNER        [HANDLE] Called in a forked child.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_list_close(keel_camera_list *list);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_camera_lifetime Lifetime
 * @brief Open, configure and close a camera.
 * @{
 */

/**
 * @brief Open a camera.
 *
 * Acquires the camera for this process, checks every option against it and
 * prepares the stream, so keel_camera_get_stream() can give the frame layout
 * before capture starts. Nothing is captured until keel_camera_start(). On
 * a camera without autofocus, an af_mode other than ::KEEL_CAMERA_AF_DEFAULT,
 * an af_range or a lens position is ::KEEL_E_UNSUPPORTED.
 *
 * Every non-OK status means no handle was created: @p *out_camera is NULL
 * and @p error describes why. An options struct larger than this library
 * knows with a non-zero tail is ::KEEL_E_INVALID_ARGUMENT (resource OPTIONS).
 *
 * @param[in]  options    Options, or NULL for all defaults.
 * @param[out] out_camera Receives the handle; set to NULL on failure. Must not
 *                        be NULL.
 * @param[out] error      Optional (may be NULL): written for every status once
 *                        its @c struct_size is valid.
 * @retval KEEL_OK                 Camera open.
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL @p out_camera, bad
 *         @c struct_size, or an option out of range; @c error->message names it.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] An autofocus option on a camera
 *         without autofocus.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No camera with this index, or the
 *         IPA module is missing.
 * @retval KEEL_E_IN_USE           [SETUP] Another process or handle has the camera.
 * @retval KEEL_E_PERMISSION       [SETUP] A camera device node is not accessible.
 * @retval KEEL_E_SETUP_IO         [SETUP] libcamera refused the configuration
 *         or failed to start.
 * @retval KEEL_E_NO_MEMORY        [SETUP] Allocation failed.
 * @par Blocking
 * Up to about 1 s (libcamera start-up).
 * @par Thread safety
 * Any thread; the calling thread owns the new handle.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_open(const keel_camera_options *options, keel_camera **out_camera,
                                  keel_camera_error *error);

/**
 * @brief Close a camera: stop capture and every output, finish their files,
 *        close their sockets, end every library thread and release the camera.
 *
 * Calls blocked in another thread return ::KEEL_E_INVALID_HANDLE. The handle
 * is invalid after return whatever the status, which is diagnostic only:
 * bindings log it and never throw from close, a cleaner or a finalizer.
 *
 * @param[in] camera Handle, or NULL (returns ::KEEL_OK).
 * @retval KEEL_OK                 Closed (also for NULL).
 * @retval KEEL_E_PERIPHERAL       [PERIPHERAL] An output could not finish its
 *         file or stream cleanly; everything was still released.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE] Already closed, stale or forged.
 * @retval KEEL_E_NOT_OWNER        [HANDLE] Called in a forked child; nothing
 *         was touched.
 * @par Blocking
 * Up to about 3 s (an output finishing its file or a network peer).
 * @par Thread safety
 * Any thread, including concurrently with the owner's calls.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_close(keel_camera *camera);

/**
 * @brief Describe the camera a handle opened.
 * @param[in]  camera Handle.
 * @param[out] out    Description; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_get_info(const keel_camera *camera, keel_camera_info *out);

/**
 * @brief Layout of the stream the handle captures, and the buffer sizes its
 *        reads need.
 * @param[in]  camera Handle.
 * @param[out] out    Layout; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_get_stream(const keel_camera *camera, keel_camera_stream_info *out);

/**
 * @brief Choose the encoder for keel_camera_read_encoded() and the outputs.
 *
 * Finds the hardware encoder by its driver and card name (bcm2835-codec,
 * "bcm2835-codec-encode" for H.264, "bcm2835-codec-encode_image" for MJPEG)
 * and checks the options; the encoder runs from keel_camera_start() on.
 * Allowed only while stopped and without outputs.
 *
 * @param[in] camera  Handle.
 * @param[in] options Encoder options; ::KEEL_CAMERA_CODEC_NONE removes the encoder.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL, bad @c struct_size, an
 *         unknown codec, profile or flag, or a value out of range or meant
 *         for the other codec.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] The stream is larger than
 *         ::KEEL_CAMERA_ENCODER_SIZE_MAX, or the options struct has an
 *         unknown non-zero tail.
 * @retval KEEL_E_STATE            [REJECTED] Capture is running or outputs exist.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No hardware encoder node.
 * @retval KEEL_E_PERMISSION       [SETUP] The encoder node is not accessible.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_set_encoder(keel_camera *camera, const keel_camera_encoder_options *options);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_camera_capture Capture
 * @brief Start and stop capture, and read raw frames and encoded chunks.
 * @{
 */

/**
 * @brief Start capture, the encoder and every output's data flow.
 * @param[in] camera Handle.
 * @retval KEEL_OK
 * @retval KEEL_E_STATE            [REJECTED] Already running (or FAILED: stop first).
 * @retval KEEL_E_SETUP_IO         [SETUP] The camera or the encoder failed to
 *         start; nothing runs.
 * @retval KEEL_E_NO_MEMORY        [SETUP] Buffer allocation failed.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to about 500 ms.
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_start(keel_camera *camera);

/**
 * @brief Stop capture and the encoder. Queued frames and chunks are
 *        discarded; outputs stay open, send what they have queued and resume
 *        at the next keyframe after keel_camera_start().
 * @param[in] camera Handle.
 * @retval KEEL_OK                 Stopped (also when already stopped).
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to about 300 ms.
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_stop(keel_camera *camera);

/**
 * @brief Take the oldest queued raw frame into a caller buffer.
 * @param[in]  camera     Handle.
 * @param[out] buffer     Destination, at least keel_camera_stream_info::frame_bytes.
 * @param[in]  size       Size of @p buffer.
 * @param[in]  timeout_ms Longest wait for a frame, up to
 *                        ::KEEL_CAMERA_TIMEOUT_MAX_MS; 0 = do not wait.
 * @param[out] out_info   Layout, sequence and timestamp of the frame;
 *                        @c out_info->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL, bad @c struct_size, @p size
 *         below the frame size (the frame stays queued), or a timeout too long.
 * @retval KEEL_E_AGAIN            [REJECTED] No frame arrived within the timeout.
 * @retval KEEL_E_STATE            [REJECTED] Capture is not running.
 * @retval KEEL_E_PERIPHERAL       [PERIPHERAL] The camera failed while
 *         capturing (state FAILED); see keel_camera_last_error().
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE] Also when another thread closed
 *         the handle during the wait.
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to @p timeout_ms.
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_read_frame(keel_camera *camera, uint8_t *buffer, uint32_t size,
                                        uint32_t timeout_ms, keel_camera_frame_info *out_info);

/**
 * @brief Take the oldest queued encoded chunk into a caller buffer.
 *
 * The first chunk after keel_camera_start() is a keyframe with the H.264
 * parameter sets. After the queue overflowed, H.264 resumes at a keyframe.
 *
 * @param[in]  camera     Handle.
 * @param[out] buffer     Destination, at least keel_camera_stream_info::encoded_bytes_max.
 * @param[in]  size       Size of @p buffer.
 * @param[in]  timeout_ms Longest wait for a chunk, up to
 *                        ::KEEL_CAMERA_TIMEOUT_MAX_MS; 0 = do not wait.
 * @param[out] out_info   Codec, flags, size, sequence and timestamp;
 *                        @c out_info->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL, bad @c struct_size, @p size
 *         below the chunk (it stays queued), or a timeout too long.
 * @retval KEEL_E_AGAIN            [REJECTED] No chunk arrived within the timeout.
 * @retval KEEL_E_STATE            [REJECTED] Capture is not running, or no encoder is set.
 * @retval KEEL_E_PERIPHERAL       [PERIPHERAL] The camera or the encoder
 *         failed while capturing; see keel_camera_last_error().
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE] Also when another thread closed
 *         the handle during the wait.
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to @p timeout_ms.
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_read_encoded(keel_camera *camera, uint8_t *buffer, uint32_t size,
                                          uint32_t timeout_ms, keel_camera_encoded_info *out_info);

/**
 * @brief Run one autofocus scan and wait for its result.
 *
 * In ::KEEL_CAMERA_AF_AUTO mode it starts one scan; in
 * ::KEEL_CAMERA_AF_DEFAULT or ::KEEL_CAMERA_AF_CONTINUOUS mode it waits until
 * the continuous search reports focus.
 *
 * @param[in]  camera       Handle.
 * @param[in]  timeout_ms   Longest wait, 1..::KEEL_CAMERA_TIMEOUT_MAX_MS.
 * @param[out] out_af_state Receives the ::keel_camera_af_state.
 * @retval KEEL_OK                 The scan ended or the timeout passed (see
 *         @p out_af_state).
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL @p out_af_state or a bad timeout.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] The camera has no autofocus, or
 *         is in ::KEEL_CAMERA_AF_MANUAL mode.
 * @retval KEEL_E_STATE            [REJECTED] Capture is not running.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to @p timeout_ms.
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_trigger_autofocus(keel_camera *camera, uint32_t timeout_ms,
                                               uint32_t *out_af_state);

/**
 * @brief Counters of the handle.
 * @param[in]  camera Handle.
 * @param[out] out    Counters; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_get_stats(const keel_camera *camera, keel_camera_stats *out);

/**
 * @brief The latest failure on the handle, with its message.
 * @param[in]  camera Handle.
 * @param[out] out    Error record (status ::KEEL_OK if no call failed);
 *                    @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_last_error(const keel_camera *camera, keel_camera_error *out);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_camera_outputs Outputs
 * @brief Files and network streams the library writes itself, from the
 *        handle's encoder, alongside the program's own reads.
 *
 * Destinations:
 * - @c tcp://HOST:PORT connects to a receiver (for example
 *   <tt>ffplay -f h264 tcp://0.0.0.0:5000?listen</tt>);
 * - @c tcp://:PORT?listen (or @c tcp://ADDRESS:PORT?listen) waits for
 *   receivers such as VLC or OpenCV and serves up to @c max_clients at once,
 *   each from its next keyframe;
 * - @c udp://HOST:PORT sends datagrams of at most @c datagram_bytes;
 * - a file path ending in @c .h264 or @c .264 (H.264 Annex B) or in
 *   @c .mjpeg or @c .mjpg (concatenated JPEG images);
 * - with libavformat built in: files ending in @c .mp4, @c .mkv, @c .mov or
 *   @c .ts (H.264; MJPEG only in @c .mkv), and @c rtsp://, @c rtmp:// and
 *   @c srt:// URLs (H.264). Without it these are ::KEEL_E_UNSUPPORTED.
 * TCP and UDP carry the bare codec stream (Annex B or JPEG images). Every
 * receiver and file starts at a keyframe preceded by the parameter sets.
 * @{
 */

/**
 * @brief Start writing the encoded stream to a file or network destination.
 *
 * Opens the file, connects (up to 3 s) or binds the listening socket now, so
 * those errors return here; a failure later puts the output in
 * ::KEEL_CAMERA_OUTPUT_FAILED without stopping the capture. Allowed while
 * stopped or running (the output joins at the next keyframe, which is
 * requested at once). Needs an encoder (keel_camera_set_encoder()).
 *
 * @param[in]  camera     Handle.
 * @param[in]  url        Destination, NUL-terminated, shorter than
 *                        ::KEEL_CAMERA_PATH_MAX bytes.
 * @param[in]  options    Output options, or NULL for defaults.
 * @param[out] out_output Receives the output id for the calls below.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL, bad @c struct_size, an
 *         unknown scheme or file extension, a destination that does not
 *         match the codec, or an option out of range or meant for another kind.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] A container or protocol output
 *         without libavformat, or an options tail this library cannot honour.
 * @retval KEEL_E_STATE            [REJECTED] No encoder is set.
 * @retval KEEL_E_AGAIN            [REJECTED] ::KEEL_CAMERA_OUTPUTS_MAX outputs
 *         exist; stop one first.
 * @retval KEEL_E_NOT_FOUND        [SETUP] The host does not resolve, the
 *         receiver refused or did not answer, or the file's directory is missing.
 * @retval KEEL_E_IN_USE           [SETUP] The listening port is taken.
 * @retval KEEL_E_PERMISSION       [SETUP] The file cannot be created, or the
 *         port needs privileges.
 * @retval KEEL_E_SETUP_IO         [SETUP] Another socket, file or libavformat failure.
 * @retval KEEL_E_NO_MEMORY        [SETUP] Allocation failed.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to about 3 s (name resolution and connect).
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_output_start(keel_camera *camera, const char *url,
                                          const keel_camera_output_options *options, uint32_t *out_output);

/**
 * @brief Stop an output: send or write what it has queued, finish the file
 *        (a container's index), close its sockets and end its thread.
 * @param[in] camera Handle.
 * @param[in] output Output id from keel_camera_output_start().
 * @retval KEEL_OK
 * @retval KEEL_E_PERIPHERAL       [PERIPHERAL] The output had failed, or its
 *         file could not be finished; it is stopped anyway.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No output with this id.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to about 3 s.
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_output_stop(keel_camera *camera, uint32_t output);

/**
 * @brief Counters and state of one output.
 * @param[in]  camera Handle.
 * @param[in]  output Output id from keel_camera_output_start().
 * @param[out] out    Counters; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No output with this id.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_get_output_stats(const keel_camera *camera, uint32_t output,
                                              keel_camera_output_stats *out);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_camera_stills Stills
 * @brief One JPEG image at a chosen resolution, up to the full sensor.
 * @{
 */

/**
 * @brief Capture one JPEG image into a caller buffer.
 *
 * Allowed while stopped. Configures a still stream at the requested size,
 * lets exposure, white balance and (on autofocus cameras) focus settle,
 * takes one frame and encodes it: on the hardware image encoder up to
 * ::KEEL_CAMERA_ENCODER_SIZE_MAX in both directions, with libjpeg above
 * that (::KEEL_CAMERA_STILL_SOFTWARE). The handle's video stream is
 * configured again by the next keel_camera_start().
 *
 * @param[in]  camera   Handle.
 * @param[in]  options  Still options, or NULL for a full-resolution still.
 * @param[out] buffer   Destination for the JPEG image.
 * @param[in]  size     Size of @p buffer. (Width rounded up to 16) x (height
 *                      rounded up to 16) x 3 + 2048 bytes always suffice;
 *                      a typical image needs a tenth of that.
 * @param[out] out_info Size, bytes and timestamp of the still;
 *                      @c out_info->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL, bad @c struct_size, a
 *         size or quality out of range, or an image larger than @p size
 *         (nothing is written; keel_camera_last_error() gives the size needed).
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] Larger than the hardware encoder
 *         takes and libjpeg is not built in, or an options tail this library
 *         cannot honour.
 * @retval KEEL_E_STATE            [REJECTED] Capture is running: stop it first.
 * @retval KEEL_E_AGAIN            [REJECTED] No frame within the timeout.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No hardware image encoder node.
 * @retval KEEL_E_PERMISSION       [SETUP] The encoder node is not accessible.
 * @retval KEEL_E_SETUP_IO         [SETUP] The camera or the encoder failed.
 * @retval KEEL_E_NO_MEMORY        [SETUP] Allocation failed.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to the options' timeout (about 1 s at full resolution).
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_capture_still(keel_camera *camera, const keel_camera_still_options *options,
                                           uint8_t *buffer, uint32_t size, keel_camera_still_info *out_info);

/**
 * @brief Capture one JPEG image into a file (created or replaced).
 *
 * As keel_camera_capture_still(), but the image goes to @p path and needs no
 * buffer of the caller's.
 *
 * @param[in]  camera   Handle.
 * @param[in]  options  Still options, or NULL for a full-resolution still.
 * @param[in]  path     File path, NUL-terminated, shorter than ::KEEL_CAMERA_PATH_MAX bytes.
 * @param[out] out_info Optional (may be NULL): size, bytes and timestamp.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL @p path, bad
 *         @c struct_size, or a size or quality out of range.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] As keel_camera_capture_still().
 * @retval KEEL_E_STATE            [REJECTED] Capture is running: stop it first.
 * @retval KEEL_E_AGAIN            [REJECTED] No frame within the timeout.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No image encoder node, or the
 *         file's directory is missing.
 * @retval KEEL_E_PERMISSION       [SETUP] The encoder node is not accessible,
 *         or the file cannot be created.
 * @retval KEEL_E_SETUP_IO         [SETUP] The camera, the encoder or the file
 *         write failed.
 * @retval KEEL_E_NO_MEMORY        [SETUP] Allocation failed.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the owning thread.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Up to the options' timeout.
 * @par Thread safety
 * Owning thread.
 * @since 1.0
 */
KEEL_API int32_t keel_camera_capture_still_file(keel_camera *camera, const keel_camera_still_options *options,
                                                const char *path, keel_camera_still_info *out_info);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* KEEL_CAMERA_H */
