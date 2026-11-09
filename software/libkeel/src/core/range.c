/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Ultrasonic ranging on J1 (SPEC section 15), with RANGE held throughout.
 *
 * An HC-SR04 answers a 10 us trigger with an echo pulse whose width is the
 * round trip of the sound. Results are statuses, never sentinel distances:
 * the classic "very far" reading of an unpowered or unplugged sensor would
 * read as a clear path, so every way a measurement can fail has its own
 * status. Timing comes from the backend's ECHO edge timestamps, not from
 * when this code happens to run.
 *
 * RANGE never takes CONTROL. The two facts it needs from the safety side
 * travel through atomics: READY drops (a measurement spanning one is
 * INVALID) and, in the other direction, the latest echo rise (sensor power
 * CONFIRMED in the snapshot).
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include <string.h>

#include "core/internal.h"

static uint64_t backend_now(const keel__device *dev)
{
    return dev->be.now_us(dev->be.user);
}

/* Starts a new result: every measurement or immediate answer gets a new
 * sequence number, so a poller can tell results apart. */
static void new_result(keel__device *dev, uint32_t status, uint64_t at)
{
    keel_range *result = &dev->range.result;
    uint32_t sequence = result->sequence + 1u;
    memset(result, 0, sizeof *result);
    result->struct_size = sizeof *result;
    result->status = status;
    result->measured_at_us = at;
    result->sequence = sequence;
}

/* Completes the pending measurement. Only VALID carries a distance: 343 m/s
 * round trip, 0.1715 mm per us, rounded to the nearest millimetre. */
static void finish(keel__device *dev, uint32_t status, uint32_t echo_us)
{
    keel_range *result = &dev->range.result;
    dev->range.pending = 0;
    result->status = status;
    result->echo_us = echo_us;
    result->distance_mm =
        status == KEEL_RANGE_VALID ? (uint32_t)((echo_us * UINT64_C(343) + 1000u) / 2000u) : 0u;
}

static void on_edge(keel__device *dev, const keel_port_edge *edge)
{
    keel__range *r = &dev->range;
    uint64_t width;
    if (edge->rising) {
        /* A second rise means an edge was lost; a rise before the trigger
         * means ECHO was already HIGH. Neither can be timed. */
        if (r->risen || edge->time_us < r->trigger_us) {
            finish(dev, KEEL_RANGE_INVALID, 0);
            return;
        }
        r->risen = 1;
        r->rise_us = edge->time_us;
        atomic_store(&dev->echo_rise_us, edge->time_us);
        atomic_store(&dev->echo_seen, 1);
        return;
    }
    if (!r->risen) {
        finish(dev, KEEL_RANGE_INVALID, 0); /* the matching rise was never seen */
        return;
    }
    width = edge->time_us >= r->rise_us ? edge->time_us - r->rise_us : 0u;
    if (width > KEEL__ECHO_MAX_US) {
        finish(dev, KEEL_RANGE_NO_ECHO, width > UINT32_MAX ? UINT32_MAX : (uint32_t)width);
    } else {
        finish(dev, width < KEEL__ECHO_MIN_US ? KEEL_RANGE_INVALID : KEEL_RANGE_VALID,
               (uint32_t)width);
    }
}

/* Drains up to one batch of edges into @p edges; non-zero on failure. */
static int drain(keel__device *dev, keel_port_edge *edges, uint32_t *count)
{
    *count = 0;
    return dev->be.range_edges(dev->be.user, edges, KEEL__RANGE_EDGE_BATCH, count) != KEEL_OK ||
           *count > KEEL__RANGE_EDGE_BATCH;
}

void keel__range_init(keel__device *dev)
{
    memset(&dev->range, 0, sizeof dev->range);
    dev->range.result.struct_size = sizeof dev->range.result;
}

int32_t keel__range_update(keel__device *dev)
{
    keel__range *r = &dev->range;
    keel_port_edge edges[KEEL__RANGE_EDGE_BATCH];
    uint32_t count, i;
    uint64_t elapsed;

    if (!r->pending) {
        return KEEL_OK;
    }
    if (atomic_load(&dev->ready_drops) != r->ready_drops) {
        /* The MCU dropped READY (and with it sensor power) mid-measurement. */
        finish(dev, KEEL_RANGE_INVALID, 0);
        return KEEL_OK;
    }
    do {
        if (drain(dev, edges, &count)) {
            finish(dev, KEEL_RANGE_INVALID, 0); /* edge events were lost */
            return KEEL_E_PERIPHERAL;
        }
        for (i = 0; i < count && r->pending; ++i) {
            on_edge(dev, &edges[i]);
        }
    } while (count == KEEL__RANGE_EDGE_BATCH && r->pending);
    if (!r->pending) {
        return KEEL_OK;
    }
    /* Edges are processed before the clock, so a late poll still sees an
     * echo that arrived in time. */
    elapsed = backend_now(dev) - r->trigger_us;
    if (!r->risen && elapsed > KEEL__MS(KEEL__ECHO_RISE_MS)) {
        finish(dev, KEEL_RANGE_UNAVAILABLE, 0);
    } else if (r->risen && elapsed >= KEEL__MS(KEEL_RANGE_TIMEOUT_MS)) {
        finish(dev, KEEL_RANGE_NO_ECHO, 0); /* still HIGH: nothing within 4 m */
    }
    return KEEL_OK;
}

int32_t keel__range_start(keel__device *dev, uint32_t power)
{
    keel__range *r = &dev->range;
    keel_port_edge edges[KEEL__RANGE_EDGE_BATCH];
    uint32_t count, echo_high = 0;
    uint64_t now;
    int32_t update_rc = keel__range_update(dev);

    now = backend_now(dev);
    if (power != KEEL_SENSOR_PERMITTED && power != KEEL_SENSOR_CONFIRMED) {
        /* Never drive TRIG into an unpowered sensor: answer at once. */
        r->pending = 0;
        new_result(dev, KEEL_RANGE_UNAVAILABLE, now);
        return KEEL_OK;
    }
    if (update_rc != KEEL_OK) {
        return update_rc;
    }
    if (r->pending ||
        (r->have_trigger && now - r->last_trigger_us < KEEL__MS(KEEL_RANGE_INTERVAL_MS))) {
        return keel__reject(dev, KEEL_E_AGAIN);
    }
    /* Edges left over from an abandoned or timed-out echo are stale. */
    do {
        if (drain(dev, edges, &count)) {
            return KEEL_E_PERIPHERAL;
        }
    } while (count == KEEL__RANGE_EDGE_BATCH);
    now = backend_now(dev);
    if (dev->be.range_trigger(dev->be.user, &echo_high) != KEEL_OK) {
        return KEEL_E_PERIPHERAL;
    }
    if (echo_high) {
        return keel__reject(dev, KEEL_E_AGAIN); /* the previous echo has not ended */
    }
    r->have_trigger = 1;
    r->last_trigger_us = now;
    r->trigger_us = now;
    r->risen = 0;
    r->pending = 1;
    r->ready_drops = atomic_load(&dev->ready_drops);
    new_result(dev, KEEL_RANGE_PENDING, now);
    return KEEL_OK;
}

void keel__range_result(const keel__device *dev, keel_range *out)
{
    *out = dev->range.result;
}

void keel__range_expire(keel__device *dev)
{
    keel__range *r = &dev->range;
    if (r->pending) {
        finish(dev, r->risen ? KEEL_RANGE_NO_ECHO : KEEL_RANGE_UNAVAILABLE, 0);
    }
}
