/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * A self-contained program linked through keel-static.pc: it uses the port
 * API, which only the static archive provides, and then drives the
 * simulator through keel.h. Linking at all proves the archive was chosen;
 * check_consumers.sh also checks that the program needs no libkeel.so.
 */
#include <keel/keel.h>
#include <keel/port.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    keel_open_options options;
    keel_device *device = NULL;
    size_t size = keel_port_storage_size();
    size_t align = keel_port_storage_align();
    int32_t status;

    if (size == 0 || align == 0 || (align & (align - 1u)) != 0) {
        fprintf(stderr, "port_consumer: storage %zu bytes, alignment %zu\n", size, align);
        return 1;
    }
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_SIMULATOR;
    options.flags = KEEL_OPEN_VIRTUAL_CLOCK;
    status = keel_open(&options, &device, NULL);
    if (status != KEEL_OK) {
        fprintf(stderr, "port_consumer: keel_open: %s\n", keel_status_name(status));
        return 1;
    }
    status = keel_tick(device, 1, KEEL_HEALTH_ALL);
    if (status == KEEL_OK) {
        status = keel_close(device, NULL);
    } else {
        (void)keel_close(device, NULL);
    }
    if (status != KEEL_OK) {
        fprintf(stderr, "port_consumer: %s\n", keel_status_name(status));
        return 1;
    }
    printf("port_consumer: libkeel %s, port storage %zu bytes\n", keel_version_string(), size);
    return 0;
}
