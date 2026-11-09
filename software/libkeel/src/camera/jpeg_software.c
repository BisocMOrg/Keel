/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Software JPEG for stills larger than the hardware image encoder takes
 * (1920x1920 on the Raspberry Pi 4, SPEC section 26.8): libjpeg (on the
 * robot libjpeg-turbo) encodes the I420 frame as raw YCbCr 4:2:0 data, so
 * no colour conversion happens. Built when CMake finds libjpeg
 * (KEEL_CAMERA_JPEG); without it such stills are KEEL_E_UNSUPPORTED.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "camera/camera_internal.h"

#ifdef KEEL_CAMERA_HAVE_JPEG

#include <setjmp.h>
#include <stdio.h>

#include <jpeglib.h>

typedef struct jpeg_failure {
    struct jpeg_error_mgr manager;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
} jpeg_failure;

static void on_error(j_common_ptr cinfo)
{
    jpeg_failure *failure = (jpeg_failure *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, failure->message);
    longjmp(failure->jump, 1);
}

static void on_message(j_common_ptr cinfo)
{
    (void)cinfo; /* warnings stay quiet */
}

int cam_jpeg_software_available(void)
{
    return 1;
}

/* Where libjpeg writes: outside the frame of the function that calls
 * setjmp, so its values survive a longjmp. */
typedef struct jpeg_target {
    unsigned char *memory;
    unsigned long size;
    char message[JMSG_LENGTH_MAX];
} jpeg_target;

/* Compresses copy (an I420 frame with the stream's layout); 0 or -1. */
static int compress(const uint8_t *copy, const cam_stream *stream, uint32_t quality, jpeg_target *target)
{
    struct jpeg_compress_struct cinfo;
    jpeg_failure failure;
    JSAMPROW rows[3][16];
    JSAMPARRAY planes[3] = {rows[0], rows[1], rows[2]};
    uint32_t line;
    cinfo.err = jpeg_std_error(&failure.manager);
    failure.manager.error_exit = on_error;
    failure.manager.output_message = on_message;
    if (setjmp(failure.jump) != 0) {
        jpeg_destroy_compress(&cinfo);
        memcpy(target->message, failure.message, sizeof(target->message));
        return -1;
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &target->memory, &target->size);
    cinfo.image_width = stream->width;
    cinfo.image_height = stream->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);
    jpeg_set_colorspace(&cinfo, JCS_YCbCr);
    jpeg_set_quality(&cinfo, (int)quality, TRUE);
    cinfo.raw_data_in = TRUE;
    cinfo.dct_method = JDCT_ISLOW;
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;
    jpeg_start_compress(&cinfo, TRUE);
    /* Raw data goes in 16-line strips; rows past the bottom repeat the last. */
    for (line = 0; line < stream->height; line += 16u) {
        uint32_t i;
        for (i = 0; i < 16u; ++i) {
            uint32_t y = line + i < stream->height ? line + i : stream->height - 1u;
            rows[0][i] = (JSAMPROW)(uintptr_t)(copy + (size_t)y * stream->stride[0]);
            if (i < 8u) {
                uint32_t c = line / 2u + i < stream->height / 2u ? line / 2u + i : stream->height / 2u - 1u;
                rows[1][i] = (JSAMPROW)(uintptr_t)(copy + stream->offset[1] + (size_t)c * stream->stride[1]);
                rows[2][i] = (JSAMPROW)(uintptr_t)(copy + stream->offset[2] + (size_t)c * stream->stride[2]);
            }
        }
        jpeg_write_raw_data(&cinfo, planes, 16);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return 0;
}

int32_t cam_jpeg_software(const cam_frame *frame, const cam_stream *stream, uint32_t quality, uint8_t **out,
                          uint32_t *out_bytes, cam_error *error)
{
    jpeg_target target;
    uint8_t *copy;
    int rc;
    *out = NULL;
    if (stream->format != KEEL_CAMERA_FORMAT_I420) {
        return cam_fail(error, KEEL_E_INTERNAL, KEEL_CAMERA_RESOURCE_JPEG, 0, NULL, "stills are I420");
    }
    /* A private copy: the frame's memory may be uncached, and libjpeg reads
     * each row several times. */
    copy = malloc(frame->bytes);
    if (copy == NULL) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, NULL, "out of memory");
    }
    cam_frame_copy(frame, copy);
    memset(&target, 0, sizeof(target));
    rc = compress(copy, stream, quality, &target);
    free(copy);
    if (rc != 0) {
        free(target.memory);
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_JPEG, 0, NULL, "libjpeg failed: %s",
                        target.message);
    }
    *out = target.memory;
    *out_bytes = (uint32_t)target.size;
    return KEEL_OK;
}

#else

int cam_jpeg_software_available(void)
{
    return 0;
}

int32_t cam_jpeg_software(const cam_frame *frame, const cam_stream *stream, uint32_t quality, uint8_t **out,
                          uint32_t *out_bytes, cam_error *error)
{
    (void)frame;
    (void)quality;
    (void)out_bytes;
    *out = NULL;
    return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_JPEG, 0, NULL,
                    "a %ux%u still exceeds the hardware JPEG encoder's 1920x1920, and this library was built "
                    "without libjpeg", stream->width, stream->height);
}

#endif
