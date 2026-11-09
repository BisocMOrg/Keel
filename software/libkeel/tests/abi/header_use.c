/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Compile-only consumer of both public headers. tests/abi/check_headers.sh
 * builds it as C99, C11, C++11 and C++17 with the strictest warning set, so
 * the headers stay usable the way applications and ports write code:
 * enumerators compared with and stored into uint32_t/int32_t without casts,
 * struct_size initialisation, every function type and a backend table
 * filled with hooks.
 */
#include <stddef.h>
#include <string.h>

#include "keel/keel.h"
#include "keel/port.h"

int keel_header_use_loop(keel_device *device, uint64_t cycle_id);
int keel_header_use_port(void *storage, size_t size);

/* One loop iteration in the shape a binding's control loop takes. */
int keel_header_use_loop(keel_device *device, uint64_t cycle_id) {
    keel_cycle_input in;
    keel_snapshot snapshot;
    keel_fault fault;
    int32_t status;

    memset(&in, 0, sizeof in);
    in.struct_size = (uint32_t)sizeof in;
    in.health = KEEL_HEALTH_ALL;
    in.cycle_id = cycle_id;
    in.command = KEEL_CYCLE_DRIVE;
    in.left = KEEL_DUTY_MAX / 5;
    in.right = -(KEEL_DUTY_MAX / 5);
    in.flags = KEEL_CYCLE_TAKE_BUTTONS;
    snapshot.struct_size = (uint32_t)sizeof snapshot;
    fault.struct_size = (uint32_t)sizeof fault;

    status = keel_cycle(device, &in, &snapshot);
    switch (keel_status_class(status)) {
    case KEEL_CLASS_OK:
        return snapshot.state == KEEL_STATE_ARMED ? 1 : 0;
    case KEEL_CLASS_FAULT:
        (void)keel_get_fault(device, &fault);
        return (int)fault.cause;
    case KEEL_CLASS_HANDLE:
        return -1;
    default:
        return keel_status_name(status)[0] == 'K' ? 0 : -1;
    }
}

static uint64_t fake_now(void *user) {
    (void)user;
    return 0;
}

static int32_t fake_motors(void *user, const keel_port_motor_command *command) {
    (void)user;
    return command->mode == KEEL_MOTOR_COAST ? KEEL_OK : KEEL_E_IO;
}

/* A backend table filled the way a port fills it. */
int keel_header_use_port(void *storage, size_t size) {
    keel_port_backend backend;
    keel_device *device = NULL;

    memset(&backend, 0, sizeof backend);
    backend.struct_size = (uint32_t)sizeof backend;
    backend.port_version = KEEL_PORT_VERSION;
    backend.capabilities = KEEL_CAP_MOTORS | KEEL_CAP_BRAKE;
    backend.flags = KEEL_PORT_SINGLE_THREADED;
    backend.now_us = fake_now;
    backend.write_motors = fake_motors;
    if (size < keel_port_storage_size()) {
        return -1;
    }
    return (int)keel_port_attach(storage, size, &backend, NULL, &device, NULL);
}
