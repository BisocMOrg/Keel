/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Handle registry (SPEC section 6). A handle is the token
 * (generation << 8) | (slot + 1) cast to keel_device *; the library never
 * dereferences it, so a closed, stale or forged handle can only ever yield
 * KEEL_E_INVALID_HANDLE. Each entry counts the calls in flight so that
 * keel_close() can wait for them before the device memory goes away.
 *
 * Validation is lock-free: every call checks the owner pid, then the entry's
 * tag (its generation and closing bit), takes a reference and checks the
 * tag again. A stop call therefore never queues behind other callers on a
 * shared mutex, which is not fair and let a thread polling in a tight loop
 * delay a disarm by milliseconds. The registry mutex remains for publishing
 * a device, for close's wait for calls in flight and around fork().
 *
 * The owner pid is checked atomically before anything else: a forked child
 * gets KEEL_E_NOT_OWNER without touching any lock it may have inherited.
 * The registry lock is never held while a device lock is taken.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#include "core/internal.h"
#include "os/os.h"

enum {
    SLOT_COUNT = 64,
    SLOT_BITS = 8,
    SLOT_MASK = (1 << SLOT_BITS) - 1,
    /* 24 generation bits keep the token inside 32 bits on every target. */
    GENERATION_MASK = 0xFFFFFF,
    /* Low bit of entry::tag: keel_close() has started. */
    TAG_CLOSING = 1
};

typedef struct entry {
    _Atomic int32_t owner_pid; /* 0 while the slot is free */
    _Atomic uint32_t tag;      /* generation << 1 | TAG_CLOSING; 0 while free */
    _Atomic uint32_t refs;     /* calls holding the entry, close included */
    uint32_t generation;       /* the slot's current generation (registry lock) */
    keel__device *device;      /* published before the tag, cleared after it */
} entry;

static entry entries[SLOT_COUNT];

static uint32_t next_generation(uint32_t generation)
{
    return (generation % GENERATION_MASK) + 1u;
}

int32_t keel__registry_add(keel__device *device, keel_device **out_handle)
{
    uint32_t slot;
    keel__os_init();
    keel__os_lock();
    for (slot = 0; slot < SLOT_COUNT; ++slot) {
        entry *e = &entries[slot];
        if (atomic_load(&e->owner_pid) == 0 && atomic_load(&e->tag) == 0) {
            uintptr_t token;
            if (e->generation == 0) {
                e->generation = 1;
            }
            e->device = device;
            /* The tag makes the entry valid; the device is visible first. */
            atomic_store(&e->tag, e->generation << 1);
            atomic_store(&e->owner_pid, keel__os_pid());
            token = ((uintptr_t)e->generation << SLOT_BITS) | (uintptr_t)(slot + 1u);
            *out_handle = (keel_device *)token;
            keel__os_unlock();
            return KEEL_OK;
        }
    }
    keel__os_unlock();
    return KEEL_E_NO_MEMORY;
}

/* Drops one reference; close's wait is woken while the entry is closing. */
static void drop(entry *e)
{
    atomic_fetch_sub(&e->refs, 1u);
    if (atomic_load(&e->tag) & TAG_CLOSING) {
        keel__os_lock();
        keel__os_wake();
        keel__os_unlock();
    }
}

int32_t keel__acquire(const keel_device *handle, keel__ref *ref, int for_close)
{
    uintptr_t token = (uintptr_t)(const void *)handle;
    uintptr_t index = token & SLOT_MASK;
    uint32_t open_tag;
    entry *e;
    int32_t owner;
    if (index == 0 || index > SLOT_COUNT) {
        return KEEL_E_INVALID_HANDLE;
    }
    e = &entries[index - 1u];
    owner = atomic_load(&e->owner_pid);
    if (owner != 0 && owner != keel__os_pid()) {
        return KEEL_E_NOT_OWNER;
    }
    open_tag = (uint32_t)((token >> SLOT_BITS) & GENERATION_MASK) << 1;
    if (open_tag == 0 || atomic_load(&e->tag) != open_tag) {
        return KEEL_E_INVALID_HANDLE;
    }
    /* The reference holds off close's retire; then the tag is checked again,
     * so a close or slot reuse that won the race is seen. */
    atomic_fetch_add(&e->refs, 1u);
    if (for_close) {
        uint32_t expected = open_tag;
        if (!atomic_compare_exchange_strong(&e->tag, &expected, open_tag | TAG_CLOSING)) {
            drop(e);
            return KEEL_E_INVALID_HANDLE; /* closed or closing already */
        }
    } else if (atomic_load(&e->tag) != open_tag) {
        drop(e);
        return KEEL_E_INVALID_HANDLE;
    }
    ref->device = e->device;
    ref->slot = (uint32_t)(index - 1u);
    return KEEL_OK;
}

void keel__release(const keel__ref *ref)
{
    drop(&entries[ref->slot]);
}

void keel__registry_retire(const keel__ref *ref)
{
    entry *e = &entries[ref->slot];
    keel__os_lock();
    /* Every call in flight is bounded: sleepers were woken by cancel_sleep
     * and CONTROL sections refuse to start once closing is set. A call that
     * lost the race to close holds its reference only until its second tag
     * check. */
    while (atomic_load(&e->refs) > 1u) {
        keel__os_wait();
    }
    atomic_store(&e->tag, 0u);
    e->device = NULL;
    e->generation = next_generation(e->generation);
    atomic_store(&e->owner_pid, 0);
    keel__os_unlock();
    drop(e); /* close's own reference */
}
