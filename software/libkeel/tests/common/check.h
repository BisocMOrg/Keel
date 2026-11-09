/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file check.h
 * @brief Minimal test harness: CHECK macros that report the failing
 *        expression (and status names) and exit, plus a case dispatcher so
 *        that CTest registers every case of a program individually.
 */
#ifndef KEEL_TEST_CHECK_H
#define KEEL_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keel/keel.h"

/** @brief Fails the test unless @p expr holds. */
#define CHECK(expr)                                                                          \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);         \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

/** @brief Fails the test unless @p call returns the status @p expected. */
#define CHECK_RC(call, expected)                                                             \
    do {                                                                                     \
        int32_t check_rc_ = (call);                                                          \
        int32_t check_expected_ = (expected);                                                \
        if (check_rc_ != check_expected_) {                                                  \
            fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__, #call, \
                    keel_status_name(check_rc_), keel_status_name(check_expected_));         \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

/** @brief Fails the test unless two unsigned values are equal (prints both). */
#define CHECK_EQ(actual, expected)                                                           \
    do {                                                                                     \
        unsigned long long check_a_ = (unsigned long long)(actual);                          \
        unsigned long long check_e_ = (unsigned long long)(expected);                        \
        if (check_a_ != check_e_) {                                                          \
            fprintf(stderr, "%s:%d: %s is %llu, expected %llu\n", __FILE__, __LINE__, #actual, \
                    check_a_, check_e_);                                                     \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

/** @brief One named test case. */
typedef struct test_case {
    const char *name;  /**< Name used on the command line and in CTest. */
    void (*run)(void); /**< Body; returns on success, exits on failure. */
} test_case;

/**
 * @brief Runs the case named by argv[1], or every case without an argument.
 * @return Process exit status.
 */
static inline int run_cases(const test_case *cases, size_t count, int argc, char **argv)
{
    size_t i;
    int found = 0;
    for (i = 0; i < count; ++i) {
        if (argc < 2 || strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            printf("PASS %s\n", cases[i].name);
            found = 1;
        }
    }
    if (!found) {
        fprintf(stderr, "unknown test case '%s'\n", argv[1]);
        return 2;
    }
    return 0;
}

#endif /* KEEL_TEST_CHECK_H */
