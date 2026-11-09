/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel_r3 GPIO (SPEC 17.1 to 17.3): chip discovery by label, the line
 * preflight, the four exclusive line requests and the control and input
 * hooks.
 *
 * pinctrl-bcm2835 is not strict, so requesting a line that SPI0 or an
 * overlay has muxed would silently take it over. Every line is therefore
 * checked with GPIO_V2_GET_LINEINFO before anything is requested, and a line
 * flagged USED stops the open with the kernel's name for its owner.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "backend/keel_r3/r3.h"

/* The Pi 4, Pi 400 and CM4 bank; the Pi 5 (pinctrl-rp1) and expanders differ. */
static const char chip_label[] = "pinctrl-bcm2711";
/* The link 99-keel.rules creates (SYMLINK+="keel/gpiochip"), tried after the scan. */
static const char chip_link[] = "/dev/keel/gpiochip";
static const char consumer_name[] = "keel";

static const uint32_t control_lines[KEEL__R3_CONTROL_LINES] = {
    KEEL__R3_BCM_ENABLE, KEEL__R3_BCM_HEARTBEAT, KEEL__R3_BCM_SERVO_REQUEST, KEEL__R3_BCM_LED_A,
    KEEL__R3_BCM_LED_B,  KEEL__R3_BCM_LED_X,     KEEL__R3_BCM_LED_Y};
static const uint32_t motor_lines[KEEL__R3_MOTOR_LINES] = {KEEL__R3_BCM_LP, KEEL__R3_BCM_LN,
                                                           KEEL__R3_BCM_RP, KEEL__R3_BCM_RN};
static const uint32_t input_lines[KEEL__R3_INPUT_LINES] = {
    KEEL__R3_BCM_READY,    KEEL__R3_BCM_ARMED,    KEEL__R3_BCM_BUTTON_A,
    KEEL__R3_BCM_BUTTON_B, KEEL__R3_BCM_BUTTON_X, KEEL__R3_BCM_BUTTON_Y};
static const uint32_t range_lines[KEEL__R3_RANGE_LINES] = {KEEL__R3_BCM_TRIG, KEEL__R3_BCM_ECHO};

enum chip_outcome { CHIP_MATCH, CHIP_OTHER, CHIP_FAILED };

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t length = strlen(src);
    if (length >= size) {
        length = size - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static int last_error(void)
{
    return errno ? errno : EIO;
}

/* ---- discovery ------------------------------------------------------------ */

/* Keeps @p path open in b->chip_fd when it is the BCM2711 bank; otherwise
 * reports its label (CHIP_OTHER) or the errno of the failure. */
static int probe_chip(keel__r3 *b, const char *path, char *label, size_t label_size, int *error)
{
    struct gpiochip_info info;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        *error = last_error();
        return CHIP_FAILED;
    }
    memset(&info, 0, sizeof info);
    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
        *error = last_error();
        (void)close(fd);
        return CHIP_FAILED;
    }
    info.label[sizeof info.label - 1] = '\0';
    copy_text(label, label_size, info.label);
    if (strcmp(info.label, chip_label) != 0 || info.lines < KEEL__R3_CHIP_LINES_MIN) {
        (void)close(fd);
        return CHIP_OTHER;
    }
    b->chip_fd = fd;
    return CHIP_MATCH;
}

static int32_t wrong_chip(keel_open_report *report, const char *path, const char *label)
{
    char hint[KEEL_HINT_MAX];
    snprintf(hint, sizeof hint,
             "%s is \"%s\", not the Raspberry Pi 4 GPIO chip (%s): keel_r3 needs a Pi 4, Pi 400 or "
             "CM4",
             path, label, chip_label);
    keel__r3_report(report, KEEL_RESOURCE_GPIO_CHIP, 0, path, -1, NULL, hint);
    return KEEL_E_NOT_FOUND;
}

static int32_t chip_error(keel_open_report *report, const char *path, int error)
{
    /* ENOTTY: the node exists but is not a GPIO character device. */
    int32_t status = error == ENOTTY ? KEEL_E_NOT_FOUND : keel__r3_open_status(error);
    const char *hint;
    switch (status) {
    case KEEL_E_PERMISSION:
        hint = "no permission for the GPIO chip: add the user to the gpio (or keel) group with "
               "sudo keel-setup --user NAME, then log in again";
        break;
    case KEEL_E_NOT_FOUND:
        hint = "no Raspberry Pi 4 GPIO chip (pinctrl-bcm2711): keel_r3 needs a Pi 4, Pi 400 or "
               "CM4";
        break;
    case KEEL_E_IN_USE:
        hint = "the GPIO chip is busy; run keel-doctor";
        break;
    case KEEL_E_NO_MEMORY:
        hint = "out of memory while opening the GPIO chip";
        break;
    default:
        hint = "opening the GPIO chip failed; run keel-doctor";
        break;
    }
    keel__r3_report(report, KEEL_RESOURCE_GPIO_CHIP, error, path, -1, NULL, hint);
    return status;
}

/* The first /dev/gpiochipN labelled pinctrl-bcm2711, then the udev link; an
 * explicit path must carry the label as well. @p path receives the chip. */
static int32_t find_chip(keel__r3 *b, const keel_open_options *options, char *path,
                         keel_open_report *report)
{
    char label[GPIO_MAX_NAME_SIZE];
    char other_label[GPIO_MAX_NAME_SIZE] = "";
    char other_path[KEEL_PATH_MAX] = "";
    char denied_path[KEEL_PATH_MAX] = "";
    int error = 0, denied = 0, outcome;
    unsigned n;

    if (options->gpiochip_path[0]) {
        copy_text(path, KEEL_PATH_MAX, options->gpiochip_path);
        outcome = probe_chip(b, path, label, sizeof label, &error);
        if (outcome == CHIP_MATCH) {
            return KEEL_OK;
        }
        return outcome == CHIP_OTHER ? wrong_chip(report, path, label)
                                     : chip_error(report, path, error);
    }
    for (n = 0; n <= KEEL__R3_CHIP_SCAN; ++n) {
        if (n < KEEL__R3_CHIP_SCAN) {
            snprintf(path, KEEL_PATH_MAX, "/dev/gpiochip%u", n);
        } else {
            copy_text(path, KEEL_PATH_MAX, chip_link);
        }
        outcome = probe_chip(b, path, label, sizeof label, &error);
        if (outcome == CHIP_MATCH) {
            return KEEL_OK;
        }
        if (outcome == CHIP_OTHER && !other_label[0]) {
            copy_text(other_label, sizeof other_label, label);
            copy_text(other_path, sizeof other_path, path);
        } else if (outcome == CHIP_FAILED && (error == EACCES || error == EPERM) && !denied) {
            denied = error;
            copy_text(denied_path, sizeof denied_path, path);
        }
    }
    /* Most useful first: a chip we may not open, then a foreign chip. */
    if (denied) {
        copy_text(path, KEEL_PATH_MAX, denied_path);
        return chip_error(report, path, denied);
    }
    if (other_label[0]) {
        copy_text(path, KEEL_PATH_MAX, other_path);
        return wrong_chip(report, path, other_label);
    }
    keel__r3_report(report, KEEL_RESOURCE_GPIO_CHIP, ENOENT, "/dev/gpiochip0", -1, NULL,
                    "no GPIO character device: keel_r3 needs a Raspberry Pi 4, Pi 400 or CM4 "
                    "running Linux 5.10 or later");
    return KEEL_E_NOT_FOUND;
}

/* ---- preflight and requests ----------------------------------------------- */

static int line_info(const keel__r3 *b, uint32_t line, struct gpio_v2_line_info *info)
{
    memset(info, 0, sizeof *info);
    info->offset = line;
    if (ioctl(b->chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, info) < 0) {
        return last_error();
    }
    info->consumer[sizeof info->consumer - 1] = '\0';
    return 0;
}

static int32_t report_busy(keel_open_report *report, const char *path, uint32_t line,
                           const char *consumer)
{
    char hint[KEEL_HINT_MAX];
    int spi_pin = line >= KEEL__R3_BCM_LP && line <= KEEL__R3_BCM_LN;
    if (strcmp(consumer, consumer_name) == 0) {
        snprintf(hint, sizeof hint,
                 "BCM%u is held by another keel program; only one program may open the robot",
                 line);
    } else if (spi_pin && (!consumer[0] || strstr(consumer, "spi"))) {
        /* Muxed SPI0 pins are USED with an empty consumer; CE0 says "spi0 CS0". */
        snprintf(hint, sizeof hint,
                 "BCM%u is used by SPI0: set dtparam=spi=off in /boot/firmware/config.txt "
                 "(sudo keel-setup) and reboot",
                 line);
    } else if (!consumer[0]) {
        snprintf(hint, sizeof hint,
                 "BCM%u is claimed by a kernel function; remove the overlay that uses it (run "
                 "keel-doctor)",
                 line);
    } else {
        snprintf(hint, sizeof hint,
                 "BCM%u is used by \"%s\": stop that program or remove the overlay that claims it "
                 "(run keel-doctor)",
                 line, consumer);
    }
    keel__r3_report(report, KEEL_RESOURCE_GPIO_LINE, EBUSY, path, (int32_t)line, consumer, hint);
    return KEEL_E_IN_USE;
}

static int32_t no_gpio_v2(keel_open_report *report, const char *path, int error)
{
    keel__r3_report(report, KEEL_RESOURCE_GPIO_CHIP, error, path, -1, NULL,
                    "this kernel has no GPIO v2 character-device interface: Linux 5.10 or later is "
                    "required");
    return KEEL_E_NOT_FOUND;
}

static int32_t line_error(keel_open_report *report, const char *path, int error, uint32_t line)
{
    char hint[KEEL_HINT_MAX];
    int32_t status = keel__r3_open_status(error);
    snprintf(hint, sizeof hint, "checking or requesting BCM%u failed (%s); run keel-doctor", line,
             strerror(error));
    keel__r3_report(report, KEEL_RESOURCE_GPIO_LINE, error, path, (int32_t)line, NULL, hint);
    return status;
}

/* Every line to be requested must be unused before any is requested. */
static int32_t check_lines(const keel__r3 *b, const uint32_t *lines, unsigned count,
                           const char *path, keel_open_report *report)
{
    struct gpio_v2_line_info info;
    unsigned i;
    for (i = 0; i < count; ++i) {
        int error = line_info(b, lines[i], &info);
        if (error == ENOTTY || error == EINVAL) {
            return no_gpio_v2(report, path, error);
        }
        if (error) {
            return line_error(report, path, error, lines[i]);
        }
        if (info.flags & GPIO_V2_LINE_FLAG_USED) {
            return report_busy(report, path, lines[i], info.consumer);
        }
    }
    return KEEL_OK;
}

static void request_init(struct gpio_v2_line_request *request, const uint32_t *lines,
                         unsigned count)
{
    memset(request, 0, sizeof *request);
    memcpy(request->offsets, lines, count * sizeof *lines);
    request->num_lines = count;
    copy_text(request->consumer, sizeof request->consumer, consumer_name);
}

/* Outputs come up LOW from the request itself, without a glitch. */
static void request_outputs(struct gpio_v2_line_request *request, const uint32_t *lines,
                            unsigned count)
{
    request_init(request, lines, count);
    request->config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    request->config.num_attrs = 1;
    request->config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request->config.attrs[0].attr.values = 0;
    request->config.attrs[0].mask = (UINT64_C(1) << count) - 1u;
}

/* READY and ARMED have board pull-downs, so no bias; the buttons pull up and
 * read active-low, so a pressed button reads 1. */
static void request_inputs(struct gpio_v2_line_request *request)
{
    request_init(request, input_lines, KEEL__R3_INPUT_LINES);
    request->config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    request->config.num_attrs = 1;
    request->config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_FLAGS;
    request->config.attrs[0].attr.flags =
        GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP | GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    request->config.attrs[0].mask = (uint64_t)0xF << KEEL__R3_IN_BUTTON_SHIFT;
}

/* TRIG starts LOW; ECHO reports both edges with kernel CLOCK_MONOTONIC
 * timestamps, so pulse widths do not depend on when userspace runs. The
 * pull-down keeps an unplugged sensor's ECHO LOW (UNAVAILABLE, not noise). */
static void request_range(struct gpio_v2_line_request *request)
{
    request_init(request, range_lines, KEEL__R3_RANGE_LINES);
    request->config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    request->config.num_attrs = 2;
    request->config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request->config.attrs[0].attr.values = 0;
    request->config.attrs[0].mask = KEEL__R3_RANGE_TRIG;
    request->config.attrs[1].attr.id = GPIO_V2_LINE_ATTR_ID_FLAGS;
    request->config.attrs[1].attr.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                                          GPIO_V2_LINE_FLAG_EDGE_FALLING |
                                          GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    request->config.attrs[1].mask = KEEL__R3_RANGE_ECHO;
    request->event_buffer_size = KEEL__R3_EDGE_BUFFER;
}

/* GPIO v2 requests are exclusive and the kernel creates their fds with
 * O_CLOEXEC. EBUSY after a clean preflight means another program won a race;
 * the line that is now USED is named if it can be found. */
static int32_t request_lines(keel__r3 *b, struct gpio_v2_line_request *request, int *out_fd,
                             const char *path, keel_open_report *report)
{
    struct gpio_v2_line_info info;
    unsigned i;
    int error;
    if (ioctl(b->chip_fd, GPIO_V2_GET_LINE_IOCTL, request) >= 0) {
        *out_fd = request->fd;
        return KEEL_OK;
    }
    error = last_error();
    if (error == ENOTTY || error == EINVAL) {
        return no_gpio_v2(report, path, error);
    }
    if (error == EBUSY) {
        for (i = 0; i < request->num_lines; ++i) {
            if (line_info(b, request->offsets[i], &info) == 0 &&
                (info.flags & GPIO_V2_LINE_FLAG_USED)) {
                return report_busy(report, path, request->offsets[i], info.consumer);
            }
        }
    }
    return line_error(report, path, error, request->offsets[0]);
}

/* ECHO edges are drained without blocking. */
static int32_t make_nonblocking(int fd, const char *path, keel_open_report *report)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return line_error(report, path, last_error(), KEEL__R3_BCM_ECHO);
    }
    return KEEL_OK;
}

int32_t keel__r3_gpio_open(keel__r3 *b, const keel_open_options *options,
                           keel_open_report *report)
{
    char path[KEEL_PATH_MAX];
    struct gpio_v2_line_request request;
    int with_range = !(options->flags & KEEL_OPEN_NO_RANGE);
    int32_t rc = find_chip(b, options, path, report);

    if (rc == KEEL_OK) {
        rc = check_lines(b, control_lines, KEEL__R3_CONTROL_LINES, path, report);
    }
    if (rc == KEEL_OK) {
        rc = check_lines(b, motor_lines, KEEL__R3_MOTOR_LINES, path, report);
    }
    if (rc == KEEL_OK) {
        rc = check_lines(b, input_lines, KEEL__R3_INPUT_LINES, path, report);
    }
    if (rc == KEEL_OK && with_range) {
        rc = check_lines(b, range_lines, KEEL__R3_RANGE_LINES, path, report);
    }
    /* Control first, so ENABLE is held LOW before the motor lines exist. */
    if (rc == KEEL_OK) {
        request_outputs(&request, control_lines, KEEL__R3_CONTROL_LINES);
        rc = request_lines(b, &request, &b->control_fd, path, report);
    }
    if (rc == KEEL_OK) {
        request_outputs(&request, motor_lines, KEEL__R3_MOTOR_LINES);
        rc = request_lines(b, &request, &b->motor_fd, path, report);
    }
    if (rc == KEEL_OK) {
        request_inputs(&request);
        rc = request_lines(b, &request, &b->input_fd, path, report);
    }
    if (rc == KEEL_OK && with_range) {
        request_range(&request);
        rc = request_lines(b, &request, &b->range_fd, path, report);
        if (rc == KEEL_OK) {
            rc = make_nonblocking(b->range_fd, path, report);
        }
    }
    /* The requests outlive the chip descriptor. */
    if (b->chip_fd >= 0) {
        (void)close(b->chip_fd);
        b->chip_fd = -1;
    }
    return rc;
}

/* ---- line values ---------------------------------------------------------- */

/* The kernel refuses an empty mask with EINVAL; selecting no line is a
 * no-op here (keel_set_button_leds with mask 0 asks for exactly that). */
int keel__r3_set_values(int fd, uint64_t mask, uint64_t bits)
{
    struct gpio_v2_line_values values;
    if (!mask) {
        return 0;
    }
    memset(&values, 0, sizeof values);
    values.mask = mask;
    values.bits = bits & mask;
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0 ? last_error() : 0;
}

int keel__r3_get_values(int fd, uint64_t mask, uint64_t *bits)
{
    struct gpio_v2_line_values values;
    *bits = 0;
    if (!mask) {
        return 0;
    }
    memset(&values, 0, sizeof values);
    values.mask = mask;
    if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
        return last_error();
    }
    *bits = values.bits & mask;
    return 0;
}

uint64_t keel__r3_control_bits(uint32_t outputs)
{
    uint64_t bits = (uint64_t)((outputs & KEEL_OUT_BUTTON_LEDS) >> KEEL_OUT_BUTTON_LED_SHIFT)
                    << KEEL__R3_CTL_LED_SHIFT;
    if (outputs & KEEL_OUT_ENABLE) {
        bits |= KEEL__R3_CTL_ENABLE;
    }
    if (outputs & KEEL_OUT_HEARTBEAT) {
        bits |= KEEL__R3_CTL_HEARTBEAT;
    }
    return bits;
}

/* ---- hooks ---------------------------------------------------------------- */

int32_t keel__r3_read_inputs(void *user, uint32_t *out_inputs)
{
    keel__r3 *b = user;
    uint64_t bits = 0;
    int32_t rc;
    int error = keel__r3_get_values(b->input_fd, KEEL__R3_IN_ALL, &bits);
    if (error) {
        pthread_mutex_lock(&b->lock);
        rc = keel__r3_latch(b, KEEL_E_IO, KEEL_CAUSE_GPIO_READ, error, 0);
        pthread_mutex_unlock(&b->lock);
        return rc;
    }
    /* The latch flag is an atomic, so reading inputs never waits for the
     * worker or a writer. */
    *out_inputs = ((bits & KEEL__R3_IN_READY) ? (uint32_t)KEEL_IN_READY : 0u) |
                  ((bits & KEEL__R3_IN_ARMED) ? (uint32_t)KEEL_IN_ARMED : 0u) |
                  (uint32_t)((bits >> KEEL__R3_IN_BUTTON_SHIFT) & 0xFu) << KEEL_IN_BUTTON_SHIFT |
                  (atomic_load(&b->latched) ? (uint32_t)KEEL_IN_BACKEND_FAULT : 0u);
    return KEEL_OK;
}

int32_t keel__r3_write_outputs(void *user, uint32_t mask, uint32_t values, uint32_t stop_epoch)
{
    keel__r3 *b = user;
    uint32_t next, rising;
    int32_t rc = KEEL_OK;
    int error;

    pthread_mutex_lock(&b->lock);
    if (mask & ~(uint32_t)KEEL_OUT_ALL) {
        rc = keel__r3_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERNAL, 0, mask);
        pthread_mutex_unlock(&b->lock);
        return rc;
    }
    next = (b->outputs & ~mask) | (values & mask);
    rising = next & ~b->outputs & (uint32_t)(KEEL_OUT_ENABLE | KEEL_OUT_HEARTBEAT);
    if (rising && b->fault.status != KEEL_OK) {
        /* While latched, LOW levels and LEDs are written; rises are refused. */
        next &= ~rising;
        rc = b->fault.status;
    } else if ((rising & KEEL_OUT_ENABLE) &&
               keel__r3_epoch_older(stop_epoch, b->newest_epoch)) {
        /* Decided before a newer stop: that stop must not be undone. */
        next &= ~(uint32_t)KEEL_OUT_ENABLE;
        b->stats.suppressed_writes++;
    } else if ((rising & KEEL_OUT_ENABLE) && b->motor.mode != KEEL_MOTOR_COAST) {
        /* Interlock: ENABLE may only rise while the motors coast. The servo
         * request is held LOW permanently on R3, so that half always holds. */
        (void)keel__r3_set_values(b->control_fd, keel__r3_control_bits(mask),
                                  keel__r3_control_bits(next & ~(uint32_t)KEEL_OUT_ENABLE));
        rc = keel__r3_latch(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERLOCK, 0, mask);
        pthread_mutex_unlock(&b->lock);
        return rc;
    }
    error = keel__r3_set_values(b->control_fd, keel__r3_control_bits(mask),
                                keel__r3_control_bits(next));
    if (error) {
        rc = keel__r3_latch(b, KEEL_E_IO, KEEL_CAUSE_GPIO_WRITE, error, mask);
    } else {
        b->outputs = next;
    }
    pthread_mutex_unlock(&b->lock);
    return rc;
}
