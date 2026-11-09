/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Outputs the library drives: files, a TCP connection, a listening TCP
 * socket with several receivers, UDP datagrams, joining a running stream,
 * receivers that disconnect, refusals and limits, and frames read while
 * streaming (SPEC section 26.7). Receivers run on loopback sockets.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "camera_test.h"

static keel_camera *open_with(uint32_t codec)
{
    keel_camera_options o = camera_options();
    keel_camera_encoder_options e = camera_encoder(codec);
    keel_camera *camera;
    o.width = 320;
    o.height = 240;
    o.framerate_milli = 60000;
    camera = camera_open(&o);
    if (codec == KEEL_CAMERA_CODEC_H264) {
        e.intra_period = 15;
    }
    CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_OK);
    return camera;
}

static keel_camera_output_stats output_stats(keel_camera *camera, uint32_t output)
{
    keel_camera_output_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    CHECK_STATUS(keel_camera_get_output_stats(camera, output, &stats), KEEL_OK);
    return stats;
}

static uint8_t *read_file(const char *path, size_t *bytes)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long size;
    CHECK(file != NULL);
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc((size_t)size + 1u);
    CHECK(data != NULL);
    CHECK(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    *bytes = (size_t)size;
    return data;
}

static void temporary_path(char *out, size_t size, const char *suffix)
{
    const char *dir = getenv("TMPDIR");
    snprintf(out, size, "%s/keel-camera-test-%d%s", dir != NULL && dir[0] != '\0' ? dir : "/tmp", (int)getpid(),
             suffix);
}

static void file_h264(void)
{
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
    keel_camera_output_stats stats;
    char path[256];
    uint32_t output = 0;
    size_t bytes;
    uint8_t *data;
    temporary_path(path, sizeof(path), ".h264");
    CHECK_STATUS(keel_camera_output_start(camera, path, NULL, &output), KEEL_OK);
    CHECK(output != 0);
    CHECK_EQ(camera_stats(camera).outputs, 1);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    camera_sleep_ms(600);
    CHECK_STATUS(keel_camera_stop(camera), KEEL_OK);
    stats = output_stats(camera, output);
    CHECK_EQ(stats.state, KEEL_CAMERA_OUTPUT_RUNNING);
    CHECK_EQ(stats.status, KEEL_OK);
    CHECK_EQ(stats.clients, 1);
    CHECK(stats.chunks >= 20);
    CHECK_STATUS(keel_camera_output_stop(camera, output), KEEL_OK);
    CHECK_EQ(camera_stats(camera).outputs, 0);
    CHECK_STATUS(keel_camera_output_stop(camera, output), KEEL_E_NOT_FOUND);
    data = read_file(path, &bytes);
    CHECK_EQ(bytes, (size_t)stats.bytes);
    /* A file starts with the parameter sets and a keyframe. */
    CHECK_EQ(camera_first_nal(data, bytes), 7);
    CHECK(camera_count_nal(data, bytes, 5) >= 2);
    CHECK_EQ(camera_count_nal(data, bytes, 5) + camera_count_nal(data, bytes, 1), stats.chunks);
    CHECK_EQ(camera_count_nal(data, bytes, 7), camera_count_nal(data, bytes, 5));
    free(data);
    unlink(path);
    keel_camera_close(camera);
}

static void file_mjpeg(void)
{
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_MJPEG);
    char path[256];
    uint32_t output = 0;
    size_t bytes;
    size_t i;
    unsigned images = 0;
    uint8_t *data;
    temporary_path(path, sizeof(path), ".mjpeg");
    CHECK_STATUS(keel_camera_output_start(camera, path, NULL, &output), KEEL_OK);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    camera_sleep_ms(300);
    /* Closing the handle finishes the file. */
    CHECK_STATUS(keel_camera_close(camera), KEEL_OK);
    data = read_file(path, &bytes);
    CHECK(bytes > 0);
    CHECK(data[0] == 0xFF && data[1] == 0xD8);
    for (i = 0; i + 1u < bytes; ++i) {
        images += data[i] == 0xFF && data[i + 1u] == 0xD9;
    }
    CHECK(images >= 8);
    CHECK(data[bytes - 2u] == 0xFF && data[bytes - 1u] == 0xD9);
    free(data);
    unlink(path);
}

static void tcp_client(void)
{
    camera_receiver *receiver = camera_receiver_start('L', 0);
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
    keel_camera_output_stats stats;
    char url[64];
    uint32_t output;
    size_t bytes;
    uint8_t *data;
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", camera_receiver_port(receiver));
    CHECK_STATUS(keel_camera_output_start(camera, url, NULL, &output), KEEL_OK);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    CHECK(camera_receiver_wait(receiver, 2000, 3000) >= 2000);
    camera_sleep_ms(200);
    stats = output_stats(camera, output);
    CHECK_EQ(stats.state, KEEL_CAMERA_OUTPUT_RUNNING);
    keel_camera_close(camera);
    data = camera_receiver_stop(receiver, &bytes, NULL, NULL);
    CHECK_EQ(camera_first_nal(data, bytes), 7);
    CHECK(camera_count_nal(data, bytes, 5) >= 1);
    free(data);
}

static void tcp_refused(void)
{
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
    keel_camera_error error;
    uint32_t output = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    char url[64];
    /* A port that nothing listens on: bind it, read its number, close it. */
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    close(fd);
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", ntohs(address.sin_port));
    CHECK_STATUS(keel_camera_output_start(camera, url, NULL, &output), KEEL_E_NOT_FOUND);
    error = camera_last_error(camera);
    CHECK_EQ(error.resource, KEEL_CAMERA_RESOURCE_NETWORK);
    CHECK_EQ(error.os_error, ECONNREFUSED);
    CHECK(strstr(error.message, "refused") != NULL);
    CHECK_STATUS(keel_camera_output_start(camera, "tcp://keel-camera-test.invalid:5000", NULL, &output),
                 KEEL_E_NOT_FOUND);
    CHECK(strstr(camera_last_error(camera).message, "does not resolve") != NULL);
    CHECK_EQ(camera_stats(camera).outputs, 0);
    keel_camera_close(camera);
}

static void tcp_listen_clients(void)
{
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
    keel_camera_output_options options;
    keel_camera_output_stats stats;
    camera_receiver *first;
    camera_receiver *second;
    camera_receiver *third;
    struct sigaction action;
    struct sigaction previous;
    uint32_t output;
    uint16_t port;
    size_t bytes;
    uint8_t *data;
    /* A receiver that vanishes must never raise SIGPIPE: keep its default
     * action (terminate) for this test. */
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &action, &previous);
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.max_clients = 2;
    CHECK_STATUS(keel_camera_output_start(camera, "tcp://127.0.0.1:0?listen", &options, &output),
                 KEEL_E_INVALID_ARGUMENT); /* port 0 is not a port */
    {
        /* Find a free port. */
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address;
        socklen_t length = sizeof(address);
        char url[64];
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
        CHECK(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
        port = ntohs(address.sin_port);
        close(fd);
        snprintf(url, sizeof(url), "tcp://127.0.0.1:%u?listen", port);
        CHECK_STATUS(keel_camera_output_start(camera, url, &options, &output), KEEL_OK);
    }
    stats = output_stats(camera, output);
    CHECK_EQ(stats.state, KEEL_CAMERA_OUTPUT_WAITING);
    CHECK_EQ(stats.clients, 0);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    camera_sleep_ms(300); /* a running stream that receivers join */
    first = camera_receiver_start('C', port);
    second = camera_receiver_start('C', port);
    CHECK(camera_receiver_wait(first, 3000, 3000) >= 3000);
    CHECK(camera_receiver_wait(second, 3000, 3000) >= 3000);
    stats = output_stats(camera, output);
    CHECK_EQ(stats.state, KEEL_CAMERA_OUTPUT_RUNNING);
    CHECK_EQ(stats.clients, 2);
    /* A third is over max_clients: its connection closes at once. */
    third = camera_receiver_start('C', port);
    camera_sleep_ms(200);
    data = camera_receiver_stop(third, &bytes, NULL, NULL);
    CHECK_EQ(bytes, 0);
    free(data);
    /* The first leaves abruptly; the second keeps receiving. */
    data = camera_receiver_stop(first, &bytes, NULL, NULL);
    CHECK_EQ(camera_first_nal(data, bytes), 7); /* each receiver starts at a keyframe with headers */
    free(data);
    camera_sleep_ms(300);
    {
        size_t before = camera_receiver_wait(second, 0, 0);
        CHECK(camera_receiver_wait(second, before + 2000, 3000) >= before + 2000);
    }
    stats = output_stats(camera, output);
    CHECK_EQ(stats.clients, 1);
    CHECK_EQ(stats.state, KEEL_CAMERA_OUTPUT_RUNNING);
    data = camera_receiver_stop(second, &bytes, NULL, NULL);
    CHECK_EQ(camera_first_nal(data, bytes), 7);
    free(data);
    camera_sleep_ms(200);
    CHECK_EQ(output_stats(camera, output).state, KEEL_CAMERA_OUTPUT_WAITING);
    CHECK_STATUS(keel_camera_output_stop(camera, output), KEEL_OK);
    keel_camera_close(camera);
    sigaction(SIGPIPE, &previous, NULL);
}

static void listen_port_in_use(void)
{
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
    camera_receiver *holder = camera_receiver_start('L', 0);
    char url[64];
    uint32_t output;
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u?listen", camera_receiver_port(holder));
    CHECK_STATUS(keel_camera_output_start(camera, url, NULL, &output), KEEL_E_IN_USE);
    CHECK(strstr(camera_last_error(camera).message, "already in use") != NULL);
    free(camera_receiver_stop(holder, &(size_t){0}, NULL, NULL));
    keel_camera_close(camera);
}

static void udp(void)
{
    camera_receiver *receiver = camera_receiver_start('U', 0);
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_MJPEG);
    keel_camera_output_options options;
    char url[64];
    uint32_t output;
    size_t bytes;
    size_t datagrams;
    size_t largest;
    uint8_t *data;
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.datagram_bytes = 512;
    snprintf(url, sizeof(url), "udp://127.0.0.1:%u", camera_receiver_port(receiver));
    CHECK_STATUS(keel_camera_output_start(camera, url, &options, &output), KEEL_OK);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    CHECK(camera_receiver_wait(receiver, 1000, 3000) >= 1000);
    keel_camera_close(camera);
    data = camera_receiver_stop(receiver, &bytes, &datagrams, &largest);
    CHECK(datagrams >= 2);
    CHECK(largest <= 512);
    CHECK(data[0] == 0xFF && data[1] == 0xD8);
    free(data);
}

static void join_running(void)
{
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
    keel_camera_output_stats stats;
    char path[256];
    uint32_t output;
    size_t bytes;
    uint8_t *data;
    temporary_path(path, sizeof(path), "-join.h264");
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    camera_sleep_ms(150);
    CHECK_STATUS(keel_camera_output_start(camera, path, NULL, &output), KEEL_OK);
    camera_sleep_ms(300);
    stats = output_stats(camera, output);
    CHECK_STATUS(keel_camera_output_stop(camera, output), KEEL_OK);
    /* Joined at a keyframe requested at once, with its parameter sets. */
    data = read_file(path, &bytes);
    CHECK_EQ(camera_first_nal(data, bytes), 7);
    CHECK(stats.chunks >= 10);
    free(data);
    unlink(path);
    keel_camera_close(camera);
}

static void refusals(void)
{
    keel_camera *camera = camera_open(NULL);
    keel_camera_output_options options;
    keel_camera_error error;
    uint32_t outputs[KEEL_CAMERA_OUTPUTS_MAX + 1];
    char path[256];
    uint32_t output;
    unsigned i;
    /* No encoder yet. */
    CHECK_STATUS(keel_camera_output_start(camera, "/tmp/x.h264", NULL, &output), KEEL_E_STATE);
    CHECK(strstr(camera_last_error(camera).message, "keel_camera_set_encoder") != NULL);
    keel_camera_close(camera);
    camera = open_with(KEEL_CAMERA_CODEC_H264);
    {
        static const struct {
            const char *url;
            int32_t status;
            const char *words;
        } cases[] = {
            {"", KEEL_E_INVALID_ARGUMENT, "empty"},
            {"-", KEEL_E_INVALID_ARGUMENT, "keel_camera_read_encoded"},
            {"http://host/x", KEEL_E_INVALID_ARGUMENT, "unknown scheme"},
            {"/tmp/x.avi", KEEL_E_INVALID_ARGUMENT, "unknown file type"},
            {"/tmp/x.mjpeg", KEEL_E_INVALID_ARGUMENT, "MJPEG"},
            {"tcp://host", KEEL_E_INVALID_ARGUMENT, "HOST:PORT"},
            {"tcp://host:70000", KEEL_E_INVALID_ARGUMENT, "port"},
            {"tcp://[::1:5000", KEEL_E_INVALID_ARGUMENT, "IPV6"},
            {"tcp://::1:5000", KEEL_E_INVALID_ARGUMENT, "brackets"},
            {"tcp://:5000", KEEL_E_INVALID_ARGUMENT, "listen"},
            {"tcp://host:5000?bogus", KEEL_E_INVALID_ARGUMENT, "listen"},
            {"udp://host:5000?listen", KEEL_E_INVALID_ARGUMENT, "cannot listen"},
            {"udp://:5000", KEEL_E_INVALID_ARGUMENT, "udp://HOST:PORT"},
            {"/nonexistent-directory/x.h264", KEEL_E_NOT_FOUND, "cannot create"},
        };
        size_t c;
        for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
            CHECK_STATUS(keel_camera_output_start(camera, cases[c].url, NULL, &output), cases[c].status);
            error = camera_last_error(camera);
            if (strstr(error.message, cases[c].words) == NULL) {
                fprintf(stderr, "%s: message '%s' lacks '%s'\n", cases[c].url, error.message, cases[c].words);
                exit(1);
            }
        }
    }
    /* libavformat outputs: UNSUPPORTED without it (KEEL_E_OK or another
     * status with it, which this build may have). */
    if (strstr(keel_camera_version_string(), "libav") == NULL) {
        CHECK_STATUS(keel_camera_output_start(camera, "/tmp/x.mp4", NULL, &output), KEEL_E_UNSUPPORTED);
        CHECK(strstr(camera_last_error(camera).message, "libavformat") != NULL);
        CHECK_STATUS(keel_camera_output_start(camera, "rtsp://127.0.0.1:8554/robot", NULL, &output),
                     KEEL_E_UNSUPPORTED);
    }
    /* Options. */
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.max_clients = 2;
    temporary_path(path, sizeof(path), "-options.h264");
    CHECK_STATUS(keel_camera_output_start(camera, path, &options, &output), KEEL_E_INVALID_ARGUMENT);
    options.max_clients = 0;
    options.datagram_bytes = 1000;
    CHECK_STATUS(keel_camera_output_start(camera, path, &options, &output), KEEL_E_INVALID_ARGUMENT);
    options.datagram_bytes = 0;
    options.flags = 1;
    CHECK_STATUS(keel_camera_output_start(camera, path, &options, &output), KEEL_E_INVALID_ARGUMENT);
    options.flags = 0;
    options.queue = 257;
    CHECK_STATUS(keel_camera_output_start(camera, path, &options, &output), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_output_start(camera, NULL, NULL, &output), KEEL_E_INVALID_ARGUMENT);
    CHECK_STATUS(keel_camera_output_start(camera, path, NULL, NULL), KEEL_E_INVALID_ARGUMENT);
    /* At most KEEL_CAMERA_OUTPUTS_MAX outputs. */
    for (i = 0; i < KEEL_CAMERA_OUTPUTS_MAX; ++i) {
        char each[300];
        snprintf(each, sizeof(each), "%s.%u.h264", path, i);
        CHECK_STATUS(keel_camera_output_start(camera, each, NULL, &outputs[i]), KEEL_OK);
    }
    CHECK_STATUS(keel_camera_output_start(camera, path, NULL, &outputs[i]), KEEL_E_AGAIN);
    /* The encoder cannot change under outputs. */
    {
        keel_camera_encoder_options e = camera_encoder(KEEL_CAMERA_CODEC_MJPEG);
        CHECK_STATUS(keel_camera_set_encoder(camera, &e), KEEL_E_STATE);
    }
    for (i = 0; i < KEEL_CAMERA_OUTPUTS_MAX; ++i) {
        char each[300];
        snprintf(each, sizeof(each), "%s.%u.h264", path, i);
        CHECK_STATUS(keel_camera_output_stop(camera, outputs[i]), KEEL_OK);
        unlink(each);
    }
    keel_camera_close(camera);
}

static void output_failure(void)
{
    camera_receiver *receiver = camera_receiver_start('L', 0);
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
    keel_camera_output_stats stats;
    char url[64];
    uint32_t output;
    size_t bytes;
    int tries;
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", camera_receiver_port(receiver));
    CHECK_STATUS(keel_camera_output_start(camera, url, NULL, &output), KEEL_OK);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    CHECK(camera_receiver_wait(receiver, 500, 3000) >= 500);
    /* The receiver goes away: the output fails, the capture goes on. */
    free(camera_receiver_stop(receiver, &bytes, NULL, NULL));
    stats = output_stats(camera, output);
    for (tries = 0; tries < 40 && stats.state != KEEL_CAMERA_OUTPUT_FAILED; ++tries) {
        camera_sleep_ms(50);
        stats = output_stats(camera, output);
    }
    CHECK_EQ(stats.state, KEEL_CAMERA_OUTPUT_FAILED);
    CHECK_EQ(stats.status, KEEL_E_PERIPHERAL);
    CHECK(stats.os_error == EPIPE || stats.os_error == ECONNRESET);
    CHECK(strstr(stats.message, "disconnected") != NULL);
    CHECK_EQ(camera_stats(camera).state, KEEL_CAMERA_STATE_RUNNING);
    CHECK_STATUS(keel_camera_output_stop(camera, output), KEEL_E_PERIPHERAL);
    CHECK(strstr(camera_last_error(camera).message, "disconnected") != NULL);
    keel_camera_close(camera);
}

/* Container files: written through libavformat when the library has it,
 * refused with KEEL_E_UNSUPPORTED when it does not. */
static void libav_files(void)
{
    static const struct {
        const char *suffix;
        const uint8_t magic[4];
        size_t offset;
    } files[] = {
        {".mkv", {0x1A, 0x45, 0xDF, 0xA3}, 0},
        {".mp4", {'f', 't', 'y', 'p'}, 4},
        {".ts", {0x47, 0, 0, 0}, 0},
    };
    int with_libav = strstr(keel_camera_version_string(), "libav") != NULL;
    size_t i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
        char path[256];
        uint32_t output;
        size_t bytes;
        uint8_t *data;
        temporary_path(path, sizeof(path), files[i].suffix);
        if (!with_libav) {
            CHECK_STATUS(keel_camera_output_start(camera, path, NULL, &output), KEEL_E_UNSUPPORTED);
            keel_camera_close(camera);
            continue;
        }
        CHECK_STATUS(keel_camera_output_start(camera, path, NULL, &output), KEEL_OK);
        CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
        camera_sleep_ms(500);
        /* Closing writes the trailer (the index of an .mp4). */
        CHECK_STATUS(keel_camera_close(camera), KEEL_OK);
        data = read_file(path, &bytes);
        CHECK(bytes > 1000);
        CHECK(memcmp(data + files[i].offset, files[i].magic, files[i].suffix[1] == 't' ? 1u : 4u) == 0);
        if (files[i].suffix[1] == 't') {
            CHECK_EQ(data[188], 0x47); /* the next transport packet */
        }
        free(data);
        unlink(path);
    }
}

static void frames_while_streaming(void)
{
    camera_receiver *receiver = camera_receiver_start('U', 0);
    keel_camera *camera = open_with(KEEL_CAMERA_CODEC_H264);
    keel_camera_stream_info stream = camera_stream(camera);
    uint8_t *frame = malloc(stream.frame_bytes);
    char url[64];
    uint32_t output;
    size_t bytes;
    int n;
    CHECK(frame != NULL);
    snprintf(url, sizeof(url), "udp://127.0.0.1:%u", camera_receiver_port(receiver));
    CHECK_STATUS(keel_camera_output_start(camera, url, NULL, &output), KEEL_OK);
    CHECK_STATUS(keel_camera_start(camera), KEEL_OK);
    for (n = 0; n < 20; ++n) {
        keel_camera_frame_info info = camera_read_frame(camera, frame, stream.frame_bytes);
        camera_check_pattern(frame, &info, 0, 0);
    }
    CHECK(camera_receiver_wait(receiver, 1000, 3000) >= 1000);
    keel_camera_close(camera);
    free(camera_receiver_stop(receiver, &bytes, NULL, NULL));
    free(frame);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"file_h264", file_h264},
        {"file_mjpeg", file_mjpeg},
        {"tcp_client", tcp_client},
        {"tcp_refused", tcp_refused},
        {"tcp_listen_clients", tcp_listen_clients},
        {"listen_port_in_use", listen_port_in_use},
        {"udp", udp},
        {"join_running", join_running},
        {"refusals", refusals},
        {"output_failure", output_failure},
        {"libav_files", libav_files},
        {"frames_while_streaming", frames_while_streaming},
    };
    return run_cases(cases, sizeof(cases) / sizeof(cases[0]), argc, argv);
}
