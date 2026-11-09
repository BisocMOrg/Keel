/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Colour signalling of H.264 streams (SPEC section 26.4). Frames are
 * full-range BT.601 (sYCC). The Raspberry Pi 4 encoder writes a sequence
 * parameter set (SPS) without a video signal type for YUV input, and
 * decoders then assume limited range, which crushes shadows and highlights.
 * Every SPS without a video signal type is rewritten to carry one: video
 * format unspecified, full range, BT.709 primaries, sRGB transfer and BT.601
 * matrix (libcamera's ColorSpace::Sycc). An SPS that already signals one, or
 * that cannot be parsed, is copied unchanged; other NAL units are copied.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "camera/camera_internal.h"

enum {
    SPS_MAX = 256,          /* larger parameter sets are copied unchanged */
    VIDEO_FORMAT_UNSPECIFIED = 5,
    PRIMARIES_BT709 = 1,
    TRANSFER_SRGB = 13,
    MATRIX_BT601 = 6
};

typedef struct reader {
    const uint8_t *data;
    uint32_t bits;
    uint32_t position;
    int error;
} reader;

static uint32_t read_bits(reader *r, uint32_t count)
{
    uint32_t value = 0;
    while (count-- > 0) {
        if (r->position >= r->bits) {
            r->error = 1;
            return 0;
        }
        value = (value << 1) | ((uint32_t)(r->data[r->position >> 3] >> (7u - (r->position & 7u))) & 1u);
        r->position++;
    }
    return value;
}

static uint32_t read_ue(reader *r)
{
    uint32_t zeros = 0;
    while (!r->error && read_bits(r, 1) == 0) {
        if (++zeros > 31) {
            r->error = 1;
            return 0;
        }
    }
    return zeros == 0 ? 0u : ((1u << zeros) - 1u) + read_bits(r, zeros);
}

static int32_t read_se(reader *r)
{
    uint32_t code = read_ue(r);
    return (code & 1u) ? (int32_t)((code + 1u) / 2u) : -(int32_t)(code / 2u);
}

static void skip_scaling_list(reader *r, uint32_t size)
{
    int32_t last = 8;
    int32_t next = 8;
    uint32_t i;
    for (i = 0; i < size && !r->error; ++i) {
        if (next != 0) {
            next = (last + read_se(r) + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
}

typedef struct writer {
    uint8_t *data;
    uint32_t capacity;
    uint32_t bits;
    int error;
} writer;

static void write_bits(writer *w, uint32_t value, uint32_t count)
{
    while (count-- > 0) {
        uint32_t byte = w->bits >> 3;
        if (byte >= w->capacity) {
            w->error = 1;
            return;
        }
        if ((w->bits & 7u) == 0) {
            w->data[byte] = 0;
        }
        if ((value >> count) & 1u) {
            w->data[byte] |= (uint8_t)(0x80u >> (w->bits & 7u));
        }
        w->bits++;
    }
}

static void copy_bits(writer *w, reader *r, uint32_t end)
{
    while (r->position < end && !w->error) {
        write_bits(w, read_bits(r, 1), 1);
    }
}

static void write_signal_type(writer *w)
{
    write_bits(w, 1, 1);                        /* video_signal_type_present_flag */
    write_bits(w, VIDEO_FORMAT_UNSPECIFIED, 3);
    write_bits(w, 1, 1);                        /* video_full_range_flag */
    write_bits(w, 1, 1);                        /* colour_description_present_flag */
    write_bits(w, PRIMARIES_BT709, 8);
    write_bits(w, TRANSFER_SRGB, 8);
    write_bits(w, MATRIX_BT601, 8);
}

/* Builds the new SPS payload (RBSP, after the NAL header) into out; returns
 * its length, or 0 to keep the original. */
static uint32_t rewrite_sps(const uint8_t *rbsp, uint32_t bytes, uint8_t *out, uint32_t capacity)
{
    reader r = {rbsp, bytes * 8u, 0, 0};
    writer w = {out, capacity, 0, 0};
    uint32_t stop;
    uint32_t profile;
    uint32_t vui_flag;
    uint32_t type;
    /* The stop bit is the last set bit. */
    for (stop = bytes * 8u; stop > 0; --stop) {
        if (((uint32_t)rbsp[(stop - 1u) >> 3] >> (7u - ((stop - 1u) & 7u))) & 1u) {
            break;
        }
    }
    if (stop == 0) {
        return 0;
    }
    stop--;
    profile = read_bits(&r, 8);
    read_bits(&r, 16); /* constraint flags, level */
    read_ue(&r);       /* seq_parameter_set_id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        uint32_t chroma = read_ue(&r);
        if (chroma == 3) {
            read_bits(&r, 1);
        }
        read_ue(&r);
        read_ue(&r);
        read_bits(&r, 1);
        if (read_bits(&r, 1)) {
            uint32_t i;
            for (i = 0; i < (chroma != 3 ? 8u : 12u) && !r.error; ++i) {
                if (read_bits(&r, 1)) {
                    skip_scaling_list(&r, i < 6 ? 16u : 64u);
                }
            }
        }
    }
    read_ue(&r); /* log2_max_frame_num_minus4 */
    type = read_ue(&r);
    if (type == 0) {
        read_ue(&r);
    } else if (type == 1) {
        uint32_t cycle;
        uint32_t i;
        read_bits(&r, 1);
        read_se(&r);
        read_se(&r);
        cycle = read_ue(&r);
        for (i = 0; i < cycle && !r.error; ++i) {
            read_se(&r);
        }
    }
    read_ue(&r);      /* max_num_ref_frames */
    read_bits(&r, 1); /* gaps_in_frame_num_value_allowed_flag */
    read_ue(&r);
    read_ue(&r);
    if (!read_bits(&r, 1)) { /* frame_mbs_only_flag */
        read_bits(&r, 1);
    }
    read_bits(&r, 1);
    if (read_bits(&r, 1)) { /* frame_cropping_flag */
        read_ue(&r);
        read_ue(&r);
        read_ue(&r);
        read_ue(&r);
    }
    vui_flag = r.position;
    if (r.error || vui_flag >= stop) {
        return 0;
    }
    r.position = 0;
    copy_bits(&w, &r, vui_flag);
    if (!read_bits(&r, 1)) {
        /* No VUI: add one that carries only the signal type. */
        write_bits(&w, 1, 1);
        write_bits(&w, 0, 2); /* aspect ratio, overscan */
        write_signal_type(&w);
        write_bits(&w, 0, 7); /* chroma location, timing, NAL and VCL HRD, pic struct, bitstream restriction */
    } else {
        uint32_t flag;
        write_bits(&w, 1, 1);
        if (read_bits(&r, 1)) { /* aspect_ratio_info_present_flag */
            uint32_t idc = read_bits(&r, 8);
            if (idc == 255) {
                read_bits(&r, 32);
            }
        }
        if (read_bits(&r, 1)) { /* overscan_info_present_flag */
            read_bits(&r, 1);
        }
        flag = r.position;
        if (r.error || read_bits(&r, 1)) {
            return 0; /* a signal type is there already */
        }
        r.position = vui_flag + 1u;
        copy_bits(&w, &r, flag);
        r.position = flag + 1u;
        write_signal_type(&w);
        copy_bits(&w, &r, stop);
    }
    write_bits(&w, 1, 1); /* rbsp_stop_one_bit */
    while (w.bits & 7u) {
        write_bits(&w, 0, 1);
    }
    return w.error || r.error ? 0 : w.bits / 8u;
}

/* Appends payload with emulation prevention; returns 0 or -1 if full. */
static int put_escaped(uint8_t *out, uint32_t capacity, uint32_t *length, const uint8_t *payload, uint32_t bytes)
{
    uint32_t zeros = 0;
    uint32_t i;
    for (i = 0; i < bytes; ++i) {
        if (zeros >= 2 && payload[i] <= 3) {
            if (*length >= capacity) {
                return -1;
            }
            out[(*length)++] = 3;
            zeros = 0;
        }
        if (*length >= capacity) {
            return -1;
        }
        out[(*length)++] = payload[i];
        zeros = payload[i] == 0 ? zeros + 1u : 0u;
    }
    return 0;
}

uint32_t cam_h264_signal_full_range(const uint8_t *in, uint32_t bytes, uint8_t *out, uint32_t capacity)
{
    uint32_t length = 0;
    uint32_t i = 0;
    while (i < bytes) {
        uint32_t start = i;
        uint32_t payload;
        uint32_t end;
        /* The NAL unit spans from its start code to the next one. */
        while (start + 3u <= bytes && !(in[start] == 0 && in[start + 1u] == 0 && in[start + 2u] == 1)) {
            ++start;
        }
        if (start + 3u > bytes) {
            start = bytes;
        }
        payload = start + 3u;
        end = payload;
        while (end + 3u <= bytes && !(in[end] == 0 && in[end + 1u] == 0 && (in[end + 2u] == 1 ||
                                                                            (in[end + 2u] == 0 && end + 3u < bytes &&
                                                                             in[end + 3u] == 1)))) {
            ++end;
        }
        if (end + 3u > bytes) {
            end = bytes;
        }
        if (payload < bytes && (in[payload] & 0x1Fu) == 7u && end - payload - 1u <= SPS_MAX) {
            uint8_t rbsp[SPS_MAX];
            uint8_t rewritten[SPS_MAX + 16];
            uint32_t rbsp_bytes = 0;
            uint32_t zeros = 0;
            uint32_t k;
            uint32_t new_bytes;
            for (k = payload + 1u; k < end; ++k) {
                if (zeros >= 2 && in[k] == 3) {
                    zeros = 0;
                    continue; /* emulation prevention byte */
                }
                rbsp[rbsp_bytes++] = in[k];
                zeros = in[k] == 0 ? zeros + 1u : 0u;
            }
            new_bytes = rewrite_sps(rbsp, rbsp_bytes, rewritten, sizeof(rewritten));
            if (new_bytes > 0) {
                if (length + (payload + 1u - i) > capacity) {
                    return 0;
                }
                memcpy(out + length, in + i, payload + 1u - i); /* leading bytes, start code, header */
                length += payload + 1u - i;
                if (put_escaped(out, capacity, &length, rewritten, new_bytes) != 0) {
                    return 0;
                }
                i = end;
                continue;
            }
        }
        if (length + (end - i) > capacity) {
            return 0;
        }
        memcpy(out + length, in + i, end - i);
        length += end - i;
        i = end;
    }
    return length;
}
