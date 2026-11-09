/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Lifecycle and process hygiene (SPEC section 26.4): open, start and close
 * cycles leave no thread and no file descriptor behind; every descriptor
 * the library opens is close-on-exec; its threads block every signal; it
 * installs no signal handler.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "camera_test.h"

static keel_camera *open_busy_camera(camera_receiver *tcp, camera_receiver *udp, uint16_t *listen_port)
{
    keel_camera_options o = camera_options();
    keel_camera_encoder_options e = camera_encoder(KEEL_CAMERA_CODEC_H264);
    keel_camera *camera;
    char url[64];
    uint32_t output;
    o.width = 160;
    o.height = 120;
    o.framerate_milli = 60000;
    camera = camera_open(&o);
    CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_OK);
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", camera_receiver_port(tcp));
    CHECK_STATUS(keel_camera_output_start(camera, url, NULL, &output), KEEL_OK);
    snprintf(url, sizeof(url), "udp://127.0.0.1:%u", camera_receiver_port(udp));
    CHECK_STATUS(keel_camera_output_start(camera, url, NULL, &output), KEEL_OK);
    *listen_port = camera_free_port();
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u?listen", *listen_port);
    CHECK_STATUS(keel_camera_output_start(camera, url, NULL, &output), KEEL_OK);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    return camera;
}

static void reopen_cycles(void)
{
    int fds = camera_count_fds();
    int threads = camera_count_threads();
    int cycle;
    for (cycle = 0; cycle < 20; ++cycle) {
        camera_receiver *tcp = camera_receiver_start('L', 0);
        camera_receiver *udp = camera_receiver_start('U', 0);
        uint16_t listen_port;
        keel_camera *camera = open_busy_camera(tcp, udp, &listen_port);
        keel_camera_stream_info stream = camera_stream(camera);
        uint8_t *frame = malloc(stream.frame_bytes);
        size_t bytes;
        CHECK(frame != NULL);
        (void)camera_read_frame(camera, frame, stream.frame_bytes);
        if (cycle % 2 == 0) {
            CHECK_STATUS(keel_camera_stop(camera), KEEL_OK);
        }
        CHECK_STATUS(keel_camera_close(camera), KEEL_OK);
        free(frame);
        free(camera_receiver_stop(tcp, &bytes, NULL, NULL));
        free(camera_receiver_stop(udp, &bytes, NULL, NULL));
    }
    /* The library's threads are gone and its descriptors closed. */
    CHECK_EQ(camera_count_threads(), (unsigned)threads);
    CHECK_EQ(camera_count_fds(), (unsigned)fds);
}

static void cloexec(void)
{
    camera_receiver *tcp = camera_receiver_start('L', 0);
    camera_receiver *udp = camera_receiver_start('U', 0);
    uint16_t listen_port;
    keel_camera *camera;
    DIR *dir;
    struct dirent *entry;
    int unmarked = 0;
    size_t bytes;
    /* Every descriptor of this process before the library opens any is
     * marked, so an unmarked one after can only be the library's. */
    dir = opendir("/proc/self/fd");
    CHECK(dir != NULL);
    while ((entry = readdir(dir)) != NULL) {
        int fd = atoi(entry->d_name);
        if (entry->d_name[0] != '.' && fd > 2) {
            fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
        }
    }
    closedir(dir);
    camera = open_busy_camera(tcp, udp, &listen_port);
    {
        /* A receiver of the listening output, so its client socket exists. */
        camera_receiver *receiver = camera_receiver_start('C', listen_port);
        CHECK(camera_receiver_wait(receiver, 100, 3000) >= 100);
        dir = opendir("/proc/self/fd");
        CHECK(dir != NULL);
        while ((entry = readdir(dir)) != NULL) {
            int fd = atoi(entry->d_name);
            if (entry->d_name[0] == '.' || fd <= 2 || fd == dirfd(dir)) {
                continue;
            }
            if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC)) {
                char link[64];
                char target[256];
                ssize_t length;
                snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
                length = readlink(link, target, sizeof(target) - 1u);
                target[length > 0 ? length : 0] = '\0';
                fprintf(stderr, "fd %d (%s) is not close-on-exec\n", fd, target);
                unmarked++;
            }
        }
        closedir(dir);
        free(camera_receiver_stop(receiver, &bytes, NULL, NULL));
    }
    keel_camera_close(camera);
    free(camera_receiver_stop(tcp, &bytes, NULL, NULL));
    free(camera_receiver_stop(udp, &bytes, NULL, NULL));
    CHECK_EQ(unmarked, 0);
}

/* SigBlk of every thread of this process except the main one. */
static void check_threads_block_signals(pid_t self)
{
    DIR *dir = opendir("/proc/self/task");
    struct dirent *entry;
    int library_threads = 0;
    CHECK(dir != NULL);
    while ((entry = readdir(dir)) != NULL) {
        char path[300];
        char line[256];
        char name[64] = "";
        FILE *status;
        if (entry->d_name[0] == '.' || atoi(entry->d_name) == self) {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
        status = fopen(path, "r");
        if (status == NULL) {
            continue;
        }
        if (fgets(name, sizeof(name), status) == NULL) {
            name[0] = '\0';
        }
        fclose(status);
        if (strncmp(name, "keel-cam", 8) != 0) {
            continue; /* the receivers' threads */
        }
        library_threads++;
        snprintf(path, sizeof(path), "/proc/self/task/%s/status", entry->d_name);
        status = fopen(path, "r");
        CHECK(status != NULL);
        while (fgets(line, sizeof(line), status) != NULL) {
            if (strncmp(line, "SigBlk:", 7) == 0) {
                unsigned long long blocked = strtoull(line + 7, NULL, 16);
                /* SIGINT, SIGTERM and SIGPIPE at least. */
                CHECK(blocked & (1ull << (SIGINT - 1)));
                CHECK(blocked & (1ull << (SIGTERM - 1)));
                CHECK(blocked & (1ull << (SIGPIPE - 1)));
            }
        }
        fclose(status);
    }
    closedir(dir);
    CHECK(library_threads >= 3); /* capture, encoder and outputs */
}

static void signals(void)
{
    struct sigaction before[65];
    struct sigaction after;
    camera_receiver *tcp = camera_receiver_start('L', 0);
    camera_receiver *udp = camera_receiver_start('U', 0);
    uint16_t listen_port;
    keel_camera *camera;
    size_t bytes;
    int number;
    for (number = 1; number < 65; ++number) {
        sigaction(number, NULL, &before[number]);
    }
    camera = open_busy_camera(tcp, udp, &listen_port);
    camera_sleep_ms(200);
    check_threads_block_signals(getpid());
    keel_camera_close(camera);
    for (number = 1; number < 65; ++number) {
        if (sigaction(number, NULL, &after) == 0) {
            CHECK(after.sa_handler == before[number].sa_handler);
        }
    }
    free(camera_receiver_stop(tcp, &bytes, NULL, NULL));
    free(camera_receiver_stop(udp, &bytes, NULL, NULL));
}

static void close_running_with_outputs(void)
{
    camera_receiver *tcp = camera_receiver_start('L', 0);
    camera_receiver *udp = camera_receiver_start('U', 0);
    uint16_t listen_port;
    keel_camera *camera = open_busy_camera(tcp, udp, &listen_port);
    camera_receiver *client = camera_receiver_start('C', listen_port);
    size_t bytes;
    CHECK(camera_receiver_wait(client, 100, 3000) >= 100);
    /* Close straight away, with every output busy and a client attached. */
    CHECK_STATUS(keel_camera_close(camera), KEEL_OK);
    free(camera_receiver_stop(client, &bytes, NULL, NULL));
    free(camera_receiver_stop(tcp, &bytes, NULL, NULL));
    CHECK(bytes > 0);
    free(camera_receiver_stop(udp, &bytes, NULL, NULL));
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"reopen_cycles", reopen_cycles},
        {"cloexec", cloexec},
        {"signals", signals},
        {"close_running_with_outputs", close_running_with_outputs},
    };
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
