/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file tool_common.h
 * @brief Helpers shared by keel-live, keel-demo and keel-doctor: argument
 *        parsing, JSON string output, open options and reports, signal
 *        handling and names for states. The tools use only the public
 *        library (keel.h).
 */
#ifndef KEEL_TOOL_COMMON_H
#define KEEL_TOOL_COMMON_H

#include <signal.h>
#include <stdint.h>
#include <stdio.h>

#include "keel/keel.h"

/** @brief Number of the signal that asked the tool to stop; 0 while none did. */
extern volatile sig_atomic_t tool_signal;

/**
 * @brief Installs handlers for SIGINT, SIGTERM, SIGHUP and SIGQUIT that only
 *        record the signal in ::tool_signal, and ignores SIGPIPE. The tools'
 *        loops see the flag and stop the robot from their own thread: the
 *        library is not async-signal-safe.
 * @return 0, or -1 if a handler could not be installed.
 */
int tool_install_signals(void);

/**
 * @brief Parses a whole decimal integer within a range.
 * @param[in]  text    Text to parse.
 * @param[in]  minimum Smallest accepted value.
 * @param[in]  maximum Largest accepted value.
 * @param[out] out     Receives the value.
 * @return 1 on success, 0 if @p text is not such an integer.
 */
int tool_parse_int(const char *text, long minimum, long maximum, int32_t *out);

/**
 * @brief Writes @p text as a JSON string (quotes and escapes included).
 * @param[in] out  Stream.
 * @param[in] text NUL-terminated UTF-8 text.
 */
void tool_json_string(FILE *out, const char *text);

/**
 * @brief Name of a ::keel_state value.
 * @param[in] state State.
 * @return Static name such as "ARMED"; "UNKNOWN" for other values.
 */
const char *tool_state_name(uint32_t state);

/**
 * @brief Full-size open options for the keel_r3 backend or the simulator.
 * @param[in] simulator Non-zero: the simulator on the real-time clock.
 * @param[in] gpiochip  GPIO chip path, or NULL for discovery by label.
 * @param[in] i2c       I2C adapter path, or NULL for /dev/i2c-1.
 * @return Options ready for keel_open().
 */
keel_open_options tool_options(int simulator, const char *gpiochip, const char *i2c);

/**
 * @brief Prints an open report as human-readable lines ("keel_open: ...").
 * @param[in] out    Stream.
 * @param[in] report Report filled by keel_open().
 */
void tool_print_report(FILE *out, const keel_open_report *report);

/**
 * @brief Reads the device's fault record into @p out (zeroed on failure).
 * @param[in]  device Handle.
 * @param[out] out    Fault record.
 */
void tool_get_fault(const keel_device *device, keel_fault *out);

#endif /* KEEL_TOOL_COMMON_H */
