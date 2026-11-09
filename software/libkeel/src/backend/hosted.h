/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file hosted.h
 * @brief Contract between the library and the backends built into it
 *        (keel_r3, the simulator): how keel_open() reaches a backend and how
 *        a backend attaches the core to storage inside its own allocation.
 *
 * A hosted backend allocates one block holding its own state and the core's
 * device storage (keel_port_storage_size/align), fills a backend table and
 * calls keel__attach(). Its @c destroy hook frees the whole block; the core
 * calls it last, once nothing touches the device any more.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#ifndef KEEL_BACKEND_HOSTED_H
#define KEEL_BACKEND_HOSTED_H

#include <stddef.h>
#include <stdint.h>

#include "keel/port.h"

/**
 * @brief Attaches the core to @p storage; the rules of keel_port_attach().
 * @param[in]  storage      Device storage inside the backend's allocation.
 * @param[in]  storage_size Size of @p storage.
 * @param[in]  backend      Backend table; its @c destroy hook is called on failure.
 * @param[in]  options      Full-size options that passed keel__options_check().
 * @param[in]  backend_kind ::keel_backend reported for the device.
 * @param[out] out_device   Receives the handle; NULL on failure.
 * @param[out] report       Full-size report; status is set by the caller.
 * @return KEEL_OK, KEEL_E_INVALID_ARGUMENT or a SETUP-class status.
 */
int32_t keel__attach(void *storage, size_t storage_size, const keel_port_backend *backend,
                     const keel_open_options *options, uint32_t backend_kind,
                     keel_device **out_device, keel_open_report *report);

/**
 * @brief Records a failure or degradation in a full-size open report.
 * @param[out] report   Report to update.
 * @param[in]  resource ::keel_resource involved.
 * @param[in]  os_error errno of the failing operation, or 0.
 * @param[in]  path     Device path involved, or NULL.
 * @param[in]  hint     One-line remedy (truncated to fit).
 */
void keel__report_fail(keel_open_report *report, uint32_t resource, int32_t os_error,
                       const char *path, const char *hint);

/**
 * @brief Opens a simulator device (src/backend/sim).
 * @param[in]  options    Full-size, validated options (backend SIMULATOR).
 * @param[out] out_device Receives the handle.
 * @param[out] report     Full-size report.
 * @return As keel__attach(), or KEEL_E_NO_MEMORY / KEEL_E_SETUP_IO.
 */
int32_t keel__sim_open(const keel_open_options *options, keel_device **out_device,
                       keel_open_report *report);

/**
 * @brief Opens the keel_r3 backend and attaches the core (src/backend/keel_r3,
 *        SPEC 17).
 * @param[in]  options    Full-size, validated options (backend R3).
 * @param[out] out_device Receives the handle; NULL on failure.
 * @param[out] report     Full-size report.
 * @return As keel__attach(), or a SETUP-class status naming the resource.
 */
int32_t keel__r3_open(const keel_open_options *options, keel_device **out_device,
                      keel_open_report *report);

#endif /* KEEL_BACKEND_HOSTED_H */
