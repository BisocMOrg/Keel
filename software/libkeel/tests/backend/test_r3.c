/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel_r3 backend tests (SPEC 23.1, layer "backend"). The backend's sources
 * are compiled into this program with their system calls renamed to the
 * fake kernel, so every case runs without hardware: line requests and the
 * lines never requested, chip discovery, the errno mapping, the USED-line
 * preflight, interlocks and the stop epoch, the backend latch, the real-time
 * fallback, the PWM lateness budget and stall check on a mocked clock, I2C,
 * ranging, an end-to-end arm-drive-disarm through the public API with the
 * real worker thread, fork, and the stop path's fairness under contention.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include "fake_syscalls.h"

#include "backend/keel_r3/r3_open.c"
#include "backend/keel_r3/r3_gpio.c"
#include "backend/keel_r3/r3_motor.c"
#include "backend/keel_r3/r3_peripherals.c"

#undef open
#undef close
#undef ioctl
#undef fcntl
#undef read
#undef clock_gettime
#undef pthread_create

#include <sys/wait.h>

#include "check.h"

/* ---- helpers -------------------------------------------------------------- */

static keel_open_options make_options(uint32_t flags)
{
    keel_open_options options;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_R3;
    options.flags = flags;
    return options;
}

static keel_open_report make_report(void)
{
    keel_open_report report;
    memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    report.line = -1;
    return report;
}

static void no_violation(void)
{
    if (keel_fake.violation) {
        fprintf(stderr, "fake kernel contract violation: %s\n", keel_fake.violation);
        exit(1);
    }
}

static keel_device *open_with(const keel_open_options *options, keel_open_report *report)
{
    keel_device *dev = NULL;
    *report = make_report();
    CHECK_RC(keel__r3_open(options, &dev, report), KEEL_OK);
    CHECK(dev != NULL);
    return dev;
}

static keel_device *open_ok(uint32_t flags, keel_open_report *report)
{
    keel_open_options options = make_options(flags);
    return open_with(&options, report);
}

/* A failed open leaves no device, no descriptor and an actionable report. */
static int32_t open_fails(const keel_open_options *options, keel_open_report *report)
{
    keel_device *dev = (keel_device *)(uintptr_t)1;
    int32_t rc;
    *report = make_report();
    rc = keel__r3_open(options, &dev, report);
    CHECK(rc != KEEL_OK);
    CHECK(dev == NULL);
    CHECK_EQ(fake_open_fds(), 0);
    CHECK(report->hint[0] != '\0');
    CHECK_EQ(report->warnings, 0);
    return rc;
}

static void close_ok(keel_device *dev)
{
    CHECK_RC(keel_close(dev, NULL), KEEL_OK);
    CHECK_EQ(fake_open_fds(), 0);
    /* Destroy left every control and motor line LOW. */
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL), 0);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0);
    no_violation();
}

/* The backend alone, lines requested, no worker: for the hooks on a mocked
 * clock. */
static keel__r3 *bare_open(uint32_t flags)
{
    keel_open_options options = make_options(flags);
    keel_open_report report = make_report();
    keel__r3 *b = calloc(1, sizeof *b);
    CHECK(b != NULL);
    CHECK(backend_init(b, sizeof *b) == 0);
    CHECK_RC(keel__r3_gpio_open(b, &options, &report), KEEL_OK);
    return b;
}

static void bare_close(keel__r3 *b)
{
    release(b);
    fake_freeze_clock(0);
    CHECK_EQ(fake_open_fds(), 0);
    no_violation();
}

static keel_port_motor_command command(uint32_t mode, int32_t left, int32_t right,
                                       uint32_t epoch, uint64_t valid_until_us)
{
    keel_port_motor_command c;
    memset(&c, 0, sizeof c);
    c.mode = mode;
    c.left = left;
    c.right = right;
    c.stop_epoch = epoch;
    c.valid_until_us = valid_until_us;
    return c;
}

static int32_t motors(keel__r3 *b, uint32_t mode, int32_t left, int32_t right, uint32_t epoch,
                      uint64_t valid_until_us)
{
    keel_port_motor_command c = command(mode, left, right, epoch, valid_until_us);
    return keel__r3_write_motors(b, &c);
}

static int32_t pass(keel__r3 *b, uint64_t now_us, uint64_t *next)
{
    pthread_mutex_lock(&b->lock);
    *next = keel__r3_pwm_pass(b, now_us);
    pthread_mutex_unlock(&b->lock);
    return b->fault.status;
}

static keel_port_fault fault_of(keel__r3 *b)
{
    keel_port_fault fault;
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    CHECK_RC(keel__r3_get_fault(b, &fault), KEEL_OK);
    return fault;
}

static void check_fault(keel__r3 *b, int32_t status, uint32_t cause)
{
    keel_port_fault fault = fault_of(b);
    if (fault.status != status || fault.cause != cause) {
        fprintf(stderr, "backend fault is %s/%s, expected %s/%s\n", keel_status_name(fault.status),
                keel_fault_cause_name(fault.cause), keel_status_name(status),
                keel_fault_cause_name(cause));
        exit(1);
    }
}

static void fail_writes(int role, int error, unsigned count)
{
    fake_lock();
    keel_fake.set_errno[role] = error;
    keel_fake.set_fails[role] = count;
    fake_unlock();
}

static void fail_reads(int role, int error, unsigned count)
{
    fake_lock();
    keel_fake.get_errno[role] = error;
    keel_fake.get_fails[role] = count;
    fake_unlock();
}

static void set_chip(unsigned index, const char *path, const char *label, uint32_t lines)
{
    fake_chip *chip = &keel_fake.chips[index];
    memset(chip, 0, sizeof *chip);
    snprintf(chip->path, sizeof chip->path, "%s", path);
    snprintf(chip->label, sizeof chip->label, "%s", label);
    chip->lines = lines;
    if (keel_fake.chip_count <= index) {
        keel_fake.chip_count = index + 1;
    }
}

static int contains(const char *text, const char *part)
{
    return strstr(text, part) != NULL;
}

static uint32_t capabilities(const keel_device *dev)
{
    uint32_t caps = 0;
    CHECK_RC(keel_get_capabilities(dev, &caps), KEEL_OK);
    return caps;
}

/* ---- line requests -------------------------------------------------------- */

static void check_request(unsigned index, const uint32_t *lines, uint32_t count)
{
    const struct gpio_v2_line_request *r = &keel_fake.requests[index];
    CHECK_EQ(r->num_lines, count);
    CHECK(memcmp(r->offsets, lines, count * sizeof *lines) == 0);
    CHECK(strcmp(r->consumer, "keel") == 0);
}

static void requests(void)
{
    static const uint32_t control[] = {26, 18, 19, 23, 22, 17, 27};
    static const uint32_t motor[] = {8, 11, 10, 9};
    static const uint32_t input[] = {20, 21, 5, 6, 16, 24};
    static const uint32_t range[] = {13, 25};
    const struct gpio_v2_line_request *r = keel_fake.requests;
    keel_open_report report;
    keel_device *dev;
    uint64_t all = 0;
    unsigned i;

    fake_reset();
    dev = open_ok(0, &report);
    CHECK_EQ(report.backend, KEEL_BACKEND_R3);
    CHECK_EQ(keel_fake.request_count, 4);
    /* Control first, so ENABLE is held LOW before the motor lines exist;
     * every output starts LOW from the request itself. */
    check_request(0, control, 7);
    CHECK_EQ(r[0].config.flags, GPIO_V2_LINE_FLAG_OUTPUT);
    CHECK_EQ(r[0].config.num_attrs, 1);
    CHECK_EQ(r[0].config.attrs[0].attr.id, GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
    CHECK_EQ(r[0].config.attrs[0].attr.values, 0);
    CHECK_EQ(r[0].config.attrs[0].mask, 0x7F);
    check_request(1, motor, 4);
    CHECK_EQ(r[1].config.flags, GPIO_V2_LINE_FLAG_OUTPUT);
    CHECK_EQ(r[1].config.attrs[0].attr.values, 0);
    CHECK_EQ(r[1].config.attrs[0].mask, 0xF);
    /* READY/ARMED without bias (board pull-downs); buttons pull-up, active low. */
    check_request(2, input, 6);
    CHECK_EQ(r[2].config.flags, GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_DISABLED);
    CHECK_EQ(r[2].config.attrs[0].attr.id, GPIO_V2_LINE_ATTR_ID_FLAGS);
    CHECK_EQ(r[2].config.attrs[0].mask, 0x3C);
    CHECK_EQ(r[2].config.attrs[0].attr.flags, GPIO_V2_LINE_FLAG_INPUT |
                                                  GPIO_V2_LINE_FLAG_BIAS_PULL_UP |
                                                  GPIO_V2_LINE_FLAG_ACTIVE_LOW);
    /* TRIG starts LOW; ECHO has both edge events on the default (monotonic)
     * clock, pulled down, and is drained without blocking. */
    check_request(3, range, 2);
    CHECK_EQ(r[3].config.flags, GPIO_V2_LINE_FLAG_OUTPUT);
    CHECK_EQ(r[3].config.num_attrs, 2);
    CHECK_EQ(r[3].config.attrs[0].attr.id, GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
    CHECK_EQ(r[3].config.attrs[0].mask, 1);
    CHECK_EQ(r[3].config.attrs[0].attr.values, 0);
    CHECK_EQ(r[3].config.attrs[1].attr.id, GPIO_V2_LINE_ATTR_ID_FLAGS);
    CHECK_EQ(r[3].config.attrs[1].mask, 2);
    CHECK_EQ(r[3].config.attrs[1].attr.flags,
             GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                 GPIO_V2_LINE_FLAG_EDGE_FALLING | GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN);
    CHECK(r[3].event_buffer_size >= 16);
    CHECK(keel_fake.fd_flags[keel_fake.role_fd[FAKE_ROLE_RANGE] - FAKE_FD_BASE] & O_NONBLOCK);
    /* Every requested line was checked first, and only those. */
    for (i = 0; i < 4; ++i) {
        unsigned j;
        for (j = 0; j < r[i].num_lines; ++j) {
            all |= UINT64_C(1) << r[i].offsets[j];
        }
    }
    CHECK_EQ(keel_fake.queried, all);
    /* Requests, I2C; the chip descriptor is closed once the lines are held. */
    CHECK_EQ(fake_open_fds(), 5);
    CHECK_EQ(capabilities(dev), KEEL_CAP_MOTORS | KEEL_CAP_BRAKE | KEEL_CAP_RGB |
                                    KEEL_CAP_BUTTONS | KEEL_CAP_BUTTON_LEDS | KEEL_CAP_RANGE);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL), 0);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0);
    close_ok(dev);
}

/* BCM0, 1, 7 (SWD/NRST), 14, 15 (UART), 4 (I2C_INT) and 12 (J5) are never
 * requested or inspected, with or without ranging; the fake flags any use. */
static void never_requested(void)
{
    static const uint32_t forbidden_lines[] = {0, 1, 4, 7, 12, 14, 15};
    static const uint32_t flag_sets[] = {0, KEEL_OPEN_NO_RANGE,
                                         KEEL_OPEN_NO_RANGE | KEEL_OPEN_NO_REALTIME};
    keel_open_report report;
    unsigned f, i, j, k;
    for (f = 0; f < sizeof flag_sets / sizeof flag_sets[0]; ++f) {
        keel_device *dev;
        fake_reset();
        dev = open_ok(flag_sets[f], &report);
        for (i = 0; i < keel_fake.request_count; ++i) {
            for (j = 0; j < keel_fake.requests[i].num_lines; ++j) {
                for (k = 0; k < sizeof forbidden_lines / sizeof forbidden_lines[0]; ++k) {
                    CHECK(keel_fake.requests[i].offsets[j] != forbidden_lines[k]);
                }
            }
        }
        for (k = 0; k < sizeof forbidden_lines / sizeof forbidden_lines[0]; ++k) {
            CHECK(!(keel_fake.queried & (UINT64_C(1) << forbidden_lines[k])));
        }
        if (flag_sets[f] & KEEL_OPEN_NO_RANGE) {
            /* BCM13/25 are left for other software. */
            CHECK_EQ(keel_fake.request_count, 3);
            CHECK(!(keel_fake.queried & (UINT64_C(1) << 13)));
            CHECK(!(keel_fake.queried & (UINT64_C(1) << 25)));
            CHECK(!(capabilities(dev) & KEEL_CAP_RANGE));
        }
        close_ok(dev);
    }
}

/* ---- discovery, errno mapping and the preflight --------------------------- */

static void chip_discovery(void)
{
    keel_open_options options = make_options(0);
    keel_open_report report;
    keel_device *dev;

    /* The first chip with the label wins, wherever it is numbered. */
    fake_reset();
    keel_fake.chip_count = 0;
    set_chip(0, "/dev/gpiochip0", "raspberrypi-exp-gpio", 8);
    set_chip(1, "/dev/gpiochip3", "pinctrl-bcm2711", 58);
    dev = open_ok(0, &report);
    close_ok(dev);

    /* A Pi 5 is refused by label, naming what was found. */
    fake_reset();
    keel_fake.chip_count = 0;
    set_chip(0, "/dev/gpiochip0", "pinctrl-rp1", 54);
    CHECK_RC(open_fails(&options, &report), KEEL_E_NOT_FOUND);
    CHECK_EQ(report.resource, KEEL_RESOURCE_GPIO_CHIP);
    CHECK(strcmp(report.path, "/dev/gpiochip0") == 0);
    CHECK(contains(report.hint, "pinctrl-rp1"));

    /* The right label with too few lines is not the GPIO bank. */
    fake_reset();
    keel_fake.chip_count = 0;
    set_chip(0, "/dev/gpiochip0", "pinctrl-bcm2711", 20);
    CHECK_RC(open_fails(&options, &report), KEEL_E_NOT_FOUND);

    /* No chip at all. */
    fake_reset();
    keel_fake.chip_count = 0;
    CHECK_RC(open_fails(&options, &report), KEEL_E_NOT_FOUND);
    CHECK_EQ(report.os_error, ENOENT);

    /* The udev link is the fallback after the scan. */
    fake_reset();
    keel_fake.chip_count = 0;
    set_chip(0, "/dev/keel/gpiochip", "pinctrl-bcm2711", 58);
    dev = open_ok(0, &report);
    close_ok(dev);

    /* A chip we may not open is the most useful thing to report. */
    fake_reset();
    keel_fake.chips[0].open_errno = EACCES;
    CHECK_RC(open_fails(&options, &report), KEEL_E_PERMISSION);
    CHECK_EQ(report.os_error, EACCES);
    CHECK(strcmp(report.path, "/dev/gpiochip0") == 0);
    CHECK(contains(report.hint, "keel-setup"));

    /* An explicit path must still be the BCM2711 bank. */
    fake_reset();
    snprintf(options.gpiochip_path, sizeof options.gpiochip_path, "/dev/gpiochip1");
    CHECK_RC(open_fails(&options, &report), KEEL_E_NOT_FOUND);
    CHECK(contains(report.hint, "raspberrypi-exp-gpio"));
    snprintf(options.gpiochip_path, sizeof options.gpiochip_path, "/dev/gpiochip0");
    dev = open_with(&options, &report);
    close_ok(dev);

    /* A node that is not a GPIO chip at all. */
    fake_reset();
    keel_fake.chips[0].info_errno = ENOTTY;
    CHECK_RC(open_fails(&options, &report), KEEL_E_NOT_FOUND);
    CHECK_EQ(report.os_error, ENOTTY);
    no_violation();
}

static void errno_mapping(void)
{
    static const struct {
        int error;
        int32_t status;
    } open_table[] = {{EACCES, KEEL_E_PERMISSION}, {EPERM, KEEL_E_PERMISSION},
                      {ENOENT, KEEL_E_NOT_FOUND},  {ENODEV, KEEL_E_NOT_FOUND},
                      {ENXIO, KEEL_E_NOT_FOUND},   {EBUSY, KEEL_E_IN_USE},
                      {ENOMEM, KEEL_E_NO_MEMORY},  {EIO, KEEL_E_SETUP_IO},
                      {EINVAL, KEEL_E_SETUP_IO}};
    static const struct {
        int error;
        int32_t status;
    } request_table[] = {{EACCES, KEEL_E_PERMISSION}, {EPERM, KEEL_E_PERMISSION},
                         {ENOENT, KEEL_E_NOT_FOUND},  {ENODEV, KEEL_E_NOT_FOUND},
                         {ENXIO, KEEL_E_NOT_FOUND},   {EBUSY, KEEL_E_IN_USE},
                         {ENOTTY, KEEL_E_NOT_FOUND},  {EINVAL, KEEL_E_NOT_FOUND},
                         {ENOMEM, KEEL_E_NO_MEMORY},  {EIO, KEEL_E_SETUP_IO}};
    keel_open_options options = make_options(0);
    keel_open_report report;
    unsigned i;
    int stage;

    /* Opening the chip named by the options. */
    snprintf(options.gpiochip_path, sizeof options.gpiochip_path, "/dev/gpiochip0");
    for (i = 0; i < sizeof open_table / sizeof open_table[0]; ++i) {
        fake_reset();
        keel_fake.chips[0].open_errno = open_table[i].error;
        CHECK_RC(open_fails(&options, &report), open_table[i].status);
        CHECK_EQ(report.os_error, open_table[i].error);
        CHECK(strcmp(report.path, "/dev/gpiochip0") == 0);
    }
    /* Each of the four line requests; the earlier ones are released. ENOTTY
     * and EINVAL mean a kernel without GPIO v2. */
    options = make_options(0);
    for (i = 0; i < sizeof request_table / sizeof request_table[0]; ++i) {
        for (stage = 0; stage < 4; ++stage) {
            fake_reset();
            keel_fake.request_errno = request_table[i].error;
            keel_fake.request_fail_at = stage;
            CHECK_RC(open_fails(&options, &report), request_table[i].status);
            CHECK_EQ(report.os_error, request_table[i].error);
            CHECK(report.path[0] != '\0');
        }
    }
    /* GPIO_V2_GET_LINEINFO unknown: no v2 interface. */
    fake_reset();
    keel_fake.lineinfo_errno = ENOTTY;
    CHECK_RC(open_fails(&options, &report), KEEL_E_NOT_FOUND);
    CHECK(contains(report.hint, "5.10"));
    no_violation();
}

static void line_in_use(void)
{
    static const struct {
        uint32_t line;
        const char *consumer;
        const char *hint;
    } table[] = {{9, "", "spi=off"},
                 {8, "spi0 CS0", "spi=off"},
                 {20, "gpio-shutdown", "\"gpio-shutdown\""},
                 {26, "", "kernel function"},
                 {13, "w1-gpio", "w1-gpio"}};
    keel_open_options options = make_options(0);
    keel_open_report report, first_report;
    keel_device *first;
    unsigned i;

    for (i = 0; i < sizeof table / sizeof table[0]; ++i) {
        fake_reset();
        keel_fake.line_used[table[i].line] = 1;
        snprintf(keel_fake.line_consumer[table[i].line], sizeof keel_fake.line_consumer[0], "%s",
                 table[i].consumer);
        CHECK_RC(open_fails(&options, &report), KEEL_E_IN_USE);
        CHECK_EQ(report.resource, KEEL_RESOURCE_GPIO_LINE);
        CHECK_EQ(report.line, table[i].line);
        CHECK_EQ(report.os_error, EBUSY);
        CHECK(strcmp(report.consumer, table[i].consumer) == 0);
        CHECK(contains(report.hint, table[i].hint));
        /* The preflight refuses before any line is requested. */
        CHECK_EQ(keel_fake.request_count, 0);
    }
    /* A line the options leave alone may be busy. */
    fake_reset();
    keel_fake.line_used[25] = 1;
    first = open_ok(KEEL_OPEN_NO_RANGE, &report);
    close_ok(first);
    /* One open device per robot: a second open names the first. */
    fake_reset();
    first = open_ok(0, &first_report);
    CHECK_RC(keel__r3_open(&options, &(keel_device *){NULL}, &report), KEEL_E_IN_USE);
    CHECK(strcmp(report.consumer, "keel") == 0);
    CHECK(contains(report.hint, "another keel program"));
    close_ok(first);
}

/* ---- interlocks, stop epoch and the latch --------------------------------- */

static void interlocks(void)
{
    keel__r3 *b;
    uint64_t now;
    fake_reset();
    b = bare_open(0);
    keel_fake.armed_force = 1;
    now = fake_real_now_us();

    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 1);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 300, 300, 0, now + 50000), KEEL_OK);
    CHECK_EQ(b->motor.mode, KEEL_MOTOR_DRIVE);

    /* ENABLE may only rise while the motors coast. */
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, 0, 0), KEEL_OK);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_E_INTERNAL);
    check_fault(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERLOCK);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0);
    CHECK_EQ(b->motor.mode, KEEL_MOTOR_COAST);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* DRIVE and BRAKE need ENABLE HIGH. */
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 300, 300, 0, now + 50000), KEEL_E_INTERNAL);
    check_fault(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERLOCK);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_BRAKE, 0, 0, 0, now + 50000), KEEL_E_INTERNAL);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* ... and a fresh ARMED = 1: ENABLE alone is HIGH during the
     * acknowledgement too. ARMED = 0 is feedback, not a defect. */
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    keel_fake.armed_force = 0;
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 300, 300, 0, now + 50000), KEEL_E_FEEDBACK);
    check_fault(b, KEEL_E_FEEDBACK, KEEL_CAUSE_ARMED_LOST);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0); /* the self-stop lowered ENABLE */
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* That read failing is an input failure. */
    keel_fake.armed_force = 1;
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    fail_reads(FAKE_ROLE_INPUT, EIO, 1);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 300, 300, 0, now + 50000), KEEL_E_IO);
    check_fault(b, KEEL_E_IO, KEEL_CAUSE_GPIO_READ);
    CHECK_EQ(fault_of(b).os_error, EIO);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* Brake with every condition met. */
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_BRAKE, 0, 0, 0, now + 50000), KEEL_OK);
    CHECK_EQ(b->motor.mode, KEEL_MOTOR_BRAKE);

    /* A request the core can never make is a contract failure. */
    CHECK_RC(motors(b, 7, 0, 0, 0, now + 50000), KEEL_E_INTERNAL);
    check_fault(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERNAL);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 1001, 0, 0, now + 50000), KEEL_E_INTERNAL);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);
    CHECK_RC(keel__r3_write_outputs(b, 0x1000, 0, 0), KEEL_E_INTERNAL);
    check_fault(b, KEEL_E_INTERNAL, KEEL_CAUSE_INTERNAL);
    bare_close(b);
}

static void stop_epoch(void)
{
    keel__r3 *b;
    uint64_t now;
    keel_port_stats stats;
    fake_reset();
    b = bare_open(0);
    keel_fake.armed_force = 1;
    now = fake_real_now_us();
    memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;

    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, 500, 0, now + 50000), KEEL_OK);
    /* emergency_disable: ENABLE LOW and COAST at once, newest epoch 5. */
    CHECK_RC(keel__r3_emergency_disable(b, 5), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0);
    CHECK_EQ(b->motor.mode, KEEL_MOTOR_COAST);
    /* Decided under an older epoch: suppressed with OK, nothing moves. */
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 4), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, 500, 4, now + 50000), KEEL_OK);
    CHECK_EQ(b->motor.mode, KEEL_MOTOR_COAST);
    CHECK_RC(keel__r3_get_stats(b, &stats), KEEL_OK);
    CHECK_EQ(stats.suppressed_writes, 2);
    CHECK_EQ(fault_of(b).status, KEEL_OK);
    /* The current epoch is honoured. */
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 5), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 1);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, 500, 5, now + 50000), KEEL_OK);
    CHECK_EQ(b->motor.mode, KEEL_MOTOR_DRIVE);
    /* Epochs compare as serial numbers across the wrap. */
    CHECK_RC(keel__r3_emergency_disable(b, 0x40000000u), KEEL_OK);
    CHECK_RC(keel__r3_emergency_disable(b, 0x80000005u), KEEL_OK);
    CHECK_RC(keel__r3_emergency_disable(b, 2u), KEEL_OK);
    CHECK_EQ(b->newest_epoch, 2);
    CHECK_RC(keel__r3_emergency_disable(b, 0x80000005u), KEEL_OK); /* older: kept at 2 */
    CHECK_EQ(b->newest_epoch, 2);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0x80000005u), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0);
    /* A lease already over when it arrives is a counted, silent coast. */
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 2), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, 500, 2, now - 1), KEEL_OK);
    CHECK_EQ(b->motor.mode, KEEL_MOTOR_COAST);
    CHECK_RC(keel__r3_get_stats(b, &stats), KEEL_OK);
    CHECK_EQ(stats.lease_expiries, 1);
    CHECK_EQ(fault_of(b).status, KEEL_OK);
    bare_close(b);
}

static void backend_latch(void)
{
    keel__r3 *b;
    uint32_t inputs = 0;
    uint64_t now;
    fake_reset();
    b = bare_open(0);
    now = fake_real_now_us();
    keel_fake.buttons = KEEL_BUTTON_A | KEEL_BUTTON_X;

    /* Inputs: READY, ARMED and the buttons as logical "pressed". */
    CHECK_RC(keel__r3_read_inputs(b, &inputs), KEEL_OK);
    CHECK_EQ(inputs, KEEL_IN_READY | ((uint32_t)(KEEL_BUTTON_A | KEEL_BUTTON_X) << 8));
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_HEARTBEAT, KEEL_OUT_HEARTBEAT, 0), KEEL_OK);
    /* An empty mask selects nothing (the kernel would refuse it): no fault. */
    CHECK_RC(keel__r3_write_outputs(b, 0, 0, 0), KEEL_OK);
    CHECK_EQ(fault_of(b).status, KEEL_OK);

    /* A failed control write latches GPIO_WRITE with the errno and mask. */
    fail_writes(FAKE_ROLE_CONTROL, EIO, 1);
    CHECK_RC(keel__r3_write_outputs(b, 1u << KEEL_OUT_BUTTON_LED_SHIFT,
                                    1u << KEEL_OUT_BUTTON_LED_SHIFT, 0),
             KEEL_E_IO);
    check_fault(b, KEEL_E_IO, KEEL_CAUSE_GPIO_WRITE);
    CHECK_EQ(fault_of(b).os_error, EIO);
    CHECK_EQ(fault_of(b).detail, 1u << KEEL_OUT_BUTTON_LED_SHIFT);
    /* The self-stop never touches the heartbeat; that is the core's call. */
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 2u, 2);

    /* Latched: inputs carry the pseudo-bit, rises are refused, LOW levels,
     * LEDs and COAST still work. */
    CHECK_RC(keel__r3_read_inputs(b, &inputs), KEEL_OK);
    CHECK(inputs & KEEL_IN_BACKEND_FAULT);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_HEARTBEAT, 0, 0), KEEL_OK);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_HEARTBEAT, KEEL_OUT_HEARTBEAT, 0), KEEL_E_IO);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 2u, 0);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE | (1u << KEEL_OUT_BUTTON_LED_SHIFT),
                                    KEEL_OUT_ENABLE | (1u << KEEL_OUT_BUTTON_LED_SHIFT), 0),
             KEEL_E_IO);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & (1u << 3), 1u << 3); /* LED A lit */
    CHECK_RC(motors(b, KEEL_MOTOR_COAST, 0, 0, 0, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 100, 100, 0, now + 50000), KEEL_E_IO);
    /* The first record wins. */
    fail_writes(FAKE_ROLE_MOTOR, EINVAL, 1);
    CHECK_RC(motors(b, KEEL_MOTOR_COAST, 0, 0, 0, 0), KEEL_E_IO);
    check_fault(b, KEEL_E_IO, KEEL_CAUSE_GPIO_WRITE);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);
    CHECK_RC(keel__r3_read_inputs(b, &inputs), KEEL_OK);
    CHECK(!(inputs & KEEL_IN_BACKEND_FAULT));
    CHECK_EQ(fault_of(b).status, KEEL_OK);

    /* An input read failure. */
    fail_reads(FAKE_ROLE_INPUT, EIO, 1);
    CHECK_RC(keel__r3_read_inputs(b, &inputs), KEEL_E_IO);
    check_fault(b, KEEL_E_IO, KEEL_CAUSE_GPIO_READ);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* A synchronous coast that fails is MOTOR_WRITE (E_IO, not E_PWM). */
    fail_writes(FAKE_ROLE_MOTOR, EIO, 1);
    CHECK_RC(motors(b, KEEL_MOTOR_COAST, 0, 0, 0, 0), KEEL_E_IO);
    check_fault(b, KEEL_E_IO, KEEL_CAUSE_MOTOR_WRITE);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* emergency_disable reports its own failures. */
    fail_writes(FAKE_ROLE_CONTROL, EIO, 1);
    CHECK_RC(keel__r3_emergency_disable(b, 1), KEEL_E_IO);
    check_fault(b, KEEL_E_IO, KEEL_CAUSE_GPIO_WRITE);
    CHECK_EQ(fault_of(b).detail, KEEL_OUT_ENABLE);
    bare_close(b);
}

/* ---- the PWM worker -------------------------------------------------------- */

static void realtime_fallback(void)
{
    keel_open_options options = make_options(0);
    keel_open_report report;
    keel_snapshot snapshot;
    keel_device *dev;

    /* Refused (an unprivileged user): best effort, warned, with a remedy. */
    fake_reset();
    dev = open_ok(0, &report);
    CHECK_EQ(keel_fake.fifo_attempts, 1);
    CHECK_EQ(keel_fake.creations, 1);
    CHECK_EQ(keel_fake.last_policy, SCHED_OTHER);
    CHECK_EQ(keel_fake.last_inherit, PTHREAD_EXPLICIT_SCHED);
    CHECK(keel_fake.last_stack >= 65536);
    CHECK(keel_fake.sigint_blocked);   /* no application handler on the worker */
    CHECK(!keel_fake.sigsegv_blocked); /* synchronous faults stay deliverable */
    CHECK(report.warnings & KEEL_OPEN_WARN_BEST_EFFORT_TIMING);
    CHECK_EQ(report.realtime_os_error, EPERM);
    CHECK_EQ(report.timing_class, KEEL_TIMING_BEST_EFFORT);
    CHECK_EQ(report.resource, KEEL_RESOURCE_PWM_WORKER);
    CHECK(contains(report.hint, "rtprio"));
    CHECK(!(report.capabilities & KEEL_CAP_REALTIME_PWM));
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.struct_size = sizeof snapshot;
    CHECK_RC(keel_get_snapshot(dev, &snapshot), KEEL_OK);
    CHECK_EQ(snapshot.timing_class, KEEL_TIMING_BEST_EFFORT);
    close_ok(dev);

    /* Granted: SCHED_FIFO at the fixed priority 40, no warning. */
    fake_reset();
    keel_fake.thread_mode = FAKE_THREADS_FIFO_GRANTED;
    dev = open_ok(0, &report);
    CHECK_EQ(keel_fake.last_policy, SCHED_FIFO);
    CHECK_EQ(keel_fake.last_priority, KEEL_RT_PRIORITY);
    CHECK_EQ(keel_fake.last_inherit, PTHREAD_EXPLICIT_SCHED);
    CHECK_EQ(report.warnings, 0);
    CHECK_EQ(report.realtime_os_error, 0);
    CHECK_EQ(report.timing_class, KEEL_TIMING_REALTIME);
    CHECK_EQ(report.resource, KEEL_RESOURCE_NONE);
    CHECK(capabilities(dev) & KEEL_CAP_REALTIME_PWM);
    close_ok(dev);

    /* Disabled by the caller: never attempted, nothing to warn about. */
    fake_reset();
    dev = open_ok(KEEL_OPEN_NO_REALTIME, &report);
    CHECK_EQ(keel_fake.fifo_attempts, 0);
    CHECK_EQ(report.warnings, 0);
    CHECK_EQ(report.timing_class, KEEL_TIMING_BEST_EFFORT);
    close_ok(dev);

    /* No thread at all: no device, every line released LOW. */
    fake_reset();
    keel_fake.thread_mode = FAKE_THREADS_FAIL;
    CHECK_RC(open_fails(&options, &report), KEEL_E_SETUP_IO);
    CHECK_EQ(report.resource, KEEL_RESOURCE_PWM_WORKER);
    CHECK_EQ(report.os_error, EAGAIN);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL), 0);

    /* The report describes the first degradation; both are warned. */
    fake_reset();
    keel_fake.i2c_open_errno = ENOENT;
    dev = open_ok(0, &report);
    CHECK_EQ(report.warnings, KEEL_OPEN_WARN_NO_RGB | KEEL_OPEN_WARN_BEST_EFFORT_TIMING);
    CHECK_EQ(report.resource, KEEL_RESOURCE_I2C_BUS);
    close_ok(dev);
}

/* A drive pattern on a mocked clock: late wakes of 1.9, 2.5 and 9.9 ms are
 * counted and recompute the output; one wake 31 ms late latches OVERRUN;
 * steady excess lateness latches DEGRADED. The self-stop leaves the
 * heartbeat alone. */
static void lateness_budget(void)
{
    const uint64_t t0 = UINT64_C(10000000), t1 = t0 + 200000;
    keel__r3 *b;
    uint64_t next = 0;
    keel_port_stats stats;
    keel_port_fault fault;
    uint32_t inputs = 0;
    fake_reset();
    b = bare_open(0);
    keel_fake.armed_force = 1;
    memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;

    fake_freeze_clock(t0);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE | KEEL_OUT_HEARTBEAT,
                                    KEEL_OUT_ENABLE | KEEL_OUT_HEARTBEAT, 0),
             KEEL_OK);
    /* Left forward 50 % (BCM11), right reverse 25 % (BCM9). */
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, -250, 0, t0 + 100000), KEEL_OK);
    CHECK_RC(pass(b, t0, &next), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), KEEL__PWM_LN | KEEL__PWM_RN);
    CHECK_EQ(next, t0 + 2500); /* the right input falls first */
    CHECK_RC(pass(b, next + 1900, &next), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), KEEL__PWM_LN);
    CHECK_EQ(next, t0 + 5000);
    CHECK_RC(pass(b, next + 2500, &next), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0);
    CHECK_EQ(next, t0 + 10000);
    CHECK_RC(pass(b, next + 9900, &next), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0); /* 9.9 ms into the second period */
    CHECK_EQ(next, t0 + 20000);
    CHECK_RC(keel__r3_get_stats(b, &stats), KEEL_OK);
    CHECK_EQ(stats.wakes, 4);
    CHECK_EQ(stats.late_wakes, 3);
    CHECK_EQ(stats.very_late_wakes, 1);
    CHECK_EQ(stats.max_lateness_us, 9900);
    CHECK_EQ(stats.last_lateness_us, 9900);

    /* One wake 31 ms late. */
    CHECK_RC(pass(b, next + 31000, &next), KEEL_E_PWM);
    CHECK_EQ(next, 0);
    fault = fault_of(b);
    CHECK_EQ(fault.cause, KEEL_CAUSE_PWM_OVERRUN);
    CHECK_EQ(fault.detail, 31000);
    CHECK_EQ(fault.time_us, t0);
    CHECK_RC(keel__r3_read_inputs(b, &inputs), KEEL_OK);
    CHECK(inputs & KEEL_IN_BACKEND_FAULT);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & (KEEL__R3_CTL_ENABLE | KEEL__R3_CTL_SERVO_REQUEST),
             0);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & KEEL__R3_CTL_HEARTBEAT, KEEL__R3_CTL_HEARTBEAT);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* 6 ms late on every pass is 5 ms of excess each: the fifth pass within
     * 100 ms crosses the 20 ms budget. */
    fake_freeze_clock(t1);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, 500, 0, t1 + 100000), KEEL_OK);
    next = t1;
    CHECK_RC(pass(b, next + 6000, &next), KEEL_OK);
    CHECK_RC(pass(b, next + 6000, &next), KEEL_OK);
    CHECK_RC(pass(b, next + 6000, &next), KEEL_OK);
    CHECK_RC(pass(b, next + 6000, &next), KEEL_OK);
    CHECK_RC(pass(b, next + 6000, &next), KEEL_E_PWM);
    fault = fault_of(b);
    CHECK_EQ(fault.cause, KEEL_CAUSE_PWM_DEGRADED);
    CHECK_EQ(fault.detail, 25000);
    bare_close(b);
}

/* The control thread catches a worker that stopped running: an unexpired
 * DRIVE or BRAKE whose scheduled wake is over 25 ms in the past. */
static void stall(void)
{
    const uint64_t t0 = UINT64_C(20000000), t1 = t0 + 1000000, t2 = t1 + 1000000;
    keel__r3 *b;
    uint64_t next = 0;
    keel_port_fault fault;
    fake_reset();
    b = bare_open(0);
    keel_fake.armed_force = 1;

    fake_freeze_clock(t0);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 400, 400, 0, t0 + 60000), KEEL_OK);
    fake_freeze_clock(t0 + 24000);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 400, 400, 0, t0 + 90000), KEEL_OK);
    fake_freeze_clock(t0 + 26000);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 400, 400, 0, t0 + 96000), KEEL_E_PWM);
    fault = fault_of(b);
    CHECK_EQ(fault.cause, KEEL_CAUSE_PWM_STALL);
    CHECK_EQ(fault.detail, 26000);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0); /* driven LOW by this thread */
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* An expired command is the lease's business, not a stall. */
    fake_freeze_clock(t1);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 400, 400, 0, t1 + 10000), KEEL_OK);
    fake_freeze_clock(t1 + 40000);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 400, 400, 0, t1 + 80000), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_COAST, 0, 0, 0, 0), KEEL_OK);

    /* A brake has no periodic wakes: its scheduled wake is the lease end. */
    fake_freeze_clock(t2);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_BRAKE, 0, 0, 0, t2 + 60000), KEEL_OK);
    CHECK_RC(pass(b, t2, &next), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), KEEL__PWM_ALL);
    CHECK_EQ(next, t2 + 60000);
    fake_freeze_clock(t2 + 50000);
    CHECK_RC(motors(b, KEEL_MOTOR_BRAKE, 0, 0, 0, t2 + 110000), KEEL_OK);
    fake_freeze_clock(t2 + 85000);
    CHECK_RC(motors(b, KEEL_MOTOR_BRAKE, 0, 0, 0, t2 + 150000), KEEL_OK);
    fake_freeze_clock(t2 + 86000);
    CHECK_RC(motors(b, KEEL_MOTOR_BRAKE, 0, 0, 0, t2 + 150000), KEEL_E_PWM);
    check_fault(b, KEEL_E_PWM, KEEL_CAUSE_PWM_STALL);
    bare_close(b);
}

static void worker_faults(void)
{
    const uint64_t t0 = UINT64_C(30000000), t1 = t0 + 1000000;
    keel__r3 *b;
    uint64_t next = 0;
    keel_port_stats stats;
    fake_reset();
    b = bare_open(0);
    keel_fake.armed_force = 1;
    memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;

    /* A motor-line write by the worker that fails: PWM_WRITE, errno kept. */
    fake_freeze_clock(t0);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, 0, 0, t0 + 50000), KEEL_OK);
    fail_writes(FAKE_ROLE_MOTOR, EIO, 1);
    CHECK_RC(pass(b, t0, &next), KEEL_E_PWM);
    check_fault(b, KEEL_E_PWM, KEEL_CAUSE_PWM_WRITE);
    CHECK_EQ(fault_of(b).os_error, EIO);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0);
    CHECK_RC(keel__r3_reset_fault(b), KEEL_OK);

    /* The lease ends without renewal: a silent coast, never a latch, and
     * the next renewal starts a new phase. */
    fake_freeze_clock(t1);
    CHECK_RC(keel__r3_write_outputs(b, KEEL_OUT_ENABLE, KEEL_OUT_ENABLE, 0), KEEL_OK);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, 0, 0, t1 + 30000), KEEL_OK);
    CHECK_RC(pass(b, t1, &next), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), KEEL__PWM_LN);
    CHECK_RC(pass(b, t1 + 30000, &next), KEEL_OK);
    CHECK_EQ(next, 0);
    CHECK_EQ(b->motor.mode, KEEL_MOTOR_COAST);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0);
    CHECK_RC(keel__r3_get_stats(b, &stats), KEEL_OK);
    CHECK_EQ(stats.lease_expiries, 1);
    fake_freeze_clock(t1 + 35000);
    CHECK_RC(motors(b, KEEL_MOTOR_DRIVE, 500, 0, 0, t1 + 80000), KEEL_OK);
    CHECK_EQ(b->motion_start_us, t1 + 35000);
    CHECK_EQ(fault_of(b).status, KEEL_OK);
    bare_close(b);
}

/* ---- peripherals ---------------------------------------------------------- */

static void i2c(void)
{
    keel_open_options options = make_options(KEEL_OPEN_NO_RANGE);
    keel_open_report report;
    keel_device *dev;
    uint32_t state = 99;
    unsigned before;

    fake_reset();
    dev = open_ok(KEEL_OPEN_NO_RANGE, &report);
    CHECK_EQ(keel_fake.i2c_timeout, 2); /* 20 ms, not the adapter's second */
    CHECK(capabilities(dev) & KEEL_CAP_RGB);
    /* The LED initialisation ended with "wake" (register 0 = 1), one
     * message per transaction, the address in the message. */
    CHECK_EQ(keel_fake.i2c_nmsgs, 1);
    CHECK_EQ(keel_fake.i2c_addr, 0x54);
    CHECK_EQ(keel_fake.i2c_len, 2);
    CHECK(keel_fake.i2c_data[0] == 0x00 && keel_fake.i2c_data[1] == 1);
    before = keel_fake.i2c_transfers;
    CHECK_RC(keel_set_rgb(dev, 0, 0x102030), KEEL_OK);
    CHECK_EQ(keel_fake.i2c_transfers, before + 2); /* frame, latch */
    CHECK_EQ(keel_fake.i2c_data[0], 0x16);
    /* A bus failure is a peripheral error and never latches. */
    keel_fake.i2c_fail = 1;
    CHECK_RC(keel_set_rgb(dev, 0, 0x000000), KEEL_E_PERIPHERAL);
    CHECK_RC(keel_get_state(dev, &state), KEEL_OK);
    CHECK_EQ(state, KEEL_STATE_DISARMED);
    keel_fake.i2c_fail = 0;
    CHECK_RC(keel_set_rgb(dev, 0, 0x000000), KEEL_OK);
    close_ok(dev);
    /* Close shut the driver down (register 0 = 0). */
    CHECK(keel_fake.i2c_data[0] == 0x00 && keel_fake.i2c_data[1] == 0);

    /* No bus: the device opens without RGB and says why. */
    fake_reset();
    keel_fake.i2c_open_errno = ENOENT;
    keel_fake.thread_mode = FAKE_THREADS_FIFO_GRANTED;
    dev = open_with(&options, &report);
    CHECK_EQ(report.warnings, KEEL_OPEN_WARN_NO_RGB);
    CHECK_EQ(report.resource, KEEL_RESOURCE_I2C_BUS);
    CHECK_EQ(report.os_error, ENOENT);
    CHECK(strcmp(report.path, "/dev/i2c-1") == 0);
    CHECK(contains(report.hint, "i2c_arm"));
    CHECK(!(capabilities(dev) & KEEL_CAP_RGB));
    CHECK_RC(keel_set_rgb(dev, 0, 0x102030), KEEL_E_UNSUPPORTED);
    close_ok(dev);

    /* No permission, and a node that is not an adapter. */
    fake_reset();
    keel_fake.i2c_open_errno = EACCES;
    dev = open_with(&options, &report);
    CHECK(contains(report.hint, "i2c"));
    CHECK(contains(report.hint, "group"));
    close_ok(dev);
    fake_reset();
    keel_fake.i2c_timeout_errno = ENOTTY;
    dev = open_with(&options, &report);
    CHECK(report.warnings & KEEL_OPEN_WARN_NO_RGB);
    CHECK_EQ(fake_open_fds(), 3); /* the adapter was closed again */
    close_ok(dev);

    /* An explicit adapter path. */
    fake_reset();
    snprintf(keel_fake.i2c_path, sizeof keel_fake.i2c_path, "/dev/i2c-7");
    snprintf(options.i2c_path, sizeof options.i2c_path, "/dev/i2c-7");
    dev = open_with(&options, &report);
    CHECK(capabilities(dev) & KEEL_CAP_RGB);
    close_ok(dev);

    /* A bus without the LED driver (no acknowledge): RGB is dropped too. */
    fake_reset();
    keel_fake.i2c_fail = 1;
    dev = open_ok(KEEL_OPEN_NO_RANGE, &report);
    CHECK(report.warnings & KEEL_OPEN_WARN_NO_RGB);
    CHECK(!(capabilities(dev) & KEEL_CAP_RGB));
    keel_fake.i2c_fail = 0;
    close_ok(dev);
}

static int range_writes(unsigned from, fake_write *out, unsigned capacity)
{
    unsigned i, n = 0;
    fake_lock();
    for (i = from; i < keel_fake.log_count && n < capacity; ++i) {
        if (keel_fake.log[i].role == FAKE_ROLE_RANGE) {
            out[n++] = keel_fake.log[i];
        }
    }
    fake_unlock();
    return (int)n;
}

static void range(void)
{
    keel__r3 *b;
    keel_port_edge edges[8];
    fake_write writes[4];
    uint32_t echo_high = 9, count = 9;
    unsigned before;
    fake_reset();
    b = bare_open(0);

    /* TRIG HIGH for at least 10 us, then LOW. */
    before = keel_fake.log_count;
    CHECK_RC(keel__r3_range_trigger(b, &echo_high), KEEL_OK);
    CHECK_EQ(echo_high, 0);
    CHECK_EQ(range_writes(before, writes, 4), 2);
    CHECK(writes[0].mask == 1 && writes[0].bits == 1);
    CHECK(writes[1].mask == 1 && writes[1].bits == 0);
    CHECK(writes[1].time_us - writes[0].time_us >= KEEL__R3_TRIG_PULSE_US);
    /* ECHO still HIGH: no trigger at all. */
    keel_fake.echo = 1;
    before = keel_fake.log_count;
    CHECK_RC(keel__r3_range_trigger(b, &echo_high), KEEL_OK);
    CHECK_EQ(echo_high, 1);
    CHECK_EQ(range_writes(before, writes, 4), 0);
    keel_fake.echo = 0;

    /* Edges: none pending is not an error. */
    CHECK_RC(keel__r3_range_edges(b, edges, 8, &count), KEEL_OK);
    CHECK_EQ(count, 0);
    /* Kernel timestamps (ns, CLOCK_MONOTONIC) become backend microseconds. */
    fake_push_event(UINT64_C(5000000000), 1, 1);
    fake_push_event(UINT64_C(5001500000), 0, 2);
    CHECK_RC(keel__r3_range_edges(b, edges, 8, &count), KEEL_OK);
    CHECK_EQ(count, 2);
    CHECK(edges[0].time_us == UINT64_C(5000000) && edges[0].rising == 1);
    CHECK(edges[1].time_us == UINT64_C(5001500) && edges[1].rising == 0);
    /* The capacity is respected. */
    fake_push_event(UINT64_C(6000000000), 1, 3);
    fake_push_event(UINT64_C(6000100000), 0, 4);
    fake_push_event(UINT64_C(6100000000), 1, 5);
    CHECK_RC(keel__r3_range_edges(b, edges, 2, &count), KEEL_OK);
    CHECK_EQ(count, 2);
    CHECK_RC(keel__r3_range_edges(b, edges, 8, &count), KEEL_OK);
    CHECK_EQ(count, 1);
    /* A gap in the line's sequence numbers: the kernel dropped events. */
    fake_push_event(UINT64_C(6200000000), 0, 7);
    CHECK_RC(keel__r3_range_edges(b, edges, 8, &count), KEEL_E_IO);
    CHECK_EQ(count, 1);
    fake_push_event(UINT64_C(6300000000), 1, 8);
    CHECK_RC(keel__r3_range_edges(b, edges, 8, &count), KEEL_OK);

    /* Failures are bare statuses: the core makes them PERIPHERAL. */
    fail_reads(FAKE_ROLE_RANGE, EIO, 1);
    CHECK_RC(keel__r3_range_trigger(b, &echo_high), KEEL_E_IO);
    fail_writes(FAKE_ROLE_RANGE, EIO, 1);
    CHECK_RC(keel__r3_range_trigger(b, &echo_high), KEEL_E_IO);
    CHECK_EQ(fault_of(b).status, KEEL_OK);
    bare_close(b);
}

/* ---- the whole stack ------------------------------------------------------- */

static uint32_t device_state(const keel_device *dev)
{
    uint32_t state = 99;
    CHECK_RC(keel_get_state(dev, &state), KEEL_OK);
    return state;
}

/* Open, arm through the acknowledgement (the fake MCU answers it), drive
 * left forward and right reverse with the real worker thread, disarm and
 * close, all through the public API. */
static void end_to_end(void)
{
    keel_open_report report;
    keel_snapshot snapshot;
    keel_fault fault;
    keel_device *dev;
    uint64_t next = 0, cycle = 0;
    unsigned i, start, writes = 0, after;
    uint64_t seen = 0;

    fake_reset();
    dev = open_ok(KEEL_OPEN_NO_RANGE, &report);
    CHECK_RC(keel_now_us(dev, &next), KEEL_OK);
    CHECK_RC(keel_request_arm(dev), KEEL_OK);
    while (device_state(dev) != KEEL_STATE_ARMED) {
        CHECK(cycle < 300);
        next += 10000;
        CHECK_RC(keel_tick(dev, ++cycle, KEEL_HEALTH_ALL), KEEL_OK);
        CHECK_RC(keel_sleep_until(dev, next), KEEL_OK);
    }
    fake_lock();
    start = keel_fake.log_count;
    fake_unlock();
    for (i = 0; i < 20; ++i) {
        next += 10000;
        CHECK_RC(keel_tick(dev, ++cycle, KEEL_HEALTH_ALL), KEEL_OK);
        CHECK_RC(keel_set_motors(dev, 500, -500), KEEL_OK);
        CHECK_RC(keel_sleep_until(dev, next), KEEL_OK);
    }
    /* Left forward is BCM11 and right reverse BCM9, switching at 100 Hz;
     * the other input of each bridge never rose. */
    fake_lock();
    for (i = start; i < keel_fake.log_count; ++i) {
        if (keel_fake.log[i].role == FAKE_ROLE_MOTOR) {
            seen |= keel_fake.log[i].bits;
            writes++;
        }
    }
    fake_unlock();
    CHECK_EQ(seen, KEEL__PWM_LN | KEEL__PWM_RN);
    CHECK(writes >= 20);
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.struct_size = sizeof snapshot;
    CHECK_RC(keel_get_snapshot(dev, &snapshot), KEEL_OK);
    CHECK_EQ(snapshot.output_left, 500);
    CHECK_EQ(snapshot.output_right, -500);
    CHECK_EQ(snapshot.timing_class, KEEL_TIMING_BEST_EFFORT);

    CHECK_RC(keel_disarm(dev), KEEL_OK);
    CHECK_EQ(device_state(dev), KEEL_STATE_DISARMED);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 1u, 0);
    CHECK_EQ(fake_values(FAKE_ROLE_MOTOR), 0);
    /* No motor edge follows the stop. */
    fake_lock();
    after = keel_fake.log_count;
    fake_unlock();
    CHECK_RC(keel_now_us(dev, &next), KEEL_OK);
    CHECK_RC(keel_sleep_until(dev, next + 30000), KEEL_OK);
    fake_lock();
    for (i = after; i < keel_fake.log_count; ++i) {
        CHECK(keel_fake.log[i].role != FAKE_ROLE_MOTOR || keel_fake.log[i].bits == 0);
    }
    fake_unlock();
    /* Button LEDs share the control request: only masked bits change, and
     * an empty mask is a harmless no-op. */
    CHECK_RC(keel_set_button_leds(dev, KEEL_BUTTON_X, KEEL_BUTTON_ALL), KEEL_OK);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL) & 0x78u, 1u << 5); /* LED X is BCM17, bit 5 */
    CHECK_RC(keel_set_button_leds(dev, 0, KEEL_BUTTON_ALL), KEEL_OK);
    CHECK_EQ(device_state(dev), KEEL_STATE_DISARMED);
    memset(&fault, 0, sizeof fault);
    fault.struct_size = sizeof fault;
    CHECK_RC(keel_close(dev, &fault), KEEL_OK);
    CHECK_EQ(fault.status, KEEL_OK);
    CHECK_EQ(fake_open_fds(), 0);
    CHECK_EQ(fake_values(FAKE_ROLE_CONTROL), 0);
    no_violation();
}

/* A forked child closes its copies of the descriptors; the parent keeps its
 * lines. */
static void forked_child(void)
{
    keel_open_report report;
    keel_device *dev;
    unsigned before;
    pid_t pid;
    int status = 0;
    fake_reset();
    dev = open_ok(0, &report);
    before = fake_open_fds();
    CHECK_EQ(before, 5);
    pid = fork();
    if (pid == 0) {
        _exit(fake_open_fds() == 0 && !keel_fake.violation ? 0 : 1);
    }
    CHECK(pid > 0);
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK_EQ(fake_open_fds(), before);
    close_ok(dev);
}

typedef struct hammer {
    keel__r3 *b;
    atomic_int stop;
} hammer;

static void *hammer_main(void *argument)
{
    hammer *h = argument;
    uint32_t level = 0;
    while (!atomic_load(&h->stop)) {
        level ^= KEEL_OUT_HEARTBEAT;
        (void)keel__r3_write_outputs(h->b, KEEL_OUT_HEARTBEAT, level, 0);
    }
    return NULL;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* The stop's wait under hostile load. ThreadSanitizer slows every lock
 * operation, so it gets a looser p99 bound; the starvation this case guards
 * against misses either by more than an order of magnitude. */
#if defined(__SANITIZE_THREAD__)
#define FAIRNESS_P99_US 10000u
#else
#define FAIRNESS_P99_US 2000u
#endif
#define FAIRNESS_P50_US 1000u

/* One round: emergency_disable while three threads keep the backend lock
 * busy all the time with writes that each hold it for 20 us. Passes when
 * p50 and p99 are within bounds; p99.9 is reported, as it measures host
 * preemption of a lock holder more than the lock. */
static int fairness_round(keel__r3 *b, uint32_t *epoch)
{
    enum { THREADS = 3, SAMPLES = 1000 };
    static uint64_t latency[SAMPLES];
    pthread_t threads[THREADS];
    hammer h;
    struct timespec gap = {0, 300000};
    unsigned i;
    h.b = b;
    atomic_init(&h.stop, 0);
    for (i = 0; i < THREADS; ++i) {
        CHECK(pthread_create(&threads[i], NULL, hammer_main, &h) == 0);
    }
    for (i = 0; i < SAMPLES; ++i) {
        uint64_t start = fake_real_now_us();
        CHECK_RC(keel__r3_emergency_disable(b, ++*epoch), KEEL_OK);
        latency[i] = fake_real_now_us() - start;
        nanosleep(&gap, NULL);
    }
    atomic_store(&h.stop, 1);
    for (i = 0; i < THREADS; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    qsort(latency, SAMPLES, sizeof latency[0], compare_u64);
    printf("emergency_disable under contention: p50 %llu us, p99 %llu us, p99.9 %llu us, "
           "max %llu us\n",
           (unsigned long long)latency[SAMPLES / 2], (unsigned long long)latency[SAMPLES * 99 / 100],
           (unsigned long long)latency[SAMPLES - 2], (unsigned long long)latency[SAMPLES - 1]);
    return latency[SAMPLES / 2] < FAIRNESS_P50_US && latency[SAMPLES * 99 / 100] < FAIRNESS_P99_US;
}

/* SPEC 8 rule 6: threads hammering the backend lock do not hold a stop's
 * ENABLE write back, because the PI lock hands over to the waiter (p50 about
 * 0.12 ms here). Measured with a plain mutex instead, the same load kept
 * emergency_disable waiting 15-60 ms at p50 and 0.4-1.1 s at p99.9. Up to
 * three rounds, so that one host or VM pause cannot fail the case while a
 * design fault shows in all of them. */
static void stop_fairness(void)
{
    keel__r3 *b;
    uint32_t epoch = 0;
    int round, passed = 0;
    fake_reset();
    b = bare_open(KEEL_OPEN_NO_RANGE);
    keel_fake.set_delay_ns = 20000;
    for (round = 0; round < 3 && !passed; ++round) {
        passed = fairness_round(b, &epoch);
    }
    keel_fake.set_delay_ns = 0;
    CHECK(passed);
    bare_close(b);
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"requests", requests},
        {"never_requested", never_requested},
        {"chip_discovery", chip_discovery},
        {"errno_mapping", errno_mapping},
        {"line_in_use", line_in_use},
        {"interlocks", interlocks},
        {"stop_epoch", stop_epoch},
        {"backend_latch", backend_latch},
        {"realtime_fallback", realtime_fallback},
        {"lateness_budget", lateness_budget},
        {"stall", stall},
        {"worker_faults", worker_faults},
        {"i2c", i2c},
        {"range", range},
        {"end_to_end", end_to_end},
        {"fork_child", forked_child},
        {"stop_fairness", stop_fairness},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
