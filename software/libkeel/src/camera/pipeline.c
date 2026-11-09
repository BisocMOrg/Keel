/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Frames, packets and their bounded queues (SPEC section 26.4).
 *
 * A frame's last reference returns its buffer to the capture source. A
 * packet's last reference returns it to its pool, which keeps a few idle
 * packets with their memory, so steady capture allocates nothing per frame.
 * Queues never grow: H.264 queues drop everything queued when full and
 * resume at the next keyframe (a reader always gets a decodable stream);
 * MJPEG and frame queues drop the oldest item.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <linux/dma-buf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "camera/camera_internal.h"

enum {
    POOL_IDLE_MAX = 16 /* idle packets kept with their memory */
};

/* ---- frames ------------------------------------------------------------- */

void cam_frame_deliver(cam_frame *frame)
{
    atomic_store(&frame->refs, 1u);
}

void cam_frame_ref(cam_frame *frame)
{
    atomic_fetch_add(&frame->refs, 1u);
}

void cam_frame_unref(cam_frame *frame)
{
    if (atomic_fetch_sub(&frame->refs, 1u) == 1u) {
        frame->source->ops->release(frame->source, frame);
    }
}

static void dma_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync;
    if (fd < 0) {
        return;
    }
    sync.flags = flags;
    /* Best effort: buffers that are not CPU-cached do not need it. */
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

void cam_frame_copy(const cam_frame *frame, uint8_t *dst)
{
    dma_sync(frame->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    memcpy(dst, frame->data, frame->bytes);
    dma_sync(frame->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

/* Copies rows lines of row_bytes into dst_rows lines; the extra lines repeat
 * the last one. */
static void copy_plane(const uint8_t *src, uint32_t src_stride, uint32_t rows, uint8_t *dst, uint32_t dst_stride,
                       uint32_t dst_rows, uint32_t row_bytes)
{
    uint32_t y;
    for (y = 0; y < dst_rows; ++y) {
        uint32_t from = y < rows ? y : rows - 1u;
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)from * src_stride, row_bytes);
    }
}

uint32_t cam_coded_frame_bytes(uint32_t format, uint32_t stride, uint32_t height)
{
    if (format == KEEL_CAMERA_FORMAT_I420) {
        return stride * height + 2u * (stride / 2u) * (height / 2u);
    }
    return stride * height;
}

void cam_frame_copy_coded(const cam_frame *frame, uint32_t format, uint32_t stride, uint32_t height, uint8_t *dst,
                          uint32_t dst_stride, uint32_t dst_height)
{
    uint32_t row_bytes = stride < dst_stride ? stride : dst_stride;
    const uint8_t *src = frame->data;
    dma_sync(frame->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    copy_plane(src, stride, height, dst, dst_stride, dst_height, row_bytes);
    if (format == KEEL_CAMERA_FORMAT_I420) {
        uint32_t src_chroma = (stride / 2u) * (height / 2u);
        uint32_t dst_chroma = (dst_stride / 2u) * (dst_height / 2u);
        const uint8_t *src_u = src + (size_t)stride * height;
        uint8_t *dst_u = dst + (size_t)dst_stride * dst_height;
        copy_plane(src_u, stride / 2u, height / 2u, dst_u, dst_stride / 2u, dst_height / 2u, row_bytes / 2u);
        copy_plane(src_u + src_chroma, stride / 2u, height / 2u, dst_u + dst_chroma, dst_stride / 2u,
                   dst_height / 2u, row_bytes / 2u);
    }
    dma_sync(frame->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

cam_frame *cam_frame_queue_push(cam_frame_queue *queue, cam_frame *frame)
{
    cam_frame *evicted = NULL;
    if (queue->count == queue->capacity) {
        evicted = cam_frame_queue_pop(queue);
    }
    cam_frame_ref(frame);
    queue->items[(queue->head + queue->count) % queue->capacity] = frame;
    queue->count++;
    return evicted;
}

cam_frame *cam_frame_queue_pop(cam_frame_queue *queue)
{
    cam_frame *frame;
    if (queue->count == 0) {
        return NULL;
    }
    frame = queue->items[queue->head];
    queue->head = (queue->head + 1u) % queue->capacity;
    queue->count--;
    return frame;
}

/* ---- packets ------------------------------------------------------------ */

void cam_packet_pool_init(cam_packet_pool *pool)
{
    memset(pool, 0, sizeof(*pool));
    pthread_mutex_init(&pool->lock, NULL);
}

cam_packet *cam_packet_get(cam_packet_pool *pool, uint32_t bytes)
{
    cam_packet *packet;
    pthread_mutex_lock(&pool->lock);
    packet = pool->free_list;
    if (packet != NULL) {
        pool->free_list = packet->next_free;
        pool->free_count--;
    }
    pthread_mutex_unlock(&pool->lock);
    if (packet == NULL) {
        packet = calloc(1, sizeof(*packet));
        if (packet == NULL) {
            return NULL;
        }
        packet->pool = pool;
    }
    if (packet->capacity < bytes) {
        /* Grow in 64 KiB steps so sizes that wobble do not reallocate. */
        uint32_t capacity = (bytes + 65535u) & ~65535u;
        uint8_t *data = realloc(packet->data, capacity);
        if (data == NULL) {
            free(packet->data);
            free(packet);
            return NULL;
        }
        packet->data = data;
        packet->capacity = capacity;
    }
    atomic_store(&packet->refs, 1u);
    packet->flags = 0;
    packet->bytes = 0;
    packet->header_bytes = 0;
    packet->next_free = NULL;
    atomic_fetch_add(&pool->live, 1u);
    return packet;
}

void cam_packet_ref(cam_packet *packet)
{
    atomic_fetch_add(&packet->refs, 1u);
}

static void packet_free(cam_packet *packet)
{
    free(packet->data);
    free(packet);
}

void cam_packet_unref(cam_packet *packet)
{
    cam_packet_pool *pool;
    int keep;
    if (atomic_fetch_sub(&packet->refs, 1u) != 1u) {
        return;
    }
    pool = packet->pool;
    pthread_mutex_lock(&pool->lock);
    keep = !pool->closed && pool->free_count < POOL_IDLE_MAX;
    if (keep) {
        packet->next_free = pool->free_list;
        pool->free_list = packet;
        pool->free_count++;
    }
    atomic_fetch_sub(&pool->live, 1u);
    pthread_mutex_unlock(&pool->lock);
    if (!keep) {
        packet_free(packet);
    }
}

void cam_packet_pool_close(cam_packet_pool *pool)
{
    cam_packet *packet;
    pthread_mutex_lock(&pool->lock);
    pool->closed = 1;
    packet = pool->free_list;
    pool->free_list = NULL;
    pool->free_count = 0;
    pthread_mutex_unlock(&pool->lock);
    while (packet != NULL) {
        cam_packet *next = packet->next_free;
        packet_free(packet);
        packet = next;
    }
}

uint32_t cam_h264_scan(const uint8_t *data, uint32_t bytes, uint32_t *header_bytes)
{
    uint32_t flags = 0;
    uint32_t i = 0;
    int leading = 1; /* still inside the SPS/PPS prefix */
    *header_bytes = 0;
    while (i + 3u < bytes) {
        uint32_t start;
        uint32_t type;
        if (data[i] != 0 || data[i + 1u] != 0 || data[i + 2u] != 1) {
            ++i;
            continue;
        }
        /* A zero before 00 00 01 makes a four-byte start code. */
        start = (i > 0 && data[i - 1u] == 0) ? i - 1u : i;
        type = data[i + 3u] & 0x1Fu;
        if (type == 7u || type == 8u) {
            flags |= KEEL_CAMERA_ENCODED_HEADERS;
        } else if (leading) {
            leading = 0;
            if (flags & KEEL_CAMERA_ENCODED_HEADERS) {
                *header_bytes = start;
            }
        }
        if (type == 5u) {
            flags |= KEEL_CAMERA_ENCODED_KEYFRAME;
        }
        i += 3u;
    }
    return flags;
}

/* ---- packet queues ------------------------------------------------------ */

int cam_packet_queue_init(cam_packet_queue *queue, uint32_t capacity, uint32_t codec)
{
    memset(queue, 0, sizeof(*queue));
    queue->items = calloc(capacity, sizeof(*queue->items));
    if (queue->items == NULL) {
        return -1;
    }
    queue->capacity = capacity;
    queue->codec = codec;
    /* An H.264 reader starts at a keyframe. */
    queue->need_keyframe = codec == KEEL_CAMERA_CODEC_H264;
    return 0;
}

cam_packet *cam_packet_queue_pop(cam_packet_queue *queue)
{
    cam_packet *packet;
    if (queue->count == 0) {
        return NULL;
    }
    packet = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1u) % queue->capacity;
    queue->count--;
    return packet;
}

cam_packet *cam_packet_queue_peek(const cam_packet_queue *queue)
{
    return queue->count == 0 ? NULL : queue->items[queue->head];
}

void cam_packet_queue_clear(cam_packet_queue *queue)
{
    cam_packet *packet;
    while ((packet = cam_packet_queue_pop(queue)) != NULL) {
        cam_packet_unref(packet);
    }
}

int cam_packet_queue_push(cam_packet_queue *queue, cam_packet *packet)
{
    int keyframe = (packet->flags & KEEL_CAMERA_ENCODED_KEYFRAME) != 0;
    if (queue->codec == KEEL_CAMERA_CODEC_H264) {
        if (queue->need_keyframe) {
            if (!keyframe) {
                queue->dropped++;
                return 0;
            }
            queue->need_keyframe = 0;
        }
        if (queue->count == queue->capacity) {
            /* Drop the whole backlog: the next picture the reader gets must
             * decode on its own. */
            queue->dropped += queue->count;
            cam_packet_queue_clear(queue);
            if (!keyframe) {
                queue->need_keyframe = 1;
                queue->dropped++;
                return 0;
            }
        }
    } else if (queue->count == queue->capacity) {
        cam_packet_unref(cam_packet_queue_pop(queue));
        queue->dropped++;
    }
    cam_packet_ref(packet);
    queue->items[(queue->head + queue->count) % queue->capacity] = packet;
    queue->count++;
    return 1;
}

void cam_packet_queue_free(cam_packet_queue *queue)
{
    if (queue->items != NULL) {
        cam_packet_queue_clear(queue);
        free(queue->items);
        queue->items = NULL;
    }
}
