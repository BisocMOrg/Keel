/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file camera_test.h
 * @brief Helpers of the camera tests: status checks that need no libkeel.so,
 *        option and info initialisers, the synthetic camera's pattern, and
 *        loopback receivers for the output tests. The tests run with
 *        KEEL_CAMERA_TEST_SOURCE=1 (CTest sets it), so the library uses its
 *        synthetic camera and encoder.
 */
#ifndef KEEL_CAMERA_TEST_H
#define KEEL_CAMERA_TEST_H

#include <stdint.h>

#include "check.h"
#include "keel/camera.h"

/** @brief Name of a status code, without libkeel.so. */
const char *camera_status_name(int32_t status);

/** @brief Fails the test unless @p call returns @p expected; prints the
 *         handle's last error when one is given. */
#define CHECK_STATUS(call, expected)                                                          \
    do {                                                                                      \
        int32_t check_rc_ = (call);                                                           \
        int32_t check_expected_ = (expected);                                                 \
        if (check_rc_ != check_expected_) {                                                   \
            fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__, #call, \
                    camera_status_name(check_rc_), camera_status_name(check_expected_));      \
            exit(1);                                                                          \
        }                                                                                     \
    } while (0)

/** @brief Zero-initialised options with struct_size set. */
keel_camera_options camera_options(void);
/** @brief Zero-initialised encoder options for @p codec. */
keel_camera_encoder_options camera_encoder(uint32_t codec);
/** @brief Opens @p options (or defaults) and fails the test on error. */
keel_camera *camera_open(const keel_camera_options *options);
/** @brief The stream layout of an open camera. */
keel_camera_stream_info camera_stream(keel_camera *camera);
/** @brief The counters of an open camera. */
keel_camera_stats camera_stats(keel_camera *camera);
/** @brief The last error of an open camera. */
keel_camera_error camera_last_error(keel_camera *camera);
/** @brief Reads one frame (2 s timeout) into @p buffer; fails the test on error. */
keel_camera_frame_info camera_read_frame(keel_camera *camera, uint8_t *buffer, uint32_t size);
/** @brief Reads one encoded chunk (2 s timeout); fails the test on error. */
keel_camera_encoded_info camera_read_encoded(keel_camera *camera, uint8_t *buffer, uint32_t size);

/** @brief Checks a frame against the synthetic camera's pattern. */
void camera_check_pattern(const uint8_t *frame, const keel_camera_frame_info *info, int hflip, int vflip);

/** @brief Sets a test fault (KEEL_CAMERA_TEST_FAULT) for the next calls; NULL clears it. */
void camera_fault(const char *name);

/** @brief Number of open file descriptors of this process. */
int camera_count_fds(void);
/** @brief Number of threads of this process. */
int camera_count_threads(void);
/** @brief Sleeps @p ms milliseconds. */
void camera_sleep_ms(unsigned ms);

/** @brief A loopback TCP or UDP receiver on its own thread. */
typedef struct camera_receiver camera_receiver;
/**
 * @brief Starts a receiver: a TCP server that accepts one connection
 *        (@p tcp_listen), a TCP client of 127.0.0.1:@p port (@p tcp_connect),
 *        or a UDP socket.
 * @param kind  'L' TCP listener, 'C' TCP client, 'U' UDP socket.
 * @param port  Port to connect to ('C'); 0 = an ephemeral port ('L', 'U').
 */
camera_receiver *camera_receiver_start(char kind, uint16_t port);
/** @brief Port the receiver listens on. */
uint16_t camera_receiver_port(const camera_receiver *receiver);
/** @brief Waits until at least @p bytes arrived or @p timeout_ms passed; returns the count. */
size_t camera_receiver_wait(camera_receiver *receiver, size_t bytes, unsigned timeout_ms);
/** @brief Stops the receiver (closing its socket) and returns what it received. */
uint8_t *camera_receiver_stop(camera_receiver *receiver, size_t *bytes, size_t *datagrams,
                              size_t *largest_datagram);

/** @brief A loopback TCP port that was free a moment ago. */
uint16_t camera_free_port(void);
/** @brief Number of Annex-B NAL units of @p type in @p data. */
unsigned camera_count_nal(const uint8_t *data, size_t bytes, unsigned type);
/** @brief Type of the first NAL unit of @p data, or 0 when it does not start with a start code. */
unsigned camera_first_nal(const uint8_t *data, size_t bytes);

#endif /* KEEL_CAMERA_TEST_H */
