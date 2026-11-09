/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * Why libcamera lists no camera (SPEC section 26.6). libcamera skips media
 * devices it cannot open and cameras whose IPA module it cannot load, so an
 * empty list alone cannot tell "nothing connected" from "no access" or "IPA
 * missing". This looks at the device nodes itself:
 *   1. a /dev/media* node that cannot be opened: KEEL_E_PERMISSION;
 *   2. a camera receiver (unicam, rp1-cfe) with a sensor entity: the camera
 *      is connected; then a missing Raspberry Pi IPA module, an inaccessible
 *      /dev/video* node or CMA heap, or else libcamera itself is named;
 *   3. otherwise no camera is connected, or the boot configuration does not
 *      enable it (no sensor overlay): KEEL_OK.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "camera/camera_internal.h"

typedef struct scan {
    char sensor[KEEL_CAMERA_SENSOR_MAX]; /* first sensor entity found */
    char receiver[32];                   /* its receiver driver */
    char denied[KEEL_CAMERA_PATH_MAX];   /* first node that could not be opened */
    int denied_errno;
} scan;

static int is_receiver(const char *driver)
{
    return strcmp(driver, "unicam") == 0 || strcmp(driver, "rp1-cfe") == 0;
}

/* Records the first sensor of a receiver's media graph. */
static void scan_media(int fd, scan *result)
{
    struct media_device_info info;
    struct media_entity_desc entity;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) != 0 || !is_receiver(info.driver)) {
        return;
    }
    memset(&entity, 0, sizeof(entity));
    entity.id = MEDIA_ENT_ID_FLAG_NEXT;
    while (ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0) {
        if (entity.type == MEDIA_ENT_F_CAM_SENSOR && result->sensor[0] == '\0') {
            /* Entity names read "imx219 10-0010": keep the sensor name. */
            size_t length = strcspn(entity.name, " ");
            if (length >= sizeof(result->sensor)) {
                length = sizeof(result->sensor) - 1u;
            }
            memcpy(result->sensor, entity.name, length);
            result->sensor[length] = '\0';
            cam_copy_string(result->receiver, sizeof(result->receiver), info.driver);
        }
        entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
    }
}

/* Opens every node of dir whose name starts with prefix; scans media nodes. */
static void scan_nodes(const char *dir, const char *prefix, int media, scan *result)
{
    DIR *handle = opendir(dir);
    struct dirent *entry;
    if (handle == NULL) {
        return;
    }
    while ((entry = readdir(handle)) != NULL) {
        char path[KEEL_CAMERA_PATH_MAX];
        int fd;
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= sizeof(path)) {
            continue;
        }
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if ((errno == EACCES || errno == EPERM) && result->denied[0] == '\0') {
                cam_copy_string(result->denied, sizeof(result->denied), path);
                result->denied_errno = errno;
            }
            continue;
        }
        if (media) {
            scan_media(fd, result);
        }
        close(fd);
    }
    closedir(handle);
}

static int ipa_present(const char *ipa_dir)
{
    static const char *const modules[] = {"ipa_rpi_vc4.so", "ipa_rpi_pisp.so"};
    size_t i;
    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        char path[KEEL_CAMERA_PATH_MAX];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", ipa_dir, modules[i]) < sizeof(path) &&
            access(path, R_OK) == 0) {
            return 1;
        }
    }
    return 0;
}

int32_t cam_diagnose_no_camera(const char *dev_root, const char *ipa_dir, cam_error *error)
{
    scan result;
    char dir[KEEL_CAMERA_PATH_MAX];
    memset(&result, 0, sizeof(result));
    scan_nodes(dev_root, "media", 1, &result);
    if (result.denied[0] != '\0') {
        return cam_fail_permission(error, KEEL_CAMERA_RESOURCE_DEVICE_NODE, result.denied_errno, result.denied);
    }
    if (result.sensor[0] == '\0') {
        return KEEL_OK;
    }
    if (ipa_dir != NULL && !ipa_present(ipa_dir)) {
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_IPA, ENOENT, ipa_dir,
                        "the %s camera is connected, but libcamera's Raspberry Pi IPA module (ipa_rpi_vc4.so, "
                        "ipa_rpi_pisp.so) is not in %s: install it with libcamera (Debian and Ubuntu: sudo apt "
                        "install libcamera-ipa)", result.sensor, ipa_dir);
    }
    scan_nodes(dev_root, "video", 0, &result);
    /* The CMA heaps libcamera allocates from; the system heap is root-only
     * by design and unused. */
    snprintf(dir, sizeof(dir), "%s/dma_heap", dev_root);
    scan_nodes(dir, "linux,cma", 0, &result);
    scan_nodes(dir, "reserved", 0, &result);
    if (result.denied[0] != '\0') {
        return cam_fail_permission(error, KEEL_CAMERA_RESOURCE_DEVICE_NODE, result.denied_errno, result.denied);
    }
    return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_LIBCAMERA, 0, NULL,
                    "the %s camera is connected to %s, but libcamera did not register it: run with "
                    "LIBCAMERA_LOG_LEVELS=*:INFO to see why", result.sensor, result.receiver);
}
