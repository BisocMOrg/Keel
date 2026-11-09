/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel-doctor: read-only checks of a robot's software setup (SPEC 22), each
 * with a remedy when it does not pass. It never requests a GPIO line and
 * never writes to I2C: the GPIO chip is opened read-only for line
 * information only, so it is safe to run while a robot program is running.
 *
 * Text by default, JSON Lines with --json. --root DIR reads /proc, /sys,
 * /dev and the device tree below DIR instead of /, for tests and for
 * inspecting a mounted image; the process checks (groups, real-time limit
 * and the SCHED_FIFO probe) describe the running user. --user NAME (for
 * install.sh, which runs as root) judges node access, groups and the
 * real-time limit for that account from the user and group databases
 * instead, as they will apply after its next login.
 * Exit status: 0 when nothing failed (warnings allowed), 1 otherwise, 2 usage.
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
/* tests/tools/test_doctor.c includes this file after choosing _GNU_SOURCE;
 * a second, different definition is an error under -Werror (glibc 2.44, as on
 * Ubuntu 26.10, defines _POSIX_C_SOURCE as 202405L there). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/gpio.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tool_common.h"

enum {
    CHIP_SCAN = 64,     /* /dev/gpiochip0..63, then /dev/keel/gpiochip */
    CHIP_LINES_MIN = 28,
    TEXT_MAX = 512,
    PATH_BUFFER = 256 /* a root prefix plus a device path */
};

/* The 19 lines keel_r3 requests (SPEC 17.2). */
static const uint32_t robot_lines[] = {26, 18, 19, 23, 22, 17, 27, 8, 11, 10,
                                       9,  20, 21, 5,  6,  16, 24, 13, 25};

static const char setup_remedy[] =
    "run sudo keel-setup --user $USER, then log out and in again";

/* The account named by --user, as the databases describe it. */
typedef struct account {
    char name[64];
    uid_t uid;
    gid_t gid; /* primary group */
} account;

typedef struct doctor {
    const char *root;
    int json;
    unsigned passed, warnings, failed, skipped;
    int chip_fd;
    char chip_path[PATH_BUFFER];
    int have_account; /* --user given: judge that account, not this process */
    account who;
} doctor;

static void report(doctor *d, const char *check, const char *status, const char *detail,
                   const char *remedy)
{
    int ok = strcmp(status, "PASS") == 0;
    if (ok) {
        d->passed++;
    } else if (strcmp(status, "WARN") == 0) {
        d->warnings++;
    } else if (strcmp(status, "FAIL") == 0) {
        d->failed++;
    } else {
        d->skipped++;
    }
    if (d->json) {
        fputs("{\"type\":\"check\",\"check\":", stdout);
        tool_json_string(stdout, check);
        fputs(",\"status\":", stdout);
        tool_json_string(stdout, status);
        fputs(",\"detail\":", stdout);
        tool_json_string(stdout, detail);
        fputs(",\"remedy\":", stdout);
        tool_json_string(stdout, ok ? "" : remedy);
        fputs("}\n", stdout);
    } else {
        printf("%-4s  %-12s %s\n", status, check, detail);
        if (!ok && remedy[0]) {
            printf("      %-12s remedy: %s\n", "", remedy);
        }
    }
}

static void path_of(const doctor *d, const char *suffix, char *out, size_t size)
{
    snprintf(out, size, "%s%s", d->root, suffix);
}

static int exists(const doctor *d, const char *suffix)
{
    char path[PATH_BUFFER];
    struct stat st;
    path_of(d, suffix, path, sizeof path);
    return stat(path, &st) == 0;
}

/* Reads a small text or device-tree file; NULs between device-tree strings
 * become commas and trailing white space is dropped. Returns the length,
 * or -1. */
static int read_text(const doctor *d, const char *suffix, char *out, size_t size)
{
    char path[PATH_BUFFER];
    size_t length, i;
    FILE *file;
    path_of(d, suffix, path, sizeof path);
    file = fopen(path, "re");
    if (!file) {
        return -1;
    }
    length = fread(out, 1, size - 1, file);
    fclose(file);
    while (length && (out[length - 1] == '\0' || out[length - 1] == '\n' ||
                      out[length - 1] == ' ')) {
        length--;
    }
    for (i = 0; i < length; ++i) {
        if (out[i] == '\0') {
            out[i] = ',';
        }
    }
    out[length] = '\0';
    return (int)length;
}

/* Group membership from the group database: the primary group or a listed
 * member. Supplementary groups of this process play no part. */
static int in_group(const account *who, const struct group *group)
{
    char **member;
    if (!group) {
        return 0;
    }
    if (group->gr_gid == who->gid) {
        return 1;
    }
    for (member = group->gr_mem; *member; ++member) {
        if (strcmp(*member, who->name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Read-write access to a node for the account, from its mode bits (as the
 * udev rule sets them; ACLs are not consulted). */
static int account_can_rw(const account *who, const struct stat *st)
{
    mode_t need;
    if (who->uid == 0) {
        return 1;
    }
    if (st->st_uid == who->uid) {
        need = S_IRUSR | S_IWUSR;
    } else if (in_group(who, getgrgid(st->st_gid))) {
        need = S_IRGRP | S_IWGRP;
    } else {
        need = S_IROTH | S_IWOTH;
    }
    return (st->st_mode & need) == need;
}

/* Read-write access to @p path for the account or, without --user, for
 * this process. */
static int can_rw(const doctor *d, const char *path)
{
    struct stat st;
    if (!d->have_account) {
        return access(path, R_OK | W_OK) == 0;
    }
    return stat(path, &st) == 0 && account_can_rw(&d->who, &st);
}

/* ---- checks --------------------------------------------------------------- */

/* The library must really reach keel_r3: an open of a path that is no GPIO
 * chip fails inside the backend (resource GPIO_CHIP), not before it
 * (resource BACKEND). Nothing is requested; /dev/null only answers ENOTTY. */
static void check_library(doctor *d)
{
    const char *version = keel_version_string();
    keel_open_options options;
    keel_open_report outcome;
    keel_device *device = NULL;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    options.backend = KEEL_BACKEND_R3;
    snprintf(options.gpiochip_path, sizeof options.gpiochip_path, "/dev/null");
    memset(&outcome, 0, sizeof outcome);
    outcome.struct_size = sizeof outcome;
    if ((keel_abi_version() >> 16) != (KEEL_ABI_VERSION >> 16)) {
        report(d, "library", "FAIL", version, "install a libkeel1 matching keel-tools");
        return;
    }
    if (keel_open(&options, &device, &outcome) == KEEL_OK) {
        (void)keel_close(device, NULL); /* not reached: /dev/null is no chip */
    }
    if (outcome.resource == KEEL_RESOURCE_GPIO_CHIP) {
        report(d, "library", "PASS", version, "");
    } else {
        report(d, "library", "WARN", version,
               "this libkeel cannot open the keel_r3 backend; install the libkeel1 package "
               "built for the robot");
    }
}

static void check_platform(doctor *d)
{
    char model[TEXT_MAX], compatible[TEXT_MAX], detail[TEXT_MAX + 64];
    static const char remedy[] =
        "keel_r3 needs a Raspberry Pi 4, Pi 400 or CM4 (BCM2711); elsewhere use the simulator "
        "(KEEL_BACKEND=simulator)";
    int have_model = read_text(d, "/proc/device-tree/model", model, sizeof model) > 0;
    int have_compatible =
        read_text(d, "/proc/device-tree/compatible", compatible, sizeof compatible) > 0;
    if (have_compatible && strstr(compatible, "brcm,bcm2711")) {
        snprintf(detail, sizeof detail, "%s (BCM2711)", have_model ? model : "BCM2711 board");
        report(d, "platform", "PASS", detail, "");
    } else if (have_model) {
        snprintf(detail, sizeof detail, "%s is not a BCM2711 board", model);
        report(d, "platform", "FAIL", detail, remedy);
    } else {
        report(d, "platform", "FAIL", "no device-tree model: not a Raspberry Pi", remedy);
    }
}

static void check_kernel(doctor *d)
{
    char release[TEXT_MAX], detail[TEXT_MAX + 64];
    unsigned major = 0, minor = 0;
    if (read_text(d, "/proc/sys/kernel/osrelease", release, sizeof release) <= 0 ||
        sscanf(release, "%u.%u", &major, &minor) != 2) {
        report(d, "kernel", "SKIP", "the kernel release is unreadable", "");
        return;
    }
    snprintf(detail, sizeof detail, "Linux %s", release);
    if (major > 5 || (major == 5 && minor >= 10)) {
        report(d, "kernel", "PASS", detail, "");
    } else {
        report(d, "kernel", "FAIL", detail,
               "upgrade to Linux 5.10 or later (GPIO character device v2)");
    }
}

/* Finds the BCM2711 chip as the library does and keeps it open read-only. */
static void check_gpio_chip(doctor *d)
{
    char path[PATH_BUFFER], detail[PATH_BUFFER * 2 + 64];
    char denied[PATH_BUFFER] = "", other[PATH_BUFFER + 48] = "", bogus[PATH_BUFFER] = "";
    struct gpiochip_info info;
    unsigned n;
    for (n = 0; n <= CHIP_SCAN; ++n) {
        char suffix[32];
        int fd;
        if (n < CHIP_SCAN) {
            snprintf(suffix, sizeof suffix, "/dev/gpiochip%u", n);
        } else {
            snprintf(suffix, sizeof suffix, "/dev/keel/gpiochip");
        }
        path_of(d, suffix, path, sizeof path);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if ((errno == EACCES || errno == EPERM) && !denied[0]) {
                snprintf(denied, sizeof denied, "%s", path);
            }
            continue;
        }
        memset(&info, 0, sizeof info);
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
            if (!bogus[0]) {
                snprintf(bogus, sizeof bogus, "%s", path);
            }
            close(fd);
            continue;
        }
        info.label[sizeof info.label - 1] = '\0';
        if (strcmp(info.label, "pinctrl-bcm2711") == 0 && info.lines >= CHIP_LINES_MIN) {
            d->chip_fd = fd;
            snprintf(d->chip_path, sizeof d->chip_path, "%s", path);
            snprintf(detail, sizeof detail, "%s is pinctrl-bcm2711 (%u lines)", path,
                     (unsigned)info.lines);
            report(d, "gpio_chip", "PASS", detail, "");
            return;
        }
        if (!other[0]) {
            snprintf(other, sizeof other, "%s is \"%s\"", path, info.label);
        }
        close(fd);
    }
    if (denied[0]) {
        snprintf(d->chip_path, sizeof d->chip_path, "%s", denied);
        snprintf(detail, sizeof detail, "no permission to open %s", denied);
        report(d, "gpio_chip", "FAIL", detail, setup_remedy);
    } else if (other[0]) {
        snprintf(detail, sizeof detail, "no pinctrl-bcm2711 chip; %s", other);
        report(d, "gpio_chip", "FAIL", detail,
               "keel_r3 needs the Raspberry Pi 4 GPIO chip (a Pi 4, Pi 400 or CM4)");
    } else if (bogus[0]) {
        snprintf(detail, sizeof detail, "%s is not a GPIO character device", bogus);
        report(d, "gpio_chip", "FAIL", detail, "check the kernel (Linux 5.10 or later) and /dev");
    } else {
        report(d, "gpio_chip", "FAIL", "no /dev/gpiochip* device",
               "keel_r3 needs a Raspberry Pi 4 running Linux 5.10 or later");
    }
}

/* The library opens the chip read-write to request lines. */
static void check_gpio_access(doctor *d)
{
    char detail[PATH_BUFFER + 128];
    struct stat st;
    const struct group *group;
    if (!d->chip_path[0] || stat(d->chip_path, &st) != 0) {
        report(d, "gpio_access", "SKIP", "no GPIO chip to inspect", "");
        return;
    }
    group = getgrgid(st.st_gid);
    snprintf(detail, sizeof detail, "%s: mode %04o, group %s%s%s", d->chip_path,
             (unsigned)(st.st_mode & 07777), group ? group->gr_name : "?",
             d->have_account ? ", for user " : "", d->have_account ? d->who.name : "");
    if (can_rw(d, d->chip_path)) {
        report(d, "gpio_access", "PASS", detail, "");
    } else {
        report(d, "gpio_access", "FAIL", detail, setup_remedy);
    }
}

/* Every line keel_r3 requests must be free; the kernel names the owner. */
static void check_gpio_lines(doctor *d)
{
    char detail[TEXT_MAX * 2], item[96];
    const char *remedy = "";
    size_t i, used = 0;
    detail[0] = '\0';
    if (d->chip_fd < 0) {
        report(d, "gpio_lines", "SKIP", "no GPIO chip to inspect", "");
        return;
    }
    for (i = 0; i < sizeof robot_lines / sizeof robot_lines[0]; ++i) {
        struct gpio_v2_line_info info;
        memset(&info, 0, sizeof info);
        info.offset = robot_lines[i];
        if (ioctl(d->chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, &info) < 0) {
            report(d, "gpio_lines", "FAIL", "the kernel has no GPIO v2 line information",
                   "upgrade to Linux 5.10 or later");
            return;
        }
        if (!(info.flags & GPIO_V2_LINE_FLAG_USED)) {
            continue;
        }
        info.consumer[sizeof info.consumer - 1] = '\0';
        snprintf(item, sizeof item, "%sBCM%u by %s%s%s", used ? ", " : "", (unsigned)info.offset,
                 info.consumer[0] ? "\"" : "a kernel function", info.consumer,
                 info.consumer[0] ? "\"" : "");
        strncat(detail, item, sizeof detail - strlen(detail) - 1);
        used++;
        /* The most specific remedy wins: SPI0, then another keel program. */
        if (info.offset >= 8 && info.offset <= 11 &&
            (!info.consumer[0] || strstr(info.consumer, "spi"))) {
            remedy = "set dtparam=spi=off in /boot/firmware/config.txt (sudo keel-setup) and "
                     "reboot";
        } else if (strcmp(info.consumer, "keel") == 0 && !remedy[0]) {
            remedy = "another keel program has the robot open; stop it first";
        } else if (!remedy[0]) {
            remedy = "remove the overlay or stop the program that claims these lines";
        }
    }
    if (used) {
        char text[TEXT_MAX * 2 + 32];
        snprintf(text, sizeof text, "in use: %s", detail);
        report(d, "gpio_lines", "FAIL", text, remedy);
    } else {
        report(d, "gpio_lines", "PASS", "all 19 robot lines are free", "");
    }
}

static void check_spi(doctor *d)
{
    static const char remedy[] =
        "set dtparam=spi=off in /boot/firmware/config.txt (sudo keel-setup) and reboot";
    char status[64];
    int have_status = read_text(d, "/proc/device-tree/soc/spi@7e204000/status", status,
                                sizeof status) > 0;
    if ((have_status && strncmp(status, "okay", 4) == 0) || exists(d, "/dev/spidev0.0") ||
        exists(d, "/dev/spidev0.1")) {
        report(d, "spi0", "FAIL", "SPI0 is enabled; it claims BCM7-11, which include the motor "
                                  "lines",
               remedy);
    } else if (have_status || exists(d, "/proc/device-tree")) {
        report(d, "spi0", "PASS", "SPI0 is disabled", "");
    } else {
        report(d, "spi0", "SKIP", "no device tree to inspect", "");
    }
}

static void check_i2c(doctor *d)
{
    char path[PATH_BUFFER];
    static const char missing[] =
        "enable I2C with dtparam=i2c_arm=on (sudo keel-setup) and reboot; without it the RGB "
        "LEDs are disabled";
    if (exists(d, "/sys/class/i2c-dev")) {
        report(d, "i2c_dev", "PASS", "the i2c-dev driver is loaded", "");
    } else {
        report(d, "i2c_dev", "WARN", "the i2c-dev driver is not loaded",
               "sudo modprobe i2c-dev (the keel-common package loads it at boot)");
    }
    path_of(d, "/dev/i2c-1", path, sizeof path);
    if (!exists(d, "/dev/i2c-1")) {
        report(d, "i2c_bus", "WARN", "/dev/i2c-1 is missing", missing);
    } else if (!can_rw(d, path)) {
        report(d, "i2c_bus", "WARN", "no read-write access to /dev/i2c-1", setup_remedy);
    } else {
        report(d, "i2c_bus", "PASS", "/dev/i2c-1 is accessible", "");
    }
}

/* --user: the device and real-time groups the account belongs to. */
static void check_account_groups(doctor *d)
{
    static const char *const names[] = {"keel", "gpio", "i2c"};
    char detail[TEXT_MAX];
    /* getgrnam() reuses one buffer: keep only what each lookup decides. */
    int have_keel = getgrnam("keel") != NULL;
    int member = in_group(&d->who, getgrnam("keel"));
    size_t used, i;
    used = (size_t)snprintf(detail, sizeof detail, "user %s, device groups:", d->who.name);
    for (i = 0; i < sizeof names / sizeof names[0] && used < sizeof detail; ++i) {
        if (in_group(&d->who, getgrnam(names[i]))) {
            used += (size_t)snprintf(detail + used, sizeof detail - used, " %s", names[i]);
        }
    }
    if (!have_keel && used < sizeof detail) {
        snprintf(detail + used, sizeof detail - used, "; there is no keel group");
    }
    if (member) {
        report(d, "groups", "PASS", detail, "");
    } else if (!have_keel) {
        report(d, "groups", "WARN", detail,
               "install the keel-common package, then run sudo keel-setup --user NAME");
    } else {
        report(d, "groups", "WARN", detail, "run sudo keel-setup --user NAME");
    }
}

static void check_groups(doctor *d)
{
    gid_t groups[256];
    char detail[TEXT_MAX];
    const struct passwd *user = getpwuid(getuid());
    const struct group *keel = getgrnam("keel");
    int count = getgroups((int)(sizeof groups / sizeof groups[0]), groups), i, member = 0;
    size_t used;
    used = (size_t)snprintf(detail, sizeof detail, "user %s, groups:", user ? user->pw_name : "?");
    for (i = 0; i < count && used < sizeof detail; ++i) {
        const struct group *group = getgrgid(groups[i]);
        used += (size_t)snprintf(detail + used, sizeof detail - used, " %s",
                                 group ? group->gr_name : "?");
        if (keel && groups[i] == keel->gr_gid) {
            member = 1;
        }
    }
    if (keel && (member || getegid() == keel->gr_gid)) {
        report(d, "groups", "PASS", detail, "");
    } else if (!keel) {
        report(d, "groups", "WARN", "there is no keel group",
               "install the keel-common package, then run sudo keel-setup --user $USER");
    } else {
        report(d, "groups", "WARN", detail, setup_remedy);
    }
}

static void *probe_main(void *argument)
{
    return argument;
}

/* --user: the account's limit comes from the keel-common limits file at its
 * next login; this process's limit and scheduler say nothing about it. */
static void check_account_realtime(doctor *d)
{
    char detail[TEXT_MAX];
    int member = in_group(&d->who, getgrnam("keel"));
    int have_limit = exists(d, "/etc/security/limits.d/90-keel-rtprio.conf");
    if (member && have_limit) {
        snprintf(detail, sizeof detail, "%s is in the keel group: rtprio %d after a new login",
                 d->who.name, KEEL_RT_PRIORITY);
        report(d, "rtprio", "PASS", detail, "");
    } else {
        report(d, "rtprio", "WARN",
               have_limit ? "not in the keel group, so no real-time limit"
                          : "no /etc/security/limits.d/90-keel-rtprio.conf",
               "install keel-common and run sudo keel-setup --user NAME; without it motor PWM "
               "runs best effort");
    }
    snprintf(detail, sizeof detail,
             "the SCHED_FIFO probe describes only the calling process; run keel-doctor as %s",
             d->who.name);
    report(d, "sched_fifo", "SKIP", detail, "");
}

static void check_realtime(doctor *d)
{
    struct rlimit limit;
    pthread_attr_t attributes;
    struct sched_param param;
    pthread_t thread;
    char detail[128];
    int error;
    static const char remedy[] =
        "members of the keel group get rtprio 40 after a new login (sudo keel-setup --user "
        "$USER); without it motor PWM runs best effort";
    if (getrlimit(RLIMIT_RTPRIO, &limit) == 0) {
        snprintf(detail, sizeof detail, "RLIMIT_RTPRIO is %llu", (unsigned long long)limit.rlim_cur);
        if (geteuid() == 0 || limit.rlim_cur >= KEEL_RT_PRIORITY) {
            report(d, "rtprio", "PASS", detail, "");
        } else {
            report(d, "rtprio", "WARN", detail, remedy);
        }
    }
    /* A real SCHED_FIFO 40 thread, as the PWM worker asks for; this also
     * catches refusals that the limit does not show (RT group scheduling). */
    pthread_attr_init(&attributes);
    memset(&param, 0, sizeof param);
    param.sched_priority = KEEL_RT_PRIORITY;
    pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attributes, SCHED_FIFO);
    pthread_attr_setschedparam(&attributes, &param);
    error = pthread_create(&thread, &attributes, probe_main, NULL);
    pthread_attr_destroy(&attributes);
    if (error == 0) {
        pthread_join(thread, NULL);
        report(d, "sched_fifo", "PASS", "a SCHED_FIFO 40 thread can be created", "");
    } else {
        snprintf(detail, sizeof detail, "a SCHED_FIFO 40 thread was refused (%s)", strerror(error));
        report(d, "sched_fifo", "WARN", detail, remedy);
    }
}

static void check_cpufreq(doctor *d)
{
    char governor[64], detail[128];
    if (read_text(d, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", governor,
                  sizeof governor) <= 0) {
        report(d, "cpufreq", "SKIP", "no cpufreq governor", "");
        return;
    }
    snprintf(detail, sizeof detail, "governor %s", governor);
    if (strcmp(governor, "powersave") == 0) {
        report(d, "cpufreq", "WARN", detail,
               "powersave holds the CPU at its lowest clock; use ondemand, schedutil or "
               "performance");
    } else {
        report(d, "cpufreq", "PASS", detail, "");
    }
}

/* The firmware's get_throttled bits: 0-3 now, 16-19 since boot
 * (under-voltage, frequency capped, throttled, soft temperature limit). */
static void check_throttling(doctor *d)
{
    char text[64], detail[128];
    static const char remedy[] = "use a 5.1 V, 3 A supply and keep the Pi cool";
    unsigned long value;
    char *end;
    if (read_text(d, "/sys/devices/platform/soc/soc:firmware/get_throttled", text,
                  sizeof text) <= 0) {
        report(d, "throttling", "SKIP", "the firmware's throttling flags are unavailable", "");
        return;
    }
    value = strtoul(text, &end, 16);
    snprintf(detail, sizeof detail, "get_throttled = 0x%lx", value);
    if (value & 0xFul) {
        report(d, "throttling", "WARN", detail, remedy);
    } else if (value & 0xF0000ul) {
        strncat(detail, " (since boot)", sizeof detail - strlen(detail) - 1);
        report(d, "throttling", "WARN", detail, remedy);
    } else {
        report(d, "throttling", "PASS", "no under-voltage or throttling since boot", "");
    }
}

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "Usage: %s [--json] [--root DIR] [--user NAME]\n"
            "Read-only checks of the robot's software setup, each with a remedy.\n"
            "  --json       JSON Lines instead of text\n"
            "  --root DIR   read /proc, /sys and /dev below DIR (tests, mounted images)\n"
            "  --user NAME  judge device access, groups and the real-time limit for NAME\n"
            "               (as after its next login) instead of the calling process\n",
            program);
}

/* Looks NAME up in the user database. */
static int select_account(doctor *d, const char *name)
{
    const struct passwd *entry = getpwnam(name);
    if (!entry || strlen(name) >= sizeof d->who.name) {
        fprintf(stderr, "keel-doctor: no such user: %s\n", name);
        return 0;
    }
    snprintf(d->who.name, sizeof d->who.name, "%s", name);
    d->who.uid = entry->pw_uid;
    d->who.gid = entry->pw_gid;
    d->have_account = 1;
    return 1;
}

int main(int argc, char **argv)
{
    doctor d;
    int i;
    memset(&d, 0, sizeof d);
    d.root = "";
    d.chip_fd = -1;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) {
            d.json = 1;
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            d.root = argv[++i];
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            if (!select_account(&d, argv[++i])) {
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    check_library(&d);
    check_platform(&d);
    check_kernel(&d);
    check_gpio_chip(&d);
    check_gpio_access(&d);
    check_gpio_lines(&d);
    check_spi(&d);
    check_i2c(&d);
    if (d.have_account) {
        check_account_groups(&d);
        check_account_realtime(&d);
    } else {
        check_groups(&d);
        check_realtime(&d);
    }
    check_cpufreq(&d);
    check_throttling(&d);
    if (d.chip_fd >= 0) {
        close(d.chip_fd);
    }
    if (d.json) {
        printf("{\"type\":\"summary\",\"passed\":%u,\"warnings\":%u,\"failed\":%u,\"skipped\":%u}\n",
               d.passed, d.warnings, d.failed, d.skipped);
    } else {
        printf("keel-doctor: %u passed, %u warnings, %u failed, %u skipped\n", d.passed,
               d.warnings, d.failed, d.skipped);
    }
    return d.failed ? 1 : 0;
}
