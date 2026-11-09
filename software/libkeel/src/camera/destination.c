/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Output destinations (SPEC section 26.7):
 *   tcp://HOST:PORT                connect to a receiver
 *   tcp://[ADDRESS]:PORT?listen    accept receivers ("?listen=1" too)
 *   udp://HOST:PORT                datagrams
 *   rtsp:// rtsps:// rtmp:// rtmps:// srt://   libavformat, H.264
 *   PATH.h264 PATH.264             H.264 Annex B
 *   PATH.mjpeg PATH.mjpg           concatenated JPEG images
 *   PATH.mp4 PATH.mov PATH.ts      libavformat, H.264
 *   PATH.mkv                       libavformat, H.264 or MJPEG
 * HOST is a name, an IPv4 address or a bracketed IPv6 address.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "camera/camera_internal.h"

static int32_t bad(cam_error *error, const char *url, const char *why)
{
    return cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, url, "%s: %s", url, why);
}

static int32_t wrong_codec(cam_error *error, const char *url, const char *needs)
{
    return cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, url,
                    "%s: this destination needs the %s codec; set it with keel_camera_set_encoder()", url, needs);
}

/* Splits "HOST:PORT" or "[V6]:PORT" (with an optional "?query"). */
static int32_t parse_host_port(const char *url, const char *rest, cam_destination *out, char *query,
                               size_t query_size, cam_error *error)
{
    char text[KEEL_CAMERA_PATH_MAX];
    char *mark;
    char *port;
    char *end;
    long value;
    cam_copy_string(text, sizeof(text), rest);
    query[0] = '\0';
    mark = strchr(text, '?');
    if (mark != NULL) {
        cam_copy_string(query, query_size, mark + 1);
        *mark = '\0';
    }
    if (text[0] == '[') {
        char *close = strchr(text, ']');
        if (close == NULL || close[1] != ':') {
            return bad(error, url, "expected [IPV6-ADDRESS]:PORT");
        }
        *close = '\0';
        cam_copy_string(out->host, sizeof(out->host), text + 1);
        port = close + 2;
    } else {
        char *colon = strrchr(text, ':');
        if (colon == NULL) {
            return bad(error, url, "expected HOST:PORT");
        }
        *colon = '\0';
        if (strchr(text, ':') != NULL) {
            return bad(error, url, "write an IPv6 address in brackets: [ADDRESS]:PORT");
        }
        cam_copy_string(out->host, sizeof(out->host), text);
        port = colon + 1;
    }
    if (strchr(out->host, '/') != NULL) {
        return bad(error, url, "a host name cannot contain '/'");
    }
    value = strtol(port, &end, 10);
    if (port[0] == '\0' || *end != '\0' || value < 1 || value > 65535) {
        return bad(error, url, "the port must be a number from 1 to 65535");
    }
    out->port = (uint16_t)value;
    return KEEL_OK;
}

static int has_extension(const char *path, const char *extension)
{
    size_t length = strlen(path);
    size_t ext = strlen(extension);
    return length > ext && strcasecmp(path + length - ext, extension) == 0;
}

int32_t cam_destination_parse(const char *url, uint32_t codec, cam_destination *out, cam_error *error)
{
    static const struct {
        const char *scheme;
        const char *format;
    } protocols[] = {
        {"rtsp://", "rtsp"}, {"rtsps://", "rtsp"}, {"rtmp://", "flv"}, {"rtmps://", "flv"}, {"srt://", "mpegts"},
    };
    char query[64];
    size_t i;
    int32_t rc;
    memset(out, 0, sizeof(*out));
    if (url[0] == '\0') {
        return bad(error, url, "the destination is empty");
    }
    if (strcmp(url, "-") == 0) {
        return bad(error, url, "standard output belongs to the program: read the chunks with "
                               "keel_camera_read_encoded() and write them");
    }
    if (strncmp(url, "tcp://", 6) == 0 || strncmp(url, "udp://", 6) == 0) {
        int udp = url[0] == 'u';
        rc = parse_host_port(url, url + 6, out, query, sizeof(query), error);
        if (rc != KEEL_OK) {
            return rc;
        }
        if (strcmp(query, "listen") == 0 || strcmp(query, "listen=1") == 0) {
            if (udp) {
                return bad(error, url, "a UDP output sends; it cannot listen");
            }
            out->kind = CAM_DEST_TCP_LISTEN;
            return KEEL_OK;
        }
        if (query[0] != '\0') {
            return bad(error, url, "the only option is ?listen (TCP)");
        }
        if (out->host[0] == '\0') {
            return bad(error, url, udp ? "name the receiver: udp://HOST:PORT"
                                       : "name the receiver, or add ?listen to wait for receivers");
        }
        out->kind = udp ? CAM_DEST_UDP : CAM_DEST_TCP;
        return KEEL_OK;
    }
    for (i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i) {
        if (strncmp(url, protocols[i].scheme, strlen(protocols[i].scheme)) == 0) {
            if (codec != KEEL_CAMERA_CODEC_H264) {
                return wrong_codec(error, url, "H.264");
            }
            out->kind = CAM_DEST_LIBAV;
            out->libav_format = protocols[i].format;
            cam_copy_string(out->path, sizeof(out->path), url);
            return KEEL_OK;
        }
    }
    if (strstr(url, "://") != NULL) {
        return bad(error, url, "unknown scheme: use tcp://, udp://, rtsp://, rtmp://, srt:// or a file path");
    }
    cam_copy_string(out->path, sizeof(out->path), url);
    if (has_extension(url, ".h264") || has_extension(url, ".264")) {
        if (codec != KEEL_CAMERA_CODEC_H264) {
            return wrong_codec(error, url, "H.264");
        }
        out->kind = CAM_DEST_FILE;
    } else if (has_extension(url, ".mjpeg") || has_extension(url, ".mjpg")) {
        if (codec != KEEL_CAMERA_CODEC_MJPEG) {
            return wrong_codec(error, url, "MJPEG");
        }
        out->kind = CAM_DEST_FILE;
    } else if (has_extension(url, ".mp4") || has_extension(url, ".mov") || has_extension(url, ".ts")) {
        if (codec != KEEL_CAMERA_CODEC_H264) {
            return wrong_codec(error, url, "H.264");
        }
        out->kind = CAM_DEST_LIBAV;
        out->libav_format = has_extension(url, ".mp4") ? "mp4" : has_extension(url, ".mov") ? "mov" : "mpegts";
    } else if (has_extension(url, ".mkv")) {
        out->kind = CAM_DEST_LIBAV;
        out->libav_format = "matroska";
    } else {
        return bad(error, url, "unknown file type: use .h264, .264, .mjpeg, .mjpg, .mp4, .mkv, .mov or .ts");
    }
    return KEEL_OK;
}
