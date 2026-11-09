/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel-doctor's GPIO checks against the fake kernel: the chip is found by
 * label, busy lines are named with their consumers and a fitting remedy,
 * and the doctor stays read-only (it never requests a line). The checks
 * that read files are covered by check_tools.sh with fake system trees.
 */
#define _GNU_SOURCE

#include <grp.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "fake_syscalls.h"

int keel_doctor_main(int argc, char **argv);

#define main keel_doctor_main
#include "keel-doctor.c"
#undef main

#undef open
#undef close
#undef ioctl
#undef fcntl
#undef read
#undef clock_gettime
#undef pthread_create

#include "check.h"

static char output[65536];

/* Runs the doctor with --json, capturing its standard output. */
static int run_doctor(void)
{
    char program[] = "keel-doctor", json[] = "--json";
    char *argv[] = {program, json, NULL};
    FILE *capture = tmpfile();
    int saved, rc;
    size_t length;
    CHECK(capture != NULL);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    CHECK(saved >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    rc = keel_doctor_main(2, argv);
    fflush(stdout);
    CHECK(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    rewind(capture);
    length = fread(output, 1, sizeof output - 1, capture);
    output[length] = '\0';
    fclose(capture);
    /* Read-only: no line was ever requested, every descriptor was closed. */
    CHECK_EQ(keel_fake.request_count, 0);
    CHECK_EQ(fake_open_fds(), 0);
    if (keel_fake.violation) {
        fprintf(stderr, "fake kernel contract violation: %s\n", keel_fake.violation);
        exit(1);
    }
    return rc;
}

/* The JSON line of one check. */
static const char *check_line(const char *check)
{
    static char line[2048];
    char key[64];
    const char *start, *end;
    size_t length;
    snprintf(key, sizeof key, "\"check\":\"%s\"", check);
    start = strstr(output, key);
    CHECK(start != NULL);
    while (start > output && start[-1] != '\n') {
        start--;
    }
    end = strchr(start, '\n');
    length = end ? (size_t)(end - start) : strlen(start);
    CHECK(length < sizeof line);
    memcpy(line, start, length);
    line[length] = '\0';
    return line;
}

static void expect(const char *check, const char *status, const char *part)
{
    const char *line = check_line(check);
    char key[64];
    snprintf(key, sizeof key, "\"status\":\"%s\"", status);
    if (!strstr(line, key) || (part && !strstr(line, part))) {
        fprintf(stderr, "unexpected %s check: %s\n", check, line);
        exit(1);
    }
}

static void free_lines(void)
{
    uint64_t lines = 0;
    size_t i;
    fake_reset();
    run_doctor();
    expect("gpio_chip", "PASS", "/dev/gpiochip0 is pinctrl-bcm2711 (58 lines)");
    expect("gpio_lines", "PASS", "all 19 robot lines are free");
    /* Exactly the robot's lines were inspected. */
    for (i = 0; i < sizeof robot_lines / sizeof robot_lines[0]; ++i) {
        lines |= UINT64_C(1) << robot_lines[i];
    }
    CHECK_EQ(keel_fake.queried, lines);
    /* The SCHED_FIFO probe reports a refusal as a warning with a remedy. */
    expect("sched_fifo", "WARN", "rtprio 40");
}

static void used_lines(void)
{
    fake_reset();
    keel_fake.line_used[8] = 1;
    snprintf(keel_fake.line_consumer[8], sizeof keel_fake.line_consumer[8], "spi0 CS0");
    keel_fake.line_used[9] = 1;
    keel_fake.line_used[20] = 1;
    snprintf(keel_fake.line_consumer[20], sizeof keel_fake.line_consumer[20], "gpio-shutdown");
    CHECK_EQ(run_doctor(), 1);
    expect("gpio_lines", "FAIL", "BCM8 by \\\"spi0 CS0\\\"");
    expect("gpio_lines", "FAIL", "BCM9 by a kernel function");
    expect("gpio_lines", "FAIL", "BCM20 by \\\"gpio-shutdown\\\"");
    expect("gpio_lines", "FAIL", "spi=off");

    fake_reset();
    keel_fake.line_used[26] = 1;
    snprintf(keel_fake.line_consumer[26], sizeof keel_fake.line_consumer[26], "keel");
    run_doctor();
    expect("gpio_lines", "FAIL", "another keel program");
}

static void chips(void)
{
    fake_reset();
    snprintf(keel_fake.chips[0].label, sizeof keel_fake.chips[0].label, "pinctrl-rp1");
    CHECK_EQ(run_doctor(), 1);
    expect("gpio_chip", "FAIL", "pinctrl-rp1");
    expect("gpio_lines", "SKIP", NULL);

    fake_reset();
    keel_fake.chips[0].open_errno = EACCES;
    run_doctor();
    expect("gpio_chip", "FAIL", "no permission to open /dev/gpiochip0");
    expect("gpio_chip", "FAIL", "keel-setup");

    /* The udev link is found after the scan, as the library finds it. */
    fake_reset();
    snprintf(keel_fake.chips[0].path, sizeof keel_fake.chips[0].path, "/dev/keel/gpiochip");
    run_doctor();
    expect("gpio_chip", "PASS", "/dev/keel/gpiochip");
}

int main(int argc, char **argv)
{
    static const test_case cases[] = {
        {"free_lines", free_lines},
        {"used_lines", used_lines},
        {"chips", chips},
    };
    return run_cases(cases, sizeof cases / sizeof cases[0], argc, argv);
}
