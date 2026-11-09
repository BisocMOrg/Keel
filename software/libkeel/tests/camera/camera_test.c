/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Helpers of the camera tests (camera_test.h).
 */
#define _GNU_SOURCE

#include "camera_test.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

const char *camera_status_name(int32_t status)
{
    static const char *const names[] = {
        "KEEL_OK", "KEEL_E_INVALID_ARGUMENT", "KEEL_E_IO", "KEEL_E_STATE", "KEEL_E_HEALTH", "KEEL_E_DEADLINE",
        "KEEL_E_FEEDBACK", "KEEL_E_AGAIN", "KEEL_E_NO_MEMORY", "KEEL_E_PWM", "KEEL_E_FAULTED", "KEEL_E_IN_USE",
        "KEEL_E_PERMISSION", "KEEL_E_NOT_FOUND", "KEEL_E_UNSUPPORTED", "KEEL_E_CONFLICT", "KEEL_E_PERIPHERAL",
        "KEEL_E_ACK_TIMEOUT", "KEEL_E_INVALID_HANDLE", "KEEL_E_NOT_OWNER", "KEEL_E_INTERNAL", "KEEL_E_SETUP_IO",
    };
    if (status > 0 || -status >= (int32_t)(sizeof(names) / sizeof(names[0]))) {
        return "KEEL_E_UNKNOWN";
    }
    return names[-status];
}

keel_camera_options camera_options(void)
{
    keel_camera_options options;
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    return options;
}

keel_camera_encoder_options camera_encoder(uint32_t codec)
{
    keel_camera_encoder_options options;
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.codec = codec;
    return options;
}

keel_camera *camera_open(const keel_camera_options *options)
{
    keel_camera_error error;
    keel_camera *camera = NULL;
    int32_t rc;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    rc = keel_camera_open(options, &camera, &error);
    if (rc != KEEL_OK) {
        fprintf(stderr, "keel_camera_open: %s: %s\n", camera_status_name(rc), error.message);
        exit(1);
    }
    return camera;
}

keel_camera_stream_info camera_stream(keel_camera *camera)
{
    keel_camera_stream_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_get_stream(camera, &info), KEEL_OK);
    return info;
}

keel_camera_stats camera_stats(keel_camera *camera)
{
    keel_camera_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    CHECK_STATUS(keel_camera_get_stats(camera, &stats), KEEL_OK);
    return stats;
}

keel_camera_error camera_last_error(keel_camera *camera)
{
    keel_camera_error error;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    CHECK_STATUS(keel_camera_last_error(camera, &error), KEEL_OK);
    return error;
}

keel_camera_frame_info camera_read_frame(keel_camera *camera, uint8_t *buffer, uint32_t size)
{
    keel_camera_frame_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_read_frame(camera, buffer, size, 2000, &info), KEEL_OK);
    return info;
}

keel_camera_encoded_info camera_read_encoded(keel_camera *camera, uint8_t *buffer, uint32_t size)
{
    keel_camera_encoded_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_STATUS(keel_camera_read_encoded(camera, buffer, size, 2000, &info), KEEL_OK);
    return info;
}

void camera_check_pattern(const uint8_t *frame, const keel_camera_frame_info *info, int hflip, int vflip)
{
    uint32_t n = (uint32_t)(info->sequence & 0xFFu);
    uint32_t x;
    uint32_t y;
    for (y = 0; y < info->height; ++y) {
        uint32_t sy = vflip ? info->height - 1u - y : y;
        const uint8_t *row = frame + info->offset0 + (size_t)y * info->stride0;
        for (x = 0; x < info->width; ++x) {
            uint32_t sx = hflip ? info->width - 1u - x : x;
            if (info->format == KEEL_CAMERA_FORMAT_I420) {
                CHECK_EQ(row[x], (7u * sx + 3u * sy + n) & 0xFFu);
            } else {
                uint8_t r = (uint8_t)((sx + n) & 0xFFu);
                uint8_t g = (uint8_t)((sy + n) & 0xFFu);
                uint8_t b = (uint8_t)((sx + sy) & 0xFFu);
                const uint8_t *pixel = row + 3u * x;
                CHECK_EQ(pixel[0], info->format == KEEL_CAMERA_FORMAT_BGR888 ? b : r);
                CHECK_EQ(pixel[1], g);
                CHECK_EQ(pixel[2], info->format == KEEL_CAMERA_FORMAT_BGR888 ? r : b);
            }
        }
        /* Row padding never carries pixels. */
        for (x = info->format == KEEL_CAMERA_FORMAT_I420 ? info->width : info->width * 3u; x < info->stride0; ++x) {
            CHECK_EQ(row[x], 0xA5);
        }
    }
    if (info->format != KEEL_CAMERA_FORMAT_I420) {
        return;
    }
    for (y = 0; y < info->height / 2u; ++y) {
        uint32_t sy = vflip ? info->height / 2u - 1u - y : y;
        const uint8_t *u = frame + info->offset1 + (size_t)y * info->stride1;
        const uint8_t *v = frame + info->offset2 + (size_t)y * info->stride2;
        for (x = 0; x < info->width / 2u; ++x) {
            uint32_t sx = hflip ? info->width / 2u - 1u - x : x;
            CHECK_EQ(u[x], (5u * sx + n) & 0xFFu);
            CHECK_EQ(v[x], (5u * sy + 2u * n) & 0xFFu);
        }
    }
}

void camera_fault(const char *name)
{
    if (name == NULL) {
        unsetenv("KEEL_CAMERA_TEST_FAULT");
    } else {
        setenv("KEEL_CAMERA_TEST_FAULT", name, 1);
    }
}

static int count_entries(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    int count = 0;
    CHECK(dir != NULL);
    while ((entry = readdir(dir)) != NULL) {
        count += entry->d_name[0] != '.';
    }
    closedir(dir);
    return count - (strcmp(path, "/proc/self/fd") == 0); /* opendir's own fd */
}

int camera_count_fds(void)
{
    return count_entries("/proc/self/fd");
}

int camera_count_threads(void)
{
    return count_entries("/proc/self/task");
}

void camera_sleep_ms(unsigned ms)
{
    struct timespec pause;
    pause.tv_sec = ms / 1000u;
    pause.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
    }
}

/* ---- receivers ---------------------------------------------------------- */

struct camera_receiver {
    char kind;
    int fd;          /* listening, connected or UDP socket */
    int peer;        /* accepted connection ('L') */
    int wake[2];
    uint16_t port;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t *data;
    size_t bytes;
    size_t capacity;
    size_t datagrams;
    size_t largest;
};

static void append(camera_receiver *r, const uint8_t *data, size_t bytes)
{
    pthread_mutex_lock(&r->lock);
    if (r->bytes + bytes > r->capacity) {
        r->capacity = (r->bytes + bytes) * 2u;
        r->data = realloc(r->data, r->capacity);
        CHECK(r->data != NULL);
    }
    memcpy(r->data + r->bytes, data, bytes);
    r->bytes += bytes;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

static void *receive_main(void *arg)
{
    camera_receiver *r = arg;
    uint8_t buffer[70000];
    for (;;) {
        struct pollfd fds[2];
        int fd = r->kind == 'L' ? (r->peer >= 0 ? r->peer : r->fd) : r->fd;
        ssize_t got;
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[1].fd = r->wake[0];
        fds[1].events = POLLIN;
        if (poll(fds, 2, -1) < 0) {
            continue;
        }
        if (fds[1].revents) {
            return NULL;
        }
        if (r->kind == 'L' && r->peer < 0) {
            r->peer = accept4(r->fd, NULL, NULL, SOCK_CLOEXEC);
            continue;
        }
        got = recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            return NULL; /* the sender closed */
        }
        append(r, buffer, (size_t)got);
        if (r->kind == 'U') {
            pthread_mutex_lock(&r->lock);
            r->datagrams++;
            if ((size_t)got > r->largest) {
                r->largest = (size_t)got;
            }
            pthread_mutex_unlock(&r->lock);
        }
    }
}

camera_receiver *camera_receiver_start(char kind, uint16_t port)
{
    camera_receiver *r = calloc(1, sizeof(*r));
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    CHECK(r != NULL);
    r->kind = kind;
    r->peer = -1;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    CHECK(pipe2(r->wake, O_CLOEXEC) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    r->fd = socket(AF_INET, (kind == 'U' ? SOCK_DGRAM : SOCK_STREAM) | SOCK_CLOEXEC, 0);
    CHECK(r->fd >= 0);
    if (kind == 'C') {
        CHECK(connect(r->fd, (struct sockaddr *)&address, sizeof(address)) == 0);
        r->port = port;
    } else {
        int big = 4 * 1024 * 1024;
        setsockopt(r->fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
        CHECK(bind(r->fd, (struct sockaddr *)&address, sizeof(address)) == 0);
        CHECK(getsockname(r->fd, (struct sockaddr *)&address, &length) == 0);
        r->port = ntohs(address.sin_port);
        if (kind == 'L') {
            CHECK(listen(r->fd, 4) == 0);
        }
    }
    CHECK(pthread_create(&r->thread, NULL, receive_main, r) == 0);
    return r;
}

uint16_t camera_receiver_port(const camera_receiver *receiver)
{
    return receiver->port;
}

size_t camera_receiver_wait(camera_receiver *receiver, size_t bytes, unsigned timeout_ms)
{
    struct timespec deadline;
    size_t got;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000u;
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&receiver->lock);
    while (receiver->bytes < bytes) {
        if (pthread_cond_timedwait(&receiver->cond, &receiver->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    got = receiver->bytes;
    pthread_mutex_unlock(&receiver->lock);
    return got;
}

uint8_t *camera_receiver_stop(camera_receiver *receiver, size_t *bytes, size_t *datagrams, size_t *largest_datagram)
{
    uint8_t *data;
    char one = 1;
    CHECK(write(receiver->wake[1], &one, 1) == 1);
    pthread_join(receiver->thread, NULL);
    close(receiver->fd);
    if (receiver->peer >= 0) {
        close(receiver->peer);
    }
    close(receiver->wake[0]);
    close(receiver->wake[1]);
    data = receiver->data;
    *bytes = receiver->bytes;
    if (datagrams != NULL) {
        *datagrams = receiver->datagrams;
    }
    if (largest_datagram != NULL) {
        *largest_datagram = receiver->largest;
    }
    pthread_cond_destroy(&receiver->cond);
    pthread_mutex_destroy(&receiver->lock);
    free(receiver);
    return data;
}

unsigned camera_count_nal(const uint8_t *data, size_t bytes, unsigned type)
{
    unsigned count = 0;
    size_t i;
    for (i = 0; i + 3u < bytes; ++i) {
        if (data[i] == 0 && data[i + 1u] == 0 && data[i + 2u] == 1 && (data[i + 3u] & 0x1Fu) == type) {
            count++;
        }
    }
    return count;
}

unsigned camera_first_nal(const uint8_t *data, size_t bytes)
{
    if (bytes >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return data[4] & 0x1Fu;
    }
    if (bytes >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return data[3] & 0x1Fu;
    }
    return 0;
}

uint16_t camera_free_port(void)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    uint16_t port;
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    port = ntohs(address.sin_port);
    close(fd);
    return port;
}
