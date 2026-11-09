/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Outputs the library drives itself (SPEC section 26.7): files, a TCP
 * connection, UDP datagrams and a listening TCP socket serving several
 * receivers. Each output has its own thread and bounded queue; a slow
 * destination loses chunks (counted), never memory, and never slows the
 * camera. A failed destination stops only its own output.
 *
 * Every socket and file is opened close-on-exec, and sockets send with
 * MSG_NOSIGNAL: a receiver that disconnects ends its connection with EPIPE,
 * never with SIGPIPE.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "camera/camera_internal.h"

enum {
    CONNECT_TIMEOUT_MS = 3000,
    SEND_TIMEOUT_MS = 2000,  /* a stream send that stalls this long fails the output */
    STOP_DRAIN_MS = 2000     /* close waits this long for queued data, then cuts the connection */
};

/* ---- shared ------------------------------------------------------------- */

void cam_output_failed_locked(cam_output *output, int32_t status, int os_error, const char *format, ...)
{
    va_list args;
    if (output->state == KEEL_CAMERA_OUTPUT_FAILED) {
        return;
    }
    output->state = KEEL_CAMERA_OUTPUT_FAILED;
    output->failure.status = status;
    output->failure.resource = output->destination.kind == CAM_DEST_FILE ? KEEL_CAMERA_RESOURCE_FILE
                               : output->destination.kind == CAM_DEST_LIBAV ? KEEL_CAMERA_RESOURCE_LIBAV
                                                                             : KEEL_CAMERA_RESOURCE_NETWORK;
    output->failure.os_error = os_error;
    cam_copy_string(output->failure.path, sizeof(output->failure.path),
                    output->destination.path[0] != '\0' ? output->destination.path : output->destination.host);
    va_start(args, format);
    vsnprintf(output->failure.message, sizeof(output->failure.message), format, args);
    va_end(args);
    output->chunks_dropped += output->queue.count;
    cam_packet_queue_clear(&output->queue);
}

int cam_send_all(int fd, const uint8_t *data, size_t bytes)
{
    while (bytes > 0) {
        ssize_t sent = send(fd, data, bytes, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        data += sent;
        bytes -= (size_t)sent;
    }
    return 0;
}

int cam_write_all(int fd, const uint8_t *data, size_t bytes)
{
    while (bytes > 0) {
        ssize_t written = write(fd, data, bytes);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        data += written;
        bytes -= (size_t)written;
    }
    return 0;
}

static const char *describe(int os_error)
{
    return os_error == EPIPE || os_error == ECONNRESET ? "the receiver disconnected" : strerror(os_error);
}

/* ---- stream outputs: file, TCP, UDP, libavformat ------------------------ */

static int write_file(cam_output *output, const cam_packet *packet)
{
    int rc = cam_write_all(output->fd, packet->data, packet->bytes);
    if (rc != 0) {
        pthread_mutex_lock(&output->lock);
        cam_output_failed_locked(output, KEEL_E_PERIPHERAL, rc, "%s: write failed: %s",
                                 output->destination.path, strerror(rc));
        pthread_mutex_unlock(&output->lock);
        return -1;
    }
    return 0;
}

static int write_tcp(cam_output *output, const cam_packet *packet)
{
    int rc = cam_send_all(output->fd, packet->data, packet->bytes);
    if (rc != 0) {
        pthread_mutex_lock(&output->lock);
        cam_output_failed_locked(output, KEEL_E_PERIPHERAL, rc, "tcp://%s:%u: %s", output->destination.host,
                                 output->destination.port,
                                 rc == EAGAIN ? "the receiver stopped reading" : describe(rc));
        pthread_mutex_unlock(&output->lock);
        return -1;
    }
    return 0;
}

typedef struct udp_state {
    struct sockaddr_storage address;
    socklen_t address_length;
} udp_state;

static int write_udp(cam_output *output, const cam_packet *packet)
{
    const udp_state *udp = output->kind_state;
    uint32_t limit = output->options.datagram_bytes;
    uint32_t offset = 0;
    while (offset < packet->bytes) {
        uint32_t length = packet->bytes - offset < limit ? packet->bytes - offset : limit;
        ssize_t sent = sendto(output->fd, packet->data + offset, length, MSG_NOSIGNAL,
                              (const struct sockaddr *)&udp->address, udp->address_length);
        if (sent < 0) {
            int rc = errno;
            if (rc == EINTR) {
                continue;
            }
            /* A full socket buffer or an ICMP error loses this datagram only. */
            if (rc == EAGAIN || rc == ENOBUFS || rc == ECONNREFUSED) {
                offset += length;
                continue;
            }
            pthread_mutex_lock(&output->lock);
            cam_output_failed_locked(output, KEEL_E_PERIPHERAL, rc, "udp://%s:%u: %s", output->destination.host,
                                     output->destination.port, strerror(rc));
            pthread_mutex_unlock(&output->lock);
            return -1;
        }
        offset += length;
    }
    return 0;
}

static void finish_fd(cam_output *output)
{
    if (output->fd >= 0) {
        close(output->fd);
        output->fd = -1;
    }
}

static const cam_output_ops file_ops = {write_file, finish_fd};
static const cam_output_ops tcp_ops = {write_tcp, finish_fd};
static const cam_output_ops udp_ops = {write_udp, finish_fd};

static void mark_done(cam_output *output)
{
    pthread_mutex_lock(&output->lock);
    output->thread_done = 1;
    pthread_cond_broadcast(&output->cond);
    pthread_mutex_unlock(&output->lock);
}

static void *stream_main(void *arg)
{
    cam_output *output = arg;
    pthread_mutex_lock(&output->lock);
    for (;;) {
        cam_packet *packet;
        while (output->queue.count == 0 && !output->stopping) {
            pthread_cond_wait(&output->cond, &output->lock);
        }
        packet = cam_packet_queue_pop(&output->queue);
        if (packet == NULL) {
            break; /* stopping, and everything queued is written */
        }
        pthread_mutex_unlock(&output->lock);
        if (output->ops->write(output, packet) == 0) {
            pthread_mutex_lock(&output->lock);
            output->bytes += packet->bytes;
            output->chunks++;
            output->state = KEEL_CAMERA_OUTPUT_RUNNING;
            pthread_mutex_unlock(&output->lock);
        }
        cam_packet_unref(packet);
        pthread_mutex_lock(&output->lock);
    }
    pthread_mutex_unlock(&output->lock);
    mark_done(output);
    return NULL;
}

/* ---- listening TCP output ----------------------------------------------- */

typedef struct client {
    int fd;
    cam_packet_queue queue;
    cam_packet *current;  /* packet being sent */
    uint32_t offset;      /* bytes of current already sent */
} client;

typedef struct listen_state {
    int listen_fd;
    client clients[KEEL_CAMERA_CLIENTS_MAX];
} listen_state;

static void drop_client(cam_output *output, client *c)
{
    output->chunks_dropped += c->queue.count + (c->current != NULL ? 1u : 0u) + c->queue.dropped;
    cam_packet_queue_free(&c->queue);
    if (c->current != NULL) {
        cam_packet_unref(c->current);
        c->current = NULL;
    }
    close(c->fd);
    c->fd = -1;
    output->clients--;
}

static void accept_clients(cam_output *output, listen_state *listen)
{
    for (;;) {
        uint32_t i;
        int one = 1;
        int fd = accept4(listen->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return;
        }
        for (i = 0; i < output->options.max_clients; ++i) {
            if (listen->clients[i].fd < 0) {
                break;
            }
        }
        if (i == output->options.max_clients ||
            cam_packet_queue_init(&listen->clients[i].queue, output->options.queue, output->codec) != 0) {
            close(fd); /* full: the receiver sees the connection close */
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        listen->clients[i].fd = fd;
        listen->clients[i].current = NULL;
        listen->clients[i].offset = 0;
        output->clients++;
        if (output->request_keyframe != NULL) {
            output->request_keyframe(output->request_context);
        }
    }
}

/* Sends what the client can take without blocking; 0, or -1 to drop it. */
static int serve_client(cam_output *output, client *c)
{
    for (;;) {
        ssize_t sent;
        if (c->current == NULL) {
            c->current = cam_packet_queue_pop(&c->queue);
            c->offset = 0;
            if (c->current == NULL) {
                return 0;
            }
        }
        sent = send(c->fd, c->current->data + c->offset, c->current->bytes - c->offset,
                    MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        }
        c->offset += (uint32_t)sent;
        output->bytes += (uint64_t)sent;
        if (c->offset == c->current->bytes) {
            output->chunks++;
            cam_packet_unref(c->current);
            c->current = NULL;
        }
    }
}

static void *listen_main(void *arg)
{
    cam_output *output = arg;
    listen_state *listen = output->kind_state;
    struct pollfd fds[2 + KEEL_CAMERA_CLIENTS_MAX];
    client *polled[KEEL_CAMERA_CLIENTS_MAX];
    pthread_mutex_lock(&output->lock);
    while (!output->stopping) {
        nfds_t count = 2;
        uint32_t i;
        fds[0].fd = output->wake_fd;
        fds[0].events = POLLIN;
        fds[1].fd = listen->listen_fd;
        fds[1].events = POLLIN;
        for (i = 0; i < output->options.max_clients; ++i) {
            client *c = &listen->clients[i];
            if (c->fd < 0) {
                continue;
            }
            fds[count].fd = c->fd;
            fds[count].events = (short)(POLLIN | (c->current != NULL || c->queue.count > 0 ? POLLOUT : 0));
            polled[count - 2] = c;
            count++;
        }
        pthread_mutex_unlock(&output->lock);
        if (poll(fds, count, -1) < 0 && errno != EINTR) {
            pthread_mutex_lock(&output->lock);
            cam_output_failed_locked(output, KEEL_E_PERIPHERAL, errno, "poll failed: %s", strerror(errno));
            break;
        }
        if (fds[0].revents & POLLIN) {
            uint64_t value;
            (void)!read(output->wake_fd, &value, sizeof(value));
        }
        pthread_mutex_lock(&output->lock);
        if (output->stopping) {
            break;
        }
        if (fds[1].revents & POLLIN) {
            accept_clients(output, listen);
        }
        for (i = 2; i < count; ++i) {
            client *c = polled[i - 2];
            int gone = (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            if (c->fd < 0) {
                continue;
            }
            if (!gone && (fds[i].revents & POLLIN)) {
                char discard[256];
                ssize_t got = recv(c->fd, discard, sizeof(discard), MSG_DONTWAIT);
                gone = got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
            }
            if (gone || serve_client(output, c) != 0) {
                drop_client(output, c);
            }
        }
        output->state = output->clients > 0 ? KEEL_CAMERA_OUTPUT_RUNNING : KEEL_CAMERA_OUTPUT_WAITING;
    }
    pthread_mutex_unlock(&output->lock);
    mark_done(output);
    return NULL;
}

static void finish_listen(cam_output *output)
{
    listen_state *listen = output->kind_state;
    uint32_t i;
    for (i = 0; i < KEEL_CAMERA_CLIENTS_MAX; ++i) {
        if (listen->clients[i].fd >= 0) {
            drop_client(output, &listen->clients[i]);
        }
    }
    close(listen->listen_fd);
    close(output->wake_fd);
    output->wake_fd = -1;
}

static const cam_output_ops listen_ops = {NULL, finish_listen};

/* ---- opening ------------------------------------------------------------ */

static int32_t resolve(const cam_destination *destination, int socktype, int passive, struct addrinfo **out,
                       cam_error *error)
{
    struct addrinfo hints;
    char port[8];
    int rc;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = passive ? AI_PASSIVE : AI_ADDRCONFIG;
    snprintf(port, sizeof(port), "%u", destination->port);
    rc = getaddrinfo(destination->host[0] != '\0' ? destination->host : NULL, port, &hints, out);
    if (rc != 0) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_NETWORK, 0, destination->host,
                        "%s does not resolve: %s", destination->host, gai_strerror(rc));
    }
    return KEEL_OK;
}

/* Connects with a deadline; returns the blocking socket or -errno. */
static int connect_with_timeout(const struct addrinfo *address)
{
    struct pollfd pfd;
    int fd = socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    address->ai_protocol);
    int rc;
    int so_error = 0;
    socklen_t length = sizeof(so_error);
    if (fd < 0) {
        return -errno;
    }
    if (connect(fd, address->ai_addr, address->ai_addrlen) != 0 && errno != EINPROGRESS) {
        rc = errno;
        close(fd);
        return -rc;
    }
    pfd.fd = fd;
    pfd.events = POLLOUT;
    rc = poll(&pfd, 1, CONNECT_TIMEOUT_MS);
    if (rc <= 0) {
        close(fd);
        return rc == 0 ? -ETIMEDOUT : -errno;
    }
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length);
    if (so_error != 0) {
        close(fd);
        return -so_error;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

static int32_t open_tcp(cam_output *output, cam_error *error)
{
    const cam_destination *d = &output->destination;
    struct addrinfo *addresses;
    struct addrinfo *a;
    struct timeval timeout;
    int one = 1;
    int fd = -ECONNREFUSED;
    int32_t rc = resolve(d, SOCK_STREAM, 0, &addresses, error);
    if (rc != KEEL_OK) {
        return rc;
    }
    for (a = addresses; a != NULL; a = a->ai_next) {
        fd = connect_with_timeout(a);
        if (fd >= 0) {
            break;
        }
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        int os_error = -fd;
        if (os_error == ECONNREFUSED) {
            return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_NETWORK, os_error, d->host,
                            "tcp://%s:%u refused the connection: start the receiver first, or stream to "
                            "tcp://:%u?listen", d->host, d->port, d->port);
        }
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_NETWORK, os_error, d->host,
                        "tcp://%s:%u: %s", d->host, d->port,
                        os_error == ETIMEDOUT ? "no answer within 3 s" : strerror(os_error));
    }
    timeout.tv_sec = SEND_TIMEOUT_MS / 1000;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    output->fd = fd;
    output->ops = &tcp_ops;
    return KEEL_OK;
}

static int32_t open_udp(cam_output *output, cam_error *error)
{
    const cam_destination *d = &output->destination;
    struct addrinfo *addresses;
    udp_state *udp;
    int32_t rc = resolve(d, SOCK_DGRAM, 0, &addresses, error);
    int fd;
    if (rc != KEEL_OK) {
        return rc;
    }
    fd = socket(addresses->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    udp = calloc(1, sizeof(*udp));
    if (fd < 0 || udp == NULL) {
        int os_error = fd < 0 ? errno : ENOMEM;
        if (fd >= 0) {
            close(fd);
        }
        free(udp);
        freeaddrinfo(addresses);
        return cam_fail(error, fd < 0 ? KEEL_E_SETUP_IO : KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_NETWORK,
                        os_error, d->host, "udp://%s:%u: %s", d->host, d->port, strerror(os_error));
    }
    memcpy(&udp->address, addresses->ai_addr, addresses->ai_addrlen);
    udp->address_length = addresses->ai_addrlen;
    freeaddrinfo(addresses);
    output->fd = fd;
    output->kind_state = udp;
    output->ops = &udp_ops;
    return KEEL_OK;
}

static int32_t open_listen(cam_output *output, cam_error *error)
{
    const cam_destination *d = &output->destination;
    struct addrinfo *addresses;
    struct addrinfo *a;
    listen_state *listen_ = calloc(1, sizeof(*listen_));
    int fd = -1;
    int os_error = 0;
    uint32_t i;
    int32_t rc;
    if (listen_ == NULL) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    rc = resolve(d, SOCK_STREAM, 1, &addresses, error);
    if (rc != KEEL_OK) {
        free(listen_);
        return rc;
    }
    /* With no address, prefer one IPv6 socket that also accepts IPv4. */
    for (a = addresses; a != NULL && fd < 0; a = a->ai_next) {
        int one = 1;
        int zero = 0;
        if (d->host[0] == '\0' && a->ai_family != AF_INET6 && a->ai_next != NULL) {
            continue;
        }
        fd = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            os_error = errno;
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (a->ai_family == AF_INET6 && d->host[0] == '\0') {
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        }
        if (bind(fd, a->ai_addr, a->ai_addrlen) != 0 || listen(fd, 8) != 0) {
            os_error = errno;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        free(listen_);
        if (os_error == EADDRINUSE) {
            return cam_fail(error, KEEL_E_IN_USE, KEEL_CAMERA_RESOURCE_NETWORK, os_error, d->host,
                            "port %u is already in use: choose another port", d->port);
        }
        if (os_error == EACCES) {
            return cam_fail(error, KEEL_E_PERMISSION, KEEL_CAMERA_RESOURCE_NETWORK, os_error, d->host,
                            "port %u needs privileges: use a port above 1023", d->port);
        }
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_NETWORK, os_error, d->host,
                        "cannot listen on port %u: %s", d->port, strerror(os_error));
    }
    output->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (output->wake_fd < 0) {
        os_error = errno;
        close(fd);
        free(listen_);
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_THREAD, os_error, NULL,
                        "eventfd failed: %s", strerror(os_error));
    }
    listen_->listen_fd = fd;
    for (i = 0; i < KEEL_CAMERA_CLIENTS_MAX; ++i) {
        listen_->clients[i].fd = -1;
    }
    output->kind_state = listen_;
    output->ops = &listen_ops;
    return KEEL_OK;
}

static int32_t open_file(cam_output *output, cam_error *error)
{
    const char *path = output->destination.path;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        int os_error = errno;
        int32_t status = cam_status_for_errno(os_error);
        return cam_fail(error, status, KEEL_CAMERA_RESOURCE_FILE, os_error, path, "cannot create %s: %s", path,
                        strerror(os_error));
    }
    output->fd = fd;
    output->ops = &file_ops;
    return KEEL_OK;
}

static void output_free(cam_output *output)
{
    cam_packet_queue_free(&output->queue);
    free(output->kind_state);
    pthread_cond_destroy(&output->cond);
    pthread_mutex_destroy(&output->lock);
    free(output);
}

int32_t cam_output_open(const cam_destination *destination, uint32_t codec,
                        const keel_camera_output_options *options, uint32_t width, uint32_t height,
                        uint32_t framerate_milli, cam_keyframe_request request_keyframe, void *request_context,
                        cam_output **out, cam_error *error)
{
    cam_output *output = calloc(1, sizeof(*output));
    int32_t rc;
    int thread_rc;
    *out = NULL;
    if (output == NULL) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    output->destination = *destination;
    output->options = *options;
    output->codec = codec;
    output->width = width;
    output->height = height;
    output->framerate_milli = framerate_milli;
    output->request_keyframe = request_keyframe;
    output->request_context = request_context;
    output->fd = -1;
    output->wake_fd = -1;
    output->state = KEEL_CAMERA_OUTPUT_WAITING;
    pthread_mutex_init(&output->lock, NULL);
    cam_cond_init(&output->cond);
    if (cam_packet_queue_init(&output->queue, options->queue, codec) != 0) {
        output_free(output);
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    switch (destination->kind) {
    case CAM_DEST_FILE:
        rc = open_file(output, error);
        break;
    case CAM_DEST_TCP:
        rc = open_tcp(output, error);
        break;
    case CAM_DEST_UDP:
        rc = open_udp(output, error);
        break;
    case CAM_DEST_TCP_LISTEN:
        rc = open_listen(output, error);
        break;
    default:
        rc = cam_output_libav_open(output, error);
        break;
    }
    if (rc != KEEL_OK) {
        output_free(output);
        return rc;
    }
    thread_rc = cam_thread_start(&output->thread, "keel-cam-output",
                                 destination->kind == CAM_DEST_TCP_LISTEN ? listen_main : stream_main, output);
    if (thread_rc != 0) {
        output->ops->finish(output);
        output_free(output);
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_THREAD, thread_rc, NULL,
                        "cannot start the output thread: %s", strerror(thread_rc));
    }
    output->thread_started = 1;
    *out = output;
    return KEEL_OK;
}

void cam_output_push(cam_output *output, cam_packet *packet)
{
    pthread_mutex_lock(&output->lock);
    if (output->state == KEEL_CAMERA_OUTPUT_FAILED || output->stopping) {
        pthread_mutex_unlock(&output->lock);
        return;
    }
    if (output->destination.kind == CAM_DEST_TCP_LISTEN) {
        listen_state *listen = output->kind_state;
        uint32_t i;
        uint64_t one = 1;
        for (i = 0; i < output->options.max_clients; ++i) {
            if (listen->clients[i].fd >= 0) {
                cam_packet_queue_push(&listen->clients[i].queue, packet);
            }
        }
        (void)!write(output->wake_fd, &one, sizeof(one));
    } else {
        cam_packet_queue_push(&output->queue, packet);
        pthread_cond_signal(&output->cond);
    }
    pthread_mutex_unlock(&output->lock);
}

void cam_output_stats(cam_output *output, keel_camera_output_stats *out)
{
    uint64_t dropped;
    pthread_mutex_lock(&output->lock);
    dropped = output->chunks_dropped + output->queue.dropped;
    if (output->destination.kind == CAM_DEST_TCP_LISTEN) {
        listen_state *listen = output->kind_state;
        uint32_t i;
        for (i = 0; i < KEEL_CAMERA_CLIENTS_MAX; ++i) {
            if (listen->clients[i].fd >= 0) {
                dropped += listen->clients[i].queue.dropped;
            }
        }
        out->clients = output->clients;
    } else {
        out->clients = output->state == KEEL_CAMERA_OUTPUT_FAILED ? 0u : 1u;
    }
    out->state = output->state;
    out->status = output->failure.status;
    out->os_error = output->failure.os_error;
    out->bytes = output->bytes;
    out->chunks = output->chunks;
    out->chunks_dropped = dropped;
    cam_copy_string(out->message, sizeof(out->message), output->failure.message);
    pthread_mutex_unlock(&output->lock);
}

int32_t cam_output_close(cam_output *output)
{
    struct timespec deadline;
    int32_t status;
    uint64_t one = 1;
    pthread_mutex_lock(&output->lock);
    output->stopping = 1;
    pthread_cond_broadcast(&output->cond);
    if (output->wake_fd >= 0) {
        (void)!write(output->wake_fd, &one, sizeof(one));
    }
    cam_deadline(&deadline, STOP_DRAIN_MS);
    while (!output->thread_done) {
        if (pthread_cond_timedwait(&output->cond, &output->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    if (!output->thread_done && output->fd >= 0) {
        /* The receiver stopped reading: cut the connection so the send ends. */
        shutdown(output->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&output->lock);
    if (output->thread_started) {
        pthread_join(output->thread, NULL);
    }
    output->ops->finish(output);
    status = output->failure.status;
    output_free(output);
    return status;
}
