/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/**
 * @file keel.h
 * @brief libkeel application API, ABI 1.0. This is the only header that C
 *        programs and language bindings (Java FFM, C# LibraryImport, Python
 *        ctypes) consume.
 *
 * libkeel drives the Keel robot baseboard (R3) on a Raspberry Pi 4
 * (BCM2711): a DRV8411 dual motor bridge behind a hardware AND gate, an
 * STM32C011 safety MCU, six RGB LEDs on an IS31FL3218-compatible driver at
 * I2C address 0x54, four buttons with indicator LEDs, the J1 HC-SR04
 * ultrasonic connector and the J5 servo port (not fitted on R3). It
 * implements the host side of the safety-MCU contract: heartbeat, arming
 * acknowledgement, READY/ARMED feedback, servo lease and cooldown. A built-in
 * simulator backend runs the same state machine against the production MCU
 * policy on any Linux PC.
 *
 * @par ABI rules (frozen for every 1.x release)
 * - Every function returns an @c int32_t status (a ::keel_status value)
 *   unless it is a metadata query that cannot fail.
 * - Parameters and struct fields are fixed-width integers, fixed-size
 *   @c char arrays or pointers to them. No @c bool, enum type, @c size_t,
 *   @c long, struct-by-value, callback or pointer inside a struct appears in
 *   the ABI, so one layout description fits aarch64, armhf, x86-64 and i386.
 *   Every struct is padded explicitly: no compiler-inserted padding exists.
 * - Every constant is an enumerator, never a macro, so it survives
 *   <tt>cc -E -P -DKEEL_API= keel.h</tt>. Only the version numbers and
 *   ::KEEL_API are macros. Enum @e types never appear in signatures; values
 *   travel as @c uint32_t or @c int32_t.
 * - Status codes, enumerator values and struct fields are only ever
 *   appended; existing ones never change meaning. Functions are only added
 *   (minor version bump).
 *
 * @par Struct versioning (the Linux copy_struct_from_user rule)
 * Every struct starts with <tt>uint32_t struct_size</tt>, which the caller
 * sets to @c sizeof the struct it was compiled against.
 * - A @c struct_size below the 1.0 size, or above ::KEEL_STRUCT_SIZE_MAX
 *   (an uninitialised field, not a newer caller), is
 *   ::KEEL_E_INVALID_ARGUMENT; the library then reads and writes nothing
 *   past @c struct_size.
 * - Input: a @c struct_size larger than the library knows is accepted only if
 *   every byte past the known size is zero. Otherwise the call returns
 *   ::KEEL_E_UNSUPPORTED and has no effect (keel_open() returns
 *   ::KEEL_E_INVALID_ARGUMENT instead, see there). Reserved input fields must
 *   be zero.
 * - Output: the library writes min(@c struct_size, known size) bytes and
 *   zeroes the rest; @c struct_size itself is never changed. An output struct
 *   is left untouched when the call returns ::KEEL_E_INVALID_ARGUMENT,
 *   ::KEEL_E_INVALID_HANDLE or ::KEEL_E_NOT_OWNER; for every other status it
 *   is fully written. The one exception is keel_open()'s report: once its
 *   own @c struct_size is valid it is written for every status, including
 *   ::KEEL_E_INVALID_ARGUMENT, because it carries the remedy.
 *
 * @par Strings
 * Every string returned by the library is static, NUL-terminated UTF-8, never
 * NULL, must not be freed and is at most ::KEEL_STRING_MAX bytes long
 * including the terminating NUL (bindings may read with that bound). Strings
 * passed in are copied during the call.
 *
 * @par Status classes
 * Every status belongs to exactly one class (keel_status_class()); the class
 * tells a caller, without knowing the individual code, what state the device
 * is in after the call:
 * - ::KEEL_CLASS_OK: the call did what it says.
 * - ::KEEL_CLASS_REJECTED: refused with no effect on the device (the refusal
 *   is recorded in keel_snapshot::last_rejection). The one documented
 *   exception: a motion request refused for its arguments or capability while
 *   ARMED coasts both motors and drops the stored command, so a refused
 *   motion command can never leave the wheels driving. No fault is latched
 *   and the heartbeat continues.
 * - ::KEEL_CLASS_FAULT: the device is latched in ::KEEL_STATE_FAULTED after
 *   the call. Either this call detected a safety-relevant failure and
 *   performed a Category-0 stop, or the device was already latched. The
 *   heartbeat stops, so the MCU removes motor, servo and sensor power.
 *   keel_get_fault() gives the first cause; only keel_clear_fault() leaves
 *   the state.
 * - ::KEEL_CLASS_PERIPHERAL: a non-safety peripheral (RGB LED driver,
 *   ultrasonic sensor) failed. The safety state is unaffected.
 * - ::KEEL_CLASS_SETUP: keel_open() could not create a device.
 * - ::KEEL_CLASS_HANDLE: the handle is not usable by this caller (closed,
 *   stale, forged, or inherited through fork()). Loop helpers stop.
 * Two call-specific rules keep the classes honest: every non-OK result of
 * keel_open() means "no device, see the report", and the status of
 * keel_close() is diagnostic only (bindings log it and never throw).
 *
 * @par Threading: stop from anywhere, go only from the loop
 * Calls on one device are serialized internally; any thread may call any
 * function, subject to these rules:
 * - The control owner is the OS thread of the first successful keel_tick()
 *   or keel_cycle() after keel_open() or keel_clear_fault(). Before that,
 *   every thread counts as the owner.
 * - A keel_tick() or keel_cycle() from another thread fail-stops with
 *   ::KEEL_E_HEALTH (cause ::KEEL_CAUSE_TICK_THREAD_CHANGED).
 * - "Go" calls from another thread return ::KEEL_E_CONFLICT with no effect:
 *   keel_request_arm(), keel_clear_fault(), non-zero keel_set_motors(),
 *   keel_brake(), keel_servo_start() and keel_servo_set_pulse().
 * - "Stop" calls are accepted from every thread in every state:
 *   keel_disarm(), keel_emergency_stop(), keel_set_motors(dev, 0, 0),
 *   keel_servo_end() and keel_close().
 * - keel_range_measure() from the control owner while ARMING, ARMED or SERVO
 *   returns ::KEEL_E_CONFLICT (it would block the loop).
 * - Run the loop on a dedicated OS thread (never a virtual thread, timer pool
 *   or async task): the heartbeat certifies that thread's progress.
 * No call sleeps while holding the control lock. keel_disarm() lowers motor
 * enable before it waits for any lock. Handles are generation-checked tokens:
 * a closed, stale or forged handle yields ::KEEL_E_INVALID_HANDLE, never a
 * memory access, and keel_close() may race other calls safely. A forked child
 * gets ::KEEL_E_NOT_OWNER from every call. The library never calls application
 * code, installs no signal handlers and changes no process-wide scheduling.
 *
 * @par Control loop contract
 * Call keel_tick() or keel_cycle() every 10 ms from the loop that really
 * evaluates the application's control and I/O health. The heartbeat that
 * keeps MCU permission alive is emitted only from that call; no library
 * thread renews it. While armed, plan for a worst-case loop stall of at most
 * ::KEEL_STALL_BUDGET_MS. If the process dies, the MCU removes motor power
 * when the heartbeat window closes: a design bound of about 86 ms after the
 * last heartbeat fall, still to be qualified on hardware.
 *
 * @par Units
 * Times are microseconds (@c _us) or milliseconds (@c _ms) as suffixed;
 * timestamps come from the device clock (keel_now_us()). Motor duty is signed
 * permille (-1000..1000). Distances are millimetres. Colours are 0x00RRGGBB
 * with linear 8-bit channels (no gamma correction).
 */
#ifndef KEEL_KEEL_H
#define KEEL_KEEL_H

#include <stdint.h>

/**
 * @defgroup keel_macros Build macros
 * @brief The only macros in the header: version numbers for @c \#if checks
 *        and the symbol-export decoration.
 * @{
 */

/** @brief Major version of this header; equals the ABI major and the SOVERSION. */
#define KEEL_VERSION_MAJOR 1
/** @brief Minor version of this header; equals the ABI minor (additive changes only). */
#define KEEL_VERSION_MINOR 0
/** @brief Patch version of this header; never affects the ABI. */
#define KEEL_VERSION_PATCH 0

/**
 * @def KEEL_API
 * @brief Decoration placed on every exported function.
 *
 * Define it (for example to nothing) before including this header to
 * override it; <tt>cc -E -P -DKEEL_API= keel.h</tt> gives a preprocessed
 * header that binding generators can parse. @c KEEL_STATIC (set by the CMake
 * target @c Keel::keel_static and by <tt>pkg-config --static keel</tt>)
 * selects the static archive, which needs no decoration.
 */
#ifndef KEEL_API
#if defined(KEEL_STATIC) || !(defined(__GNUC__) || defined(__clang__))
#define KEEL_API
#else
#define KEEL_API __attribute__((visibility("default")))
#endif
#endif

/** @} */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/**
 * @defgroup keel_constants Constants
 * @brief Enumerations. Values travel through the ABI as @c uint32_t or
 *        @c int32_t; the enum types themselves never appear in signatures.
 * @{
 */

/** @brief ABI identification, for comparison with keel_abi_version(). */
enum keel_abi {
    /** ABI major: incompatible changes. Equals the shared-library SOVERSION. */
    KEEL_ABI_MAJOR = KEEL_VERSION_MAJOR,
    /** ABI minor: additive changes (new functions, appended fields and values). */
    KEEL_ABI_MINOR = KEEL_VERSION_MINOR,
    /** Packed value returned by keel_abi_version(): (major << 16) | minor. */
    KEEL_ABI_VERSION = (KEEL_VERSION_MAJOR << 16) | KEEL_VERSION_MINOR
};

/**
 * @brief Status codes returned by every function (as @c int32_t).
 *
 * Zero is success; errors are negative. The class of each code
 * (::keel_status_class) is fixed and given in brackets.
 */
enum keel_status {
    /** [OK] Success. */
    KEEL_OK = 0,
    /** [REJECTED] NULL pointer, @c struct_size below the 1.0 size, non-zero
     *  reserved field, unknown enumerator or flag, or a value out of range. */
    KEEL_E_INVALID_ARGUMENT = -1,
    /** [FAULT] A synchronous control-path I/O operation failed: ENABLE,
     *  heartbeat, servo request or button-LED write, READY/ARMED/button read,
     *  or a motor command written by the calling thread. The OS error is in
     *  keel_fault::os_error. From keel_close(): the final stop could not be
     *  confirmed (diagnostic only; the MCU heartbeat timeout still applies). */
    KEEL_E_IO = -2,
    /** [REJECTED] Not allowed in the current state: motion, brake or servo
     *  start while not ARMED, a servo pulse change without a burst, or
     *  keel_request_arm() within ::KEEL_REARM_HOLDOFF_MS of a fault latch
     *  (::KEEL_SNAP_REARM_HOLDOFF is then set). */
    KEEL_E_STATE = -3,
    /** [FAULT] The application reported a health value other than
     *  ::KEEL_HEALTH_ALL, @c cycle_id did not strictly increase, or a tick
     *  came from a thread other than the control owner. */
    KEEL_E_HEALTH = -4,
    /** [FAULT] Application timing failed while arming, armed or in a servo
     *  burst: a tick gap over ::KEEL_TICK_WINDOW_MS or a heartbeat fall later
     *  than ::KEEL_HEARTBEAT_LATE_MS. */
    KEEL_E_DEADLINE = -5,
    /** [FAULT] READY/ARMED feedback disagrees with the commanded state
     *  (READY lost, ARMED lost or unexpected, early ARMED during the
     *  acknowledgement, servo grant missing, both permits at once). */
    KEEL_E_FEEDBACK = -6,
    /** [REJECTED] Temporarily refused; retry later: servo cooldown running or
     *  motors coasting for less than ::KEEL_SERVO_IDLE_MS; ultrasonic
     *  measurement pending, less than ::KEEL_RANGE_INTERVAL_MS since the last
     *  trigger, or ECHO still HIGH. Never used for safety refusals. */
    KEEL_E_AGAIN = -7,
    /** [SETUP] Memory allocation failed while opening. */
    KEEL_E_NO_MEMORY = -8,
    /** [FAULT] The motor PWM generator failed on its own thread: worker
     *  overrun, sustained lateness, stall, or a motor-line write error inside
     *  the worker. */
    KEEL_E_PWM = -9,
    /** [FAULT] The device is latched FAULTED and nothing was done; or, from
     *  keel_clear_fault(), the recorded cause is sticky. See keel_get_fault().
     *  Also the record status of a keel_emergency_stop() latch. */
    KEEL_E_FAULTED = -10,
    /** [SETUP] A GPIO line is held by another process, another libkeel
     *  device, or a kernel function such as SPI0 (see the open report). */
    KEEL_E_IN_USE = -11,
    /** [SETUP] Permission denied on a device node; see the open report hint
     *  (keel-setup adds the user to the right groups). */
    KEEL_E_PERMISSION = -12,
    /** [SETUP] A required device, kernel interface or backend is missing: no
     *  GPIO chip labelled pinctrl-bcm2711 (not a Pi 4), a kernel without GPIO
     *  character device v2, or a backend not built into this library. */
    KEEL_E_NOT_FOUND = -13,
    /** [REJECTED] Not supported here: a capability this device lacks (see
     *  keel_get_capabilities()), a simulator call on hardware, or an input
     *  struct larger than this library knows with a non-zero tail. */
    KEEL_E_UNSUPPORTED = -14,
    /** [REJECTED] Conflicts with the control owner or an active instruction:
     *  a go call from a thread other than the control owner; motion or brake
     *  during a servo burst; servo start while a motor is driven or braked, a
     *  reversal is pending or a burst is active; arming during a servo burst;
     *  keel_range_measure() from the control owner while ARMING, ARMED or
     *  SERVO. */
    KEEL_E_CONFLICT = -15,
    /** [PERIPHERAL] A non-safety peripheral failed (RGB LED driver I2C or the
     *  ultrasonic lines). The safety state is unchanged. */
    KEEL_E_PERIPHERAL = -16,
    /** [FAULT] Arming did not complete within ::KEEL_ARM_TIMEOUT_MS (READY
     *  never HIGH, or ARMED never rose). */
    KEEL_E_ACK_TIMEOUT = -17,
    /** [HANDLE] NULL, closed, stale or forged device handle; or the device
     *  was closed while this call waited. */
    KEEL_E_INVALID_HANDLE = -18,
    /** [HANDLE] Called from a process that did not open the device (for
     *  example a forked child). Nothing was touched. */
    KEEL_E_NOT_OWNER = -19,
    /** [FAULT] Internal invariant violated: the clock went backwards, a
     *  backend interlock refused a command, or a backend broke its contract.
     *  Indicates a defect; the cause is sticky until close. */
    KEEL_E_INTERNAL = -20,
    /** [SETUP] An I/O operation failed while opening (requesting lines,
     *  driving outputs to their safe levels, starting the PWM worker);
     *  everything acquired was released. See the open report. */
    KEEL_E_SETUP_IO = -21
};

/** @brief Status classes returned by keel_status_class(). */
enum keel_status_class {
    /** Success. */
    KEEL_CLASS_OK = 0,
    /** Refused without effect (see the file overview for the one exception). */
    KEEL_CLASS_REJECTED = 1,
    /** The device is latched FAULTED after the call. */
    KEEL_CLASS_FAULT = 2,
    /** Non-safety peripheral failure; safety state unaffected. */
    KEEL_CLASS_PERIPHERAL = 3,
    /** keel_open() failed; no device exists. */
    KEEL_CLASS_SETUP = 4,
    /** The handle is not usable by this caller; stop using it. */
    KEEL_CLASS_HANDLE = 5
};

/** @brief Coarse safety state reported by keel_get_state(). Values are
 *  stable; the arming sub-phase is a diagnostic (::keel_ack_phase). */
enum keel_state {
    /** Motor permission off (GPIO26 LOW). The heartbeat runs, so sensor power
     *  is available. Initial state after open. */
    KEEL_STATE_DISARMED = 0,
    /** Arming acknowledgement in progress (about 0.6-0.85 s of ticks). */
    KEEL_STATE_ARMING = 1,
    /** Motor permission granted and confirmed by READY = ARMED = 1. */
    KEEL_STATE_ARMED = 2,
    /** Servo burst: motors locked out, J5 powered, ARMED = 0. */
    KEEL_STATE_SERVO = 3,
    /** Latched after a safety-relevant failure or keel_emergency_stop():
     *  outputs LOW and no heartbeat. Only keel_clear_fault() leaves it. */
    KEEL_STATE_FAULTED = 4
};

/** @brief Arming-acknowledgement sub-phase (diagnostic only; reported in
 *  keel_snapshot::ack_phase and keel_fault::ack_phase). */
enum keel_ack_phase {
    /** No acknowledgement in progress. */
    KEEL_ACK_NONE = 0,
    /** GPIO26 LOW, waiting at least ::KEEL_ACK_LEVEL_MS and for READY. */
    KEEL_ACK_LOW1 = 1,
    /** First GPIO26 HIGH level. */
    KEEL_ACK_HIGH1 = 2,
    /** Second GPIO26 LOW level. */
    KEEL_ACK_LOW2 = 3,
    /** Final GPIO26 HIGH level; ARMED is expected after ::KEEL_ACK_LEVEL_MS. */
    KEEL_ACK_HIGH2 = 4
};

/** @brief Health flags passed to keel_tick(). The application asserts each
 *  one only after it has actually checked that condition this cycle. */
enum keel_health {
    /** The control computation of this cycle completed. */
    KEEL_HEALTH_CONTROL_OK = 1,
    /** The application's own I/O (sensors, network, files) is healthy. */
    KEEL_HEALTH_IO_OK = 2,
    /** No unhandled exception or error occurred in this cycle. */
    KEEL_HEALTH_NO_EXCEPTION = 4,
    /** The only accepted value; any other value fail-stops with
     *  ::KEEL_E_HEALTH and records the missing bits. */
    KEEL_HEALTH_ALL = 7
};

/** @brief Button bits (buttons and button LEDs share this layout). */
enum keel_button {
    /** Button A (BCM5, active low; its LED is BCM23). */
    KEEL_BUTTON_A = 1,
    /** Button B (BCM6, active low; its LED is BCM22). */
    KEEL_BUTTON_B = 2,
    /** Button X (BCM16, active low; its LED is BCM17). */
    KEEL_BUTTON_X = 4,
    /** Button Y (BCM24, active low; its LED is BCM27). */
    KEEL_BUTTON_Y = 8,
    /** All four buttons. */
    KEEL_BUTTON_ALL = 15
};

/** @brief RGB LED indices (board designators RGB1..RGB6). */
enum keel_led {
    KEEL_LED_RGB1 = 0, /**< Designator RGB1. */
    KEEL_LED_RGB2 = 1, /**< Designator RGB2. */
    KEEL_LED_RGB3 = 2, /**< Designator RGB3. */
    KEEL_LED_RGB4 = 3, /**< Designator RGB4. */
    KEEL_LED_RGB5 = 4, /**< Designator RGB5. */
    KEEL_LED_RGB6 = 5  /**< Designator RGB6. */
};

/** @brief Safety input bits returned by keel_read_safety_inputs() and
 *  reported in snapshots and fault records. */
enum keel_input {
    /** READY (BCM20): the MCU is healthy and fault-free. */
    KEEL_INPUT_READY = 1,
    /** ARMED (BCM21): the motor-enable gate output is HIGH. */
    KEEL_INPUT_ARMED = 2
};

/** @brief Capability bits returned by keel_get_capabilities(). */
enum keel_capability {
    KEEL_CAP_MOTORS = 1,       /**< Drive and coast. */
    KEEL_CAP_BRAKE = 2,        /**< keel_brake(). */
    /** RGB LEDs. Cleared, without failing keel_open(), when the I2C bus or
     *  the LED driver is unavailable. */
    KEEL_CAP_RGB = 4,
    KEEL_CAP_BUTTONS = 8,      /**< Buttons. */
    KEEL_CAP_BUTTON_LEDS = 16, /**< Button indicator LEDs. */
    /** Servo bursts. Absent on R3 (J5 not fitted) and with
     *  ::KEEL_OPEN_NO_SERVO; present on the simulator. */
    KEEL_CAP_SERVO = 32,
    /** Ultrasonic ranging (absent with ::KEEL_OPEN_NO_RANGE). */
    KEEL_CAP_RANGE = 64,
    /** The device is the simulator; no hardware is touched. */
    KEEL_CAP_SIMULATED = 128,
    /** The motor PWM worker runs at SCHED_FIFO priority ::KEEL_RT_PRIORITY. */
    KEEL_CAP_REALTIME_PWM = 256
};

/** @brief Backend selector for keel_open_options::backend and value of
 *  keel_open_report::backend. */
enum keel_backend {
    /** The environment variable @c KEEL_BACKEND ("r3" or "simulator") if
     *  set, else the keel_r3 backend if it was built in, else
     *  ::KEEL_E_NOT_FOUND. Never falls back to the simulator silently. */
    KEEL_BACKEND_AUTO = 0,
    /** keel_r3: R3 board on a Raspberry Pi 4 (BCM2711, Linux >= 5.10). */
    KEEL_BACKEND_R3 = 1,
    /** In-process simulator: board model plus the production MCU policy. */
    KEEL_BACKEND_SIMULATOR = 2,
    /** Reported only: a backend attached through the C porting interface. */
    KEEL_BACKEND_PORT = 3
};

/** @brief Flags for keel_open_options::flags. Zero enables everything. */
enum keel_open_flag {
    /** Do not offer servo bursts. A servo request line, if any, stays LOW. */
    KEEL_OPEN_NO_SERVO = 1,
    /** Leave the ultrasonic lines (BCM13/BCM25) unclaimed for other software. */
    KEEL_OPEN_NO_RANGE = 2,
    /** Never request real-time priority for the PWM worker. */
    KEEL_OPEN_NO_REALTIME = 4,
    /** Simulator only: time advances only inside keel_sleep_until(), which
     *  makes tests deterministic. Without it the simulator follows real time. */
    KEEL_OPEN_VIRTUAL_CLOCK = 8
};

/** @brief Bits of keel_open_report::warnings: the device opened, degraded. */
enum keel_open_warning {
    /** The I2C bus or LED driver is unavailable; ::KEEL_CAP_RGB is clear and
     *  the report's resource, os_error, path and hint describe why. */
    KEEL_OPEN_WARN_NO_RGB = 1,
    /** SCHED_FIFO was refused; the PWM worker runs best effort.
     *  keel_open_report::realtime_os_error holds the errno. */
    KEEL_OPEN_WARN_BEST_EFFORT_TIMING = 2
};

/** @brief Resource named by keel_open_report::resource. */
enum keel_resource {
    KEEL_RESOURCE_NONE = 0,       /**< No specific resource. */
    KEEL_RESOURCE_GPIO_CHIP = 1,  /**< GPIO character device. */
    KEEL_RESOURCE_GPIO_LINE = 2,  /**< One GPIO line (see keel_open_report::line). */
    KEEL_RESOURCE_I2C_BUS = 3,    /**< I2C adapter for the LED driver. */
    KEEL_RESOURCE_LED_DRIVER = 4, /**< LED driver at 0x54 did not respond. */
    KEEL_RESOURCE_PWM_WORKER = 5, /**< Motor PWM worker thread. */
    KEEL_RESOURCE_MEMORY = 6,     /**< Allocation. */
    KEEL_RESOURCE_BACKEND = 7,    /**< Backend selection or availability. */
    KEEL_RESOURCE_OPTIONS = 8     /**< The options struct itself. */
};

/**
 * @brief Fault causes (keel_fault::cause). Each cause has one fixed status;
 *        the meaning of keel_fault::detail is given per cause.
 */
enum keel_fault_cause {
    /** No fault recorded. */
    KEEL_CAUSE_NONE = 0,
    /** [E_HEALTH] Health flags were not ::KEEL_HEALTH_ALL; detail = missing bits. */
    KEEL_CAUSE_HEALTH_FLAGS = 1,
    /** [E_HEALTH] cycle_id did not increase; detail = rejected cycle_id. */
    KEEL_CAUSE_CYCLE_NOT_INCREASING = 2,
    /** [E_HEALTH] A tick came from a thread other than the control owner;
     *  detail = the calling thread's id. */
    KEEL_CAUSE_TICK_THREAD_CHANGED = 3,
    /** [E_DEADLINE] Tick gap over ::KEEL_TICK_WINDOW_MS while arming, armed
     *  or in a servo burst; detail = gap in us. */
    KEEL_CAUSE_TICK_LATE = 4,
    /** [E_DEADLINE] Heartbeat fall more than ::KEEL_HEARTBEAT_LATE_MS after
     *  the previous one while arming, armed or in a servo burst; detail =
     *  interval in us. */
    KEEL_CAUSE_HEARTBEAT_LATE = 5,
    /** [E_FEEDBACK] READY fell after the first acknowledgement level, while
     *  armed or in a servo burst. */
    KEEL_CAUSE_READY_LOST = 6,
    /** [E_FEEDBACK] ARMED fell while armed. */
    KEEL_CAUSE_ARMED_LOST = 7,
    /** [E_FEEDBACK, sticky] ARMED HIGH more than ::KEEL_ARMED_GRACE_MS after
     *  GPIO26 went LOW; the AND gate no longer removes enable. */
    KEEL_CAUSE_ARMED_UNEXPECTED = 8,
    /** [E_ACK_TIMEOUT] READY never HIGH during the arming window. */
    KEEL_CAUSE_ACK_NO_READY = 9,
    /** [E_ACK_TIMEOUT] ARMED never rose after the final acknowledgement level. */
    KEEL_CAUSE_ACK_NO_ARMED = 10,
    /** [E_FEEDBACK] ARMED rose early during the acknowledgement, or a second
     *  stale completion was seen; detail = ::keel_ack_phase. */
    KEEL_CAUSE_ACK_FEEDBACK = 11,
    /** [E_FEEDBACK] ARMED still HIGH (or READY LOW) ::KEEL_SERVO_GRANT_MS
     *  after the servo request. */
    KEEL_CAUSE_SERVO_NOT_GRANTED = 12,
    /** [E_FEEDBACK] ARMED HIGH during a granted servo burst (both MCU permits). */
    KEEL_CAUSE_DUAL_PERMIT = 13,
    /** [E_PWM] One PWM worker wake more than 30 ms late; detail = lateness in us. */
    KEEL_CAUSE_PWM_OVERRUN = 14,
    /** [E_PWM] PWM excess lateness over 20 ms within 100 ms; detail = excess in us. */
    KEEL_CAUSE_PWM_DEGRADED = 15,
    /** [E_PWM] The PWM worker's scheduled wake is more than 25 ms in the past
     *  while a drive or brake command is unexpired; detail = age in us. */
    KEEL_CAUSE_PWM_STALL = 16,
    /** [E_PWM] A motor-line write inside the PWM worker failed; os_error set. */
    KEEL_CAUSE_PWM_WRITE = 17,
    /** [E_IO] A control output write failed; detail = logical output mask. */
    KEEL_CAUSE_GPIO_WRITE = 18,
    /** [E_IO] A control input read failed. */
    KEEL_CAUSE_GPIO_READ = 19,
    /** [E_IO] A synchronous motor command write failed. */
    KEEL_CAUSE_MOTOR_WRITE = 20,
    /** [E_INTERNAL, sticky] The device clock went backwards or failed;
     *  detail = step back in us. */
    KEEL_CAUSE_CLOCK = 21,
    /** [E_INTERNAL, sticky] A backend interlock refused a conflicting
     *  low-level command (library defect). */
    KEEL_CAUSE_INTERLOCK = 22,
    /** [E_INTERNAL, sticky] A backend returned a status outside the FAULT
     *  class on a safety path, or flagged a fault it could not describe;
     *  detail = the raw status, sign-extended to 64 bits. */
    KEEL_CAUSE_HAL_CONTRACT = 23,
    /** [E_INTERNAL, sticky] Another internal invariant; detail = internal code. */
    KEEL_CAUSE_INTERNAL = 24,
    /** [E_FAULTED] keel_emergency_stop() was called. */
    KEEL_CAUSE_USER_STOP = 25
};

/** @brief Bits of keel_fault::flags. */
enum keel_fault_flag {
    /** keel_clear_fault() has acknowledged this record; the device left
     *  FAULTED. The record stays readable until the next latch. */
    KEEL_FAULT_CLEARED = 1,
    /** The cause is sticky: keel_clear_fault() returns ::KEEL_E_FAULTED until
     *  the device is closed and reopened. */
    KEEL_FAULT_STICKY = 2
};

/** @brief Bits of keel_snapshot::flags. */
enum keel_snapshot_flag {
    /** Heartbeat pulses are being emitted (not FAULTED, ticks on time). */
    KEEL_SNAP_HEARTBEAT_RUNNING = 1,
    /** The last motion command outlived its timeout; motors coast, still ARMED. */
    KEEL_SNAP_COMMAND_EXPIRED = 2,
    /** A direction reversal is coasting through its ::KEEL_REVERSAL_DWELL_MS dwell. */
    KEEL_SNAP_REVERSAL_PENDING = 4,
    /** keel_brake() is in effect. */
    KEEL_SNAP_BRAKING = 8,
    /** The most recent tick restarted the heartbeat after a gap while DISARMED. */
    KEEL_SNAP_HEARTBEAT_RESTARTED = 16,
    /** keel_request_arm() is refused for the re-arm hold-off after a latch. */
    KEEL_SNAP_REARM_HOLDOFF = 32,
    /** The device is the simulator. */
    KEEL_SNAP_SIMULATED = 64,
    /** The servo grant was confirmed and the pulse is running. */
    KEEL_SNAP_SERVO_GRANTED = 128,
    /** The recorded fault is sticky (see ::KEEL_FAULT_STICKY). */
    KEEL_SNAP_FAULT_STICKY = 256
};

/** @brief Inferred ultrasonic sensor power (keel_snapshot::sensor_power).
 *  The Pi has no sensor-power feedback; this is a documented inference. */
enum keel_sensor_power {
    /** Heartbeat running but not yet qualified (fewer than two falls), or
     *  READY LOW for less than 150 ms. */
    KEEL_SENSOR_UNKNOWN = 0,
    /** Heartbeat stopped, or READY LOW for at least 150 ms while it ran. */
    KEEL_SENSOR_OFF = 1,
    /** Heartbeat qualified and READY HIGH: the MCU should be powering the
     *  sensor (an MCU strike lockout is invisible to the Pi). */
    KEEL_SENSOR_PERMITTED = 2,
    /** PERMITTED and an echo rose within the last 500 ms. */
    KEEL_SENSOR_CONFIRMED = 3
};

/** @brief Scheduling class of the motor PWM worker. */
enum keel_timing_class {
    KEEL_TIMING_NONE = 0,        /**< No PWM worker thread (simulator). */
    KEEL_TIMING_BEST_EFFORT = 1, /**< SCHED_OTHER (real-time refused or disabled). */
    KEEL_TIMING_REALTIME = 2     /**< SCHED_FIFO at ::KEEL_RT_PRIORITY. */
};

/** @brief Ultrasonic measurement status (keel_range::status). Only
 *  ::KEEL_RANGE_VALID carries a distance; no status means "path clear".
 *  A zero-initialised result reads as ::KEEL_RANGE_IDLE. */
enum keel_range_status {
    /** No measurement has been started on this device. */
    KEEL_RANGE_IDLE = 0,
    /** Measurement in progress. */
    KEEL_RANGE_PENDING = 1,
    /** distance_mm is valid (::KEEL_RANGE_MIN_MM..::KEEL_RANGE_MAX_MM). */
    KEEL_RANGE_VALID = 2,
    /** Echo rose but no object within 4 m (or the echo was still HIGH at
     *  ::KEEL_RANGE_TIMEOUT_MS). Means "nothing detected", never "clear". */
    KEEL_RANGE_NO_ECHO = 3,
    /** Sensor unpowered (heartbeat stopped or READY LOW, so TRIG was not
     *  driven), or the echo never rose within 5 ms of the trigger (locked out
     *  by the MCU or unplugged). */
    KEEL_RANGE_UNAVAILABLE = 4,
    /** Echo HIGH before the trigger, pulse under 117 us, lost edge events,
     *  or READY fell during the measurement. */
    KEEL_RANGE_INVALID = 5
};

/** @brief Motor command carried by keel_cycle_input::command. */
enum keel_cycle_command {
    /** Leave the stored motor command as it is (it keeps ageing toward its
     *  timeout). */
    KEEL_CYCLE_NONE = 0,
    /** keel_set_motors(left, right) after the tick. */
    KEEL_CYCLE_DRIVE = 1,
    /** keel_brake() after the tick. */
    KEEL_CYCLE_BRAKE = 2
};

/** @brief Bits of keel_cycle_input::flags. */
enum keel_cycle_flag {
    /** Consume button edges: the snapshot's pressed/released masks are taken,
     *  as by keel_take_buttons(). */
    KEEL_CYCLE_TAKE_BUTTONS = 1
};

/** @brief Fixed limits and timing constants of the board contract, as
 *  enumerators so that bindings can read them. */
enum keel_limit {
    KEEL_DUTY_MAX = 1000,                  /**< Maximum |duty|, permille. */
    KEEL_PWM_FREQUENCY_HZ = 100,           /**< Motor PWM frequency. */
    KEEL_TICK_PERIOD_MS = 10,              /**< Recommended tick period. */
    KEEL_TICK_WINDOW_MS = 70,              /**< Maximum tick gap while arming, armed or servo. */
    KEEL_HEARTBEAT_MIN_MS = 30,            /**< Minimum spacing of heartbeat falls (MCU accepts 20-80). */
    KEEL_HEARTBEAT_LATE_MS = 74,           /**< A later fall is a fault while arming, armed or servo. */
    KEEL_HEARTBEAT_PULSE_US = 1000,        /**< Heartbeat HIGH time. */
    KEEL_STALL_BUDGET_MS = 40,             /**< Planning budget for one loop stall while armed. */
    KEEL_COMMAND_TIMEOUT_DEFAULT_MS = 500, /**< Default motion-command lifetime. */
    KEEL_COMMAND_TIMEOUT_MIN_MS = 20,      /**< Smallest configurable lifetime. */
    KEEL_COMMAND_TIMEOUT_MAX_MS = 1000,    /**< Largest configurable lifetime. */
    KEEL_REVERSAL_DWELL_MS = 20,           /**< Idle time before a wheel may reverse. */
    KEEL_ACK_LEVEL_MS = 150,               /**< Hold time of each acknowledgement level. */
    KEEL_ARM_TIMEOUT_MS = 2000,            /**< Arming must complete within this time. */
    KEEL_REARM_HOLDOFF_MS = 1000,          /**< Minimum time from a fault latch to re-arm. */
    KEEL_ARMED_GRACE_MS = 10,              /**< ARMED may lag a GPIO26 fall by this much. */
    KEEL_SERVO_PULSE_MIN_US = 500,         /**< Shortest servo pulse. */
    KEEL_SERVO_PULSE_MAX_US = 2500,        /**< Longest servo pulse. */
    KEEL_SERVO_LEASE_MIN_MS = 20,          /**< Shortest servo burst. */
    KEEL_SERVO_LEASE_MAX_MS = 400,         /**< Longest servo burst (MCU lease is 500 ms). */
    KEEL_SERVO_COOLDOWN_MS = 2200,         /**< Host cooldown (MCU: 2000 ms plus margin). */
    KEEL_SERVO_IDLE_MS = 20,               /**< Motors must coast this long before a burst. */
    KEEL_SERVO_GRANT_MS = 10,              /**< ARMED must fall within this time of a servo request. */
    KEEL_RANGE_MIN_MM = 20,                /**< Smallest reported distance. */
    KEEL_RANGE_MAX_MM = 4000,              /**< Largest reported distance. */
    KEEL_RANGE_INTERVAL_MS = 60,           /**< Minimum spacing between triggers. */
    KEEL_RANGE_TIMEOUT_MS = 30,            /**< A measurement completes within this time. */
    KEEL_RANGE_MEASURE_MAX_MS = 35,        /**< Longest wait inside keel_range_measure(). */
    KEEL_SLEEP_AHEAD_MAX_MS = 10000,       /**< Largest keel_sleep_until() look-ahead. */
    KEEL_RT_PRIORITY = 40,                 /**< SCHED_FIFO priority of the PWM worker. */
    KEEL_LED_COUNT = 6,                    /**< Number of RGB LEDs. */
    KEEL_BUTTON_COUNT = 4,                 /**< Number of buttons. */
    KEEL_PATH_MAX = 128,                   /**< Size of path fields, including NUL. */
    KEEL_CONSUMER_MAX = 32,                /**< Size of keel_open_report::consumer, including NUL. */
    KEEL_HINT_MAX = 256,                   /**< Size of keel_open_report::hint, including NUL. */
    KEEL_STRING_MAX = 256,                 /**< Upper bound of every returned string, including NUL. */
    KEEL_STRUCT_SIZE_MAX = 4096            /**< Largest accepted @c struct_size of any struct. */
};

/** @brief Simulator stimulus items for keel_sim_set(). */
enum keel_sim_item {
    /** value = ::keel_button mask of buttons physically held down. */
    KEEL_SIM_BUTTONS = 1,
    /** value = distance of the nearest object in mm; -1 = nothing within range. */
    KEEL_SIM_RANGE_MM = 2,
    /** value != 0: one hardware-fault-bus pulse (the MCU latches F_EXTERNAL). */
    KEEL_SIM_HARDWARE_FAULT = 3,
    /** value != 0: reset the MCU model (READY drops, permissions removed,
     *  the 2 s servo cooldown restarts). */
    KEEL_SIM_MCU_RESET = 4,
    /** value = number of upcoming control output writes that fail (EIO). */
    KEEL_SIM_FAIL_GPIO_WRITES = 5,
    /** value = number of upcoming control input reads that fail (EIO). */
    KEEL_SIM_FAIL_GPIO_READS = 6,
    /** value = number of upcoming LED driver I2C transactions that fail. */
    KEEL_SIM_FAIL_I2C = 7,
    /** value != 0: MCU sensor strike lockout active (the sensor stays off). */
    KEEL_SIM_SENSOR_LOCKOUT = 8,
    /** value = lateness in us applied to the next PWM generator wake. */
    KEEL_SIM_PWM_LATENESS_US = 9,
    /** value != 0: ARMED reads HIGH whatever the gate does (a board fault). */
    KEEL_SIM_ARMED_STUCK_HIGH = 10,
    /** value = number of upcoming motor-line writes inside the PWM generator
     *  that fail. */
    KEEL_SIM_FAIL_PWM_WRITES = 11,
    /** value != 0: the PWM generator stops making passes (stall). */
    KEEL_SIM_PWM_STALL = 12
};

/** @brief Bits of keel_sim_state::flags. */
enum keel_sim_flag {
    KEEL_SIM_MOTOR_PERMIT = 1,  /**< MCU motor permission output. */
    KEEL_SIM_SERVO_PERMIT = 2,  /**< MCU servo permission output. */
    KEEL_SIM_SENSOR_PERMIT = 4, /**< MCU sensor power output. */
    KEEL_SIM_READY = 8,         /**< MCU READY output. */
    KEEL_SIM_LEFT_BRAKE = 16,   /**< Left bridge inputs both HIGH. */
    KEEL_SIM_RIGHT_BRAKE = 32   /**< Right bridge inputs both HIGH. */
};

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_types Types
 * @brief The opaque handle and the size-versioned structs.
 * @{
 */

/**
 * @brief Opaque device handle.
 *
 * The value is a generation-checked token issued by keel_open(); it is never
 * dereferenced by the application or by the library. Compare handles only
 * for equality. A handle is valid in the opening process until keel_close()
 * returns.
 */
typedef struct keel_device keel_device;

/**
 * @brief Options for keel_open(). Zero-initialise, set @c struct_size, then
 *        change only what you need; every zero field means "default".
 */
typedef struct keel_open_options {
    /** Caller sets sizeof(keel_open_options). */
    uint32_t struct_size;
    /** ::keel_backend value; 0 = ::KEEL_BACKEND_AUTO. ::KEEL_BACKEND_PORT is
     *  not accepted here. */
    uint32_t backend;
    /** Bitwise OR of ::keel_open_flag values. */
    uint32_t flags;
    /** Motion-command lifetime in ms: 0 = ::KEEL_COMMAND_TIMEOUT_DEFAULT_MS,
     *  else ::KEEL_COMMAND_TIMEOUT_MIN_MS..::KEEL_COMMAND_TIMEOUT_MAX_MS. */
    uint32_t command_timeout_ms;
    /** keel_r3 GPIO chip path, NUL-terminated (::KEEL_PATH_MAX bytes); empty
     *  = the /dev/gpiochipN labelled "pinctrl-bcm2711". Must be empty for the
     *  simulator. */
    char gpiochip_path[128];
    /** keel_r3 I2C adapter path, NUL-terminated (::KEEL_PATH_MAX bytes);
     *  empty = /dev/i2c-1. Must be empty for the simulator. */
    char i2c_path[128];
} keel_open_options;

/**
 * @brief Detailed outcome of keel_open(), written on success and failure.
 *        Lets an application show an actionable message such as
 *        "BCM8 is used by spi0: set dtparam=spi=off".
 */
typedef struct keel_open_report {
    /** Caller sets sizeof(keel_open_report). */
    uint32_t struct_size;
    /** Same value keel_open() returned. */
    int32_t status;
    /** ::keel_resource that failed, or on success the first degraded one;
     *  ::KEEL_RESOURCE_NONE if none. */
    uint32_t resource;
    /** errno (Linux) of that resource's failing operation; 0 if none. */
    int32_t os_error;
    /** BCM line number for ::KEEL_RESOURCE_GPIO_LINE, else -1. */
    int32_t line;
    /** Selected ::keel_backend (useful with ::KEEL_BACKEND_AUTO). */
    uint32_t backend;
    /** ::keel_capability bits of the opened device; 0 on failure. */
    uint32_t capabilities;
    /** Bitwise OR of ::keel_open_warning values; 0 when fully functional. */
    uint32_t warnings;
    /** ::keel_timing_class the PWM worker obtained. */
    uint32_t timing_class;
    /** errno of the refused SCHED_FIFO request; 0 if granted or not tried. */
    int32_t realtime_os_error;
    /** Device path involved, or empty (::KEEL_PATH_MAX bytes). */
    char path[128];
    /** Kernel consumer label holding a busy line (e.g. "spi0"), or empty
     *  (::KEEL_CONSUMER_MAX bytes). */
    char consumer[32];
    /** One-line remedy, e.g. "add the user to the gpio group: sudo keel-setup
     *  --user NAME", or empty (::KEEL_HINT_MAX bytes). */
    char hint[256];
} keel_open_report;

/**
 * @brief Fault record: the first fault since the last keel_clear_fault(),
 *        captured before any cleanup. After keel_clear_fault() it stays
 *        readable with ::KEEL_FAULT_CLEARED set until the next latch.
 */
typedef struct keel_fault {
    /** Caller sets sizeof(keel_fault). */
    uint32_t struct_size;
    /** Primary status (FAULT class); ::KEEL_OK when no fault was ever recorded. */
    int32_t status;
    /** ::keel_fault_cause. */
    uint32_t cause;
    /** ::keel_state in which the fault was detected. */
    uint32_t state;
    /** ::keel_ack_phase at detection. */
    uint32_t ack_phase;
    /** ::keel_input bits last sampled before the fault. */
    uint32_t inputs;
    /** errno behind the primary status; 0 if none. */
    int32_t os_error;
    /** Status of the most recent safe-stop attempt for this record (the
     *  latch itself, later keel_disarm() / keel_emergency_stop() / keel_close()
     *  retries); ::KEEL_OK if every stop action succeeded. Never replaces
     *  @c status. */
    int32_t cleanup_status;
    /** Cause-specific value (see ::keel_fault_cause). */
    uint64_t detail;
    /** Device-clock time of detection, us. */
    uint64_t time_us;
    /** Number of latches since open, including this one. */
    uint32_t fault_count;
    /** Bitwise OR of ::keel_fault_flag values. */
    uint32_t flags;
    /** errno of the most recent failed stop action; 0 if none. */
    int32_t cleanup_os_error;
    /** Reserved; written as zero. */
    uint32_t reserved0;
} keel_fault;

/**
 * @brief Cached device state, filled by keel_get_snapshot() and keel_cycle().
 *        Reading it performs no I/O and consumes nothing.
 */
typedef struct keel_snapshot {
    /** Caller sets sizeof(keel_snapshot). */
    uint32_t struct_size;
    /** ::keel_state. */
    uint32_t state;
    /** Bitwise OR of ::keel_snapshot_flag. */
    uint32_t flags;
    /** ::keel_input bits as last sampled. */
    uint32_t inputs;
    /** Device-clock time at which this snapshot was taken, us. */
    uint64_t time_us;
    /** Device-clock time at which @c inputs were sampled, us. */
    uint64_t inputs_sampled_us;
    /** cycle_id of the last accepted tick (0 before the first). */
    uint64_t last_cycle_id;
    /** OS thread id of the control owner; 0 while none is recorded. */
    uint64_t owner_thread_id;
    /** Time left before the tick window closes, us; negative when late.
     *  Meaningful while arming, armed or in a servo burst. */
    int32_t tick_budget_us;
    /** ::keel_ack_phase. */
    uint32_t ack_phase;
    /** Stored left command, permille (0 after a coast, stop or expiry). */
    int32_t command_left;
    /** Stored right command, permille. */
    int32_t command_right;
    /** Left duty actually applied, permille (0 when coasting, braking,
     *  expired or in reversal dwell). */
    int32_t output_left;
    /** Right duty actually applied, permille. */
    int32_t output_right;
    /** Remaining lifetime of the stored motion command, ms (0 if none). */
    uint32_t command_remaining_ms;
    /** Debounced buttons held (::keel_button mask). */
    uint32_t buttons_held;
    /** Press edges not yet taken. */
    uint32_t buttons_pressed;
    /** Release edges not yet taken. */
    uint32_t buttons_released;
    /** Button LEDs currently lit (::keel_button mask). */
    uint32_t button_leds;
    /** ::keel_sensor_power inference. */
    uint32_t sensor_power;
    /** Current servo pulse width, us (0 when no burst). */
    uint32_t servo_pulse_us;
    /** Remaining servo burst lease, ms. */
    uint32_t servo_lease_remaining_ms;
    /** Remaining servo cooldown, ms (0 = a burst may start). */
    uint32_t servo_cooldown_remaining_ms;
    /** ::keel_fault_cause of the current fault record (0 if none). */
    uint32_t fault_cause;
    /** Status of the most recent REJECTED call (0 if none since open). */
    int32_t last_rejection;
    /** Heartbeat restarts after gaps while DISARMED, since open. */
    uint32_t heartbeat_restarts;
    /** Stale-acknowledgement restarts, since open. */
    uint32_t stale_ack_restarts;
    /** ::keel_timing_class of the PWM worker. */
    uint32_t timing_class;
    /** PWM worker wakes more than 1 ms late, since open. */
    uint32_t pwm_late_wakes;
    /** Largest PWM worker lateness since open, us. */
    uint32_t pwm_max_lateness_us;
    /** Motor leases that ran out and were coasted by the backend, since open. */
    uint32_t pwm_lease_expiries;
    /** Number of fault latches since open. */
    uint32_t fault_count;
} keel_snapshot;

/** @brief Debounced button state returned by keel_take_buttons(). */
typedef struct keel_buttons {
    /** Caller sets sizeof(keel_buttons). */
    uint32_t struct_size;
    /** Buttons held now (debounced, 20 ms). */
    uint32_t held;
    /** Buttons pressed since the previous take. */
    uint32_t pressed;
    /** Buttons released since the previous take. */
    uint32_t released;
} keel_buttons;

/** @brief Ultrasonic measurement result. */
typedef struct keel_range {
    /** Caller sets sizeof(keel_range). */
    uint32_t struct_size;
    /** ::keel_range_status. */
    uint32_t status;
    /** Distance in mm when status is ::KEEL_RANGE_VALID; otherwise 0. */
    uint32_t distance_mm;
    /** Echo pulse width in us from kernel edge timestamps; 0 if none. */
    uint32_t echo_us;
    /** Device-clock time of the trigger (or of the immediate result), us. */
    uint64_t measured_at_us;
    /** Measurement sequence number, starting at 1; 0 while IDLE. */
    uint32_t sequence;
    /** Reserved; written as zero. */
    uint32_t reserved0;
} keel_range;

/** @brief Input of keel_cycle(): one tick plus an optional motor command. */
typedef struct keel_cycle_input {
    /** Caller sets sizeof(keel_cycle_input). */
    uint32_t struct_size;
    /** Health flags; must be ::KEEL_HEALTH_ALL (see keel_tick()). */
    uint32_t health;
    /** Strictly increasing cycle identifier (see keel_tick()). */
    uint64_t cycle_id;
    /** ::keel_cycle_command. */
    uint32_t command;
    /** Left duty for ::KEEL_CYCLE_DRIVE, permille. */
    int32_t left;
    /** Right duty for ::KEEL_CYCLE_DRIVE, permille. */
    int32_t right;
    /** Bitwise OR of ::keel_cycle_flag. */
    uint32_t flags;
} keel_cycle_input;

/**
 * @brief Simulator ground truth returned by keel_sim_get_state().
 *
 * @c mcu_stage and @c mcu_faults are opaque diagnostic values taken from the
 * production MCU policy; they are outside ABI stability and may change when
 * the policy changes.
 */
typedef struct keel_sim_state {
    /** Caller sets sizeof(keel_sim_state). */
    uint32_t struct_size;
    /** Bit n = level of BCM line n (0..27) as the board model sees it. */
    uint32_t pins;
    /** MCU policy stage (opaque diagnostic, not ABI-stable). */
    uint32_t mcu_stage;
    /** MCU latched fault bits (opaque diagnostic, not ABI-stable). */
    uint32_t mcu_faults;
    /** Bitwise OR of ::keel_sim_flag. */
    uint32_t flags;
    /** Left bridge drive after the enable gate, permille (0 if gated off). */
    int32_t bridge_left;
    /** Right bridge drive after the enable gate, permille. */
    int32_t bridge_right;
    /** Servo pulse reaching J5, us (0 when unpowered or idle). */
    uint32_t servo_pulse_us;
    /** Simulator clock, us. */
    uint64_t now_us;
    /** Heartbeat falls seen by the MCU model since open. */
    uint32_t heartbeat_falls;
    /** Reserved; written as zero. */
    uint32_t reserved0;
} keel_sim_state;

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_meta Library metadata
 * @brief Calls that need no device. All are thread-safe and never block.
 * @{
 */

/**
 * @brief ABI version of the loaded library.
 * @return (major << 16) | minor, i.e. ::KEEL_ABI_VERSION of the library's own
 *         header. A binding requires an equal major and a minor at least as
 *         high as the newest function it calls.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API uint32_t keel_abi_version(void);

/**
 * @brief Human-readable build identification.
 * @return Static string such as
 *         "1.0.0 (aarch64-linux-gnu; backends r3,simulator)". Never NULL; do
 *         not free; at most ::KEEL_STRING_MAX bytes including the NUL.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API const char *keel_version_string(void);

/**
 * @brief Symbolic name of a status code, for logs and exception mapping.
 * @param[in] status Any value.
 * @return Static string such as "KEEL_E_IO"; "KEEL_E_UNKNOWN" for values this
 *         library does not define. Never NULL; at most ::KEEL_STRING_MAX
 *         bytes including the NUL.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API const char *keel_status_name(int32_t status);

/**
 * @brief One-sentence description of a status code that names the remedy
 *        where there is one.
 * @param[in] status Any value.
 * @return Static English text. Never NULL; at most ::KEEL_STRING_MAX bytes
 *         including the NUL.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API const char *keel_status_string(int32_t status);

/**
 * @brief Class of a status code.
 * @param[in] status Any value.
 * @return A ::keel_status_class value. Values this library does not define
 *         return ::KEEL_CLASS_FAULT (the conservative interpretation).
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API uint32_t keel_status_class(int32_t status);

/**
 * @brief Symbolic name of a fault cause.
 * @param[in] cause A ::keel_fault_cause value.
 * @return Static string such as "PWM_OVERRUN"; "UNKNOWN" if undefined. Never
 *         NULL; at most ::KEEL_STRING_MAX bytes including the NUL.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API const char *keel_fault_cause_name(uint32_t cause);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_lifetime Lifetime
 * @brief One open and one close for every backend.
 * @{
 */

/**
 * @brief Open a device.
 *
 * keel_r3: discovers the GPIO chip labelled "pinctrl-bcm2711"; refuses lines
 * already used by kernel functions such as SPI0; claims BCM26, 18, 19, 23,
 * 22, 17 and 27 (control outputs), BCM8, 11, 10 and 9 (motor outputs) and
 * BCM20, 21, 5, 6, 16 and 24 (inputs), plus BCM13/BCM25 unless
 * ::KEEL_OPEN_NO_RANGE, with exclusive kernel requests and every output
 * starting LOW; starts the PWM worker at SCHED_FIFO ::KEEL_RT_PRIORITY (best
 * effort when refused or with ::KEEL_OPEN_NO_REALTIME); initialises the LED
 * driver dark. BCM0, 1 and 7 (MCU SWD/NRST), BCM14/15 and BCM4 are never
 * touched. A missing I2C bus or LED driver clears ::KEEL_CAP_RGB and sets
 * ::KEEL_OPEN_WARN_NO_RGB; it never fails the open. The device starts
 * DISARMED; the first heartbeat is emitted by a tick at least
 * ::KEEL_HEARTBEAT_MIN_MS after open.
 *
 * Every non-OK status means no device was created: @p *out_device is NULL
 * and a binding raises its setup error carrying @p report, whatever the
 * class. An options struct larger than this library knows with a non-zero
 * tail is reported as ::KEEL_E_INVALID_ARGUMENT (resource
 * ::KEEL_RESOURCE_OPTIONS), not ::KEEL_E_UNSUPPORTED, so that open only ever
 * returns OK, INVALID_ARGUMENT or SETUP-class codes.
 *
 * @param[in]  options    Options, or NULL for all defaults.
 * @param[out] out_device Receives the handle; set to NULL on failure. Must
 *                        not be NULL.
 * @param[out] report     Optional detailed outcome (may be NULL); written on
 *                        success and on failure.
 * @retval KEEL_OK                 Device open (check @c report->warnings).
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL @p out_device, bad
 *         @c struct_size of @p options or @p report, unknown backend or flag,
 *         timeout out of range, unterminated or non-empty-for-simulator path,
 *         ::KEEL_OPEN_VIRTUAL_CLOCK without the simulator, or an options tail
 *         this library cannot honour.
 * @retval KEEL_E_NOT_FOUND        [SETUP] No BCM2711 GPIO chip, no GPIO v2
 *         kernel interface, or the backend is not built in.
 * @retval KEEL_E_IN_USE           [SETUP] A line is owned elsewhere; see
 *         @c report->line and @c report->consumer.
 * @retval KEEL_E_PERMISSION       [SETUP] EACCES/EPERM on a device node.
 * @retval KEEL_E_NO_MEMORY        [SETUP] Allocation failed.
 * @retval KEEL_E_SETUP_IO         [SETUP] Another I/O failure while opening;
 *         everything acquired was released.
 * @par Blocking
 * Up to about 100 ms (line requests, LED initialisation with a 20 ms I2C
 * timeout, worker start).
 * @par Thread safety
 * Any thread. keel_r3 allows one open device per robot, including within one
 * process (a second open returns ::KEEL_E_IN_USE).
 * @since 1.0
 */
KEEL_API int32_t keel_open(const keel_open_options *options, keel_device **out_device,
                           keel_open_report *report);

/**
 * @brief Close a device: Category-0 stop, LEDs off, release every resource.
 *
 * Lowers GPIO26 at once (before taking any lock), then under the control lock
 * coasts both motors, ends any servo burst and stops the heartbeat, then
 * turns all LEDs off, wakes callers blocked in keel_sleep_until() or
 * keel_range_measure() (they return ::KEEL_E_INVALID_HANDLE), waits for
 * in-flight calls, stops the PWM worker and releases all lines. The handle is
 * invalid after return whatever the status. The MCU removes all permissions
 * and sensor power when the heartbeat window closes.
 *
 * The status is diagnostic only: bindings log it and never throw from close,
 * Dispose or a cleaner.
 *
 * @param[in]  device          Handle, or NULL (returns ::KEEL_OK).
 * @param[out] out_final_fault Optional (may be NULL): receives the final fault
 *                             record, including @c cleanup_status of this
 *                             close's stop.
 * @retval KEEL_OK                 Closed (also for a NULL @p device).
 * @retval KEEL_E_IO               [FAULT] A stop action failed: the stop could
 *         not be confirmed; the handle is still released and the MCU timeout
 *         still applies. Details are in @p out_final_fault.
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] @p out_final_fault has a bad
 *         @c struct_size; the device was still closed.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE] Already closed, stale or forged.
 * @retval KEEL_E_NOT_OWNER        [HANDLE] Called in a forked child; nothing
 *         was touched (the parent still owns the hardware).
 * @par Blocking
 * About 100 ms at most (bounded in-flight calls, worker join).
 * @par Thread safety
 * Any thread ("stop wins"), including concurrently with other calls on the
 * same handle.
 * @since 1.0
 */
KEEL_API int32_t keel_close(keel_device *device, keel_fault *out_final_fault);

/**
 * @brief Features available on this device.
 * @param[in]  device           Handle.
 * @param[out] out_capabilities Receives ::keel_capability bits.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL output pointer.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread; no I/O.
 * @since 1.0
 */
KEEL_API int32_t keel_get_capabilities(const keel_device *device, uint32_t *out_capabilities);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_clock Device clock
 * @brief Monotonic time and absolute sleeps. Using these instead of the
 *        language's own clock lets the same loop run unchanged against the
 *        simulator's virtual clock.
 * @{
 */

/**
 * @brief Current device time.
 * @param[in]  device     Handle.
 * @param[out] out_now_us Receives monotonic microseconds (CLOCK_MONOTONIC on
 *                        Linux; the virtual clock of a virtual-clock simulator).
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL output pointer.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread; takes no device lock.
 * @since 1.0
 */
KEEL_API int32_t keel_now_us(const keel_device *device, uint64_t *out_now_us);

/**
 * @brief Sleep until an absolute device time.
 *
 * Returns at once if @p deadline_us has passed and resumes after signal
 * interruptions. Wakes early when keel_close() starts, returning
 * ::KEEL_E_INVALID_HANDLE. With ::KEEL_OPEN_VIRTUAL_CLOCK it advances the
 * virtual clock instead of sleeping, stepping the board and MCU model every
 * simulated millisecond under the simulator's own lock; drive a virtual-clock
 * device from one thread.
 *
 * @param[in] device      Handle.
 * @param[in] deadline_us Absolute device time, at most
 *                        ::KEEL_SLEEP_AHEAD_MAX_MS ahead.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] Deadline too far ahead (usually a
 *         relative value passed by mistake).
 * @retval KEEL_E_IO               [FAULT] The backend's sleep failed (cause
 *         ::KEEL_CAUSE_CLOCK, sticky).
 * @retval KEEL_E_INTERNAL         [FAULT] The backend's sleep broke the port
 *         contract (cause ::KEEL_CAUSE_HAL_CONTRACT, sticky).
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE] Also when the device was closed
 *         during the sleep.
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * Until the deadline or the start of keel_close(). Holds no device lock on
 * the real-time clock.
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_sleep_until(keel_device *device, uint64_t deadline_us);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_safety Safety and control loop
 * @brief Heartbeat, arming, disarming and fault handling.
 * @{
 */

/**
 * @brief One health-loop step: input sampling, heartbeat, arming progress and
 *        motor-lease renewal.
 *
 * Samples READY/ARMED and the buttons (20 ms debounce); checks the backend
 * fault latch before anything else; emits a 1 ms heartbeat pulse when at
 * least ::KEEL_HEARTBEAT_MIN_MS have passed since the last fall (the control
 * lock is released during the pulse); advances the arming acknowledgement;
 * while ARMED renews the motor lease to min(command expiry, now + 70 ms),
 * coasts an expired command and applies a pending reversal once its dwell has
 * elapsed; while SERVO checks the grant, starts the pulse once granted and
 * ends the burst when its lease has run out.
 *
 * Rules:
 * - Call every 10 ms (gaps up to ::KEEL_TICK_WINDOW_MS accepted) from the
 *   loop that really performs the application's control work, never from a
 *   timer or keepalive thread that runs regardless of that work.
 * - The first successful tick after open or keel_clear_fault() makes the
 *   calling thread the control owner; a tick from any other thread
 *   fail-stops (::KEEL_CAUSE_TICK_THREAD_CHANGED).
 * - @p cycle_id must strictly increase over the device's lifetime.
 * - @p health must be ::KEEL_HEALTH_ALL; report a problem by passing fewer
 *   bits, which fail-stops on purpose.
 * - A gap over ::KEEL_TICK_WINDOW_MS, or a heartbeat fall later than
 *   ::KEEL_HEARTBEAT_LATE_MS, restarts the heartbeat when the previous tick
 *   ran while DISARMED (no fault; ::KEEL_SNAP_HEARTBEAT_RESTARTED) and
 *   fail-stops with ::KEEL_E_DEADLINE when it ran while ARMING, ARMED or
 *   SERVO.
 * - While FAULTED the inputs and buttons are still sampled, no heartbeat is
 *   emitted, and ::KEEL_E_FAULTED is returned.
 *
 * @param[in] device   Handle.
 * @param[in] cycle_id Strictly increasing loop counter.
 * @param[in] health   ::KEEL_HEALTH_ALL.
 * @retval KEEL_OK
 * @retval KEEL_E_HEALTH         [FAULT] Health bits missing, cycle_id not
 *         increasing, or not the control owner's thread.
 * @retval KEEL_E_DEADLINE       [FAULT] Tick or heartbeat late while arming,
 *         armed or in a servo burst.
 * @retval KEEL_E_FEEDBACK       [FAULT] READY/ARMED inconsistent with the state.
 * @retval KEEL_E_ACK_TIMEOUT    [FAULT] Arming not completed in time.
 * @retval KEEL_E_IO             [FAULT] Control I/O failure.
 * @retval KEEL_E_PWM            [FAULT] Motor PWM worker failure.
 * @retval KEEL_E_INTERNAL       [FAULT] Clock, interlock or backend contract failure.
 * @retval KEEL_E_FAULTED        [FAULT] Already latched; nothing emitted.
 * @retval KEEL_E_INVALID_HANDLE [HANDLE]
 * @retval KEEL_E_NOT_OWNER      [HANDLE]
 * @par Blocking
 * About 1.1 ms when a heartbeat pulse is due, otherwise about 0.1 ms.
 * @par Thread safety
 * The control owner only.
 * @since 1.0
 */
KEEL_API int32_t keel_tick(keel_device *device, uint64_t cycle_id, uint32_t health);

/**
 * @brief Tick, apply a motor command and read the state in one call.
 *
 * Runs keel_tick(@c in->cycle_id, @c in->health); then, if the tick returned
 * ::KEEL_OK, the command in @c in->command (::KEEL_CYCLE_DRIVE =
 * keel_set_motors(), ::KEEL_CYCLE_BRAKE = keel_brake()); then fills @p out,
 * taking button edges if ::KEEL_CYCLE_TAKE_BUTTONS is set. The command and the
 * snapshot are applied in the same control-lock section as the tick's input
 * evaluation. One foreign call per loop iteration, with caller-allocated
 * structs.
 *
 * The tick always runs when @p in is non-NULL and its @c struct_size is
 * acceptable. An unknown command or flag, an out-of-range duty or a bad
 * @p out is detected after the tick: the motors coast as for an out-of-range
 * keel_set_motors() while ARMED, and ::KEEL_E_INVALID_ARGUMENT is returned.
 * Bindings treat ::KEEL_E_INVALID_ARGUMENT and ::KEEL_E_UNSUPPORTED from
 * keel_cycle() as fatal for the loop.
 *
 * @param[in]  device Handle.
 * @param[in]  in     Cycle input; must not be NULL.
 * @param[out] out    Snapshot after the cycle, or NULL.
 * @return The tick's status if it is not ::KEEL_OK, otherwise the motor
 *         command's status (see keel_tick(), keel_set_motors() and
 *         keel_brake()), or:
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL @p in or @c struct_size
 *         below the 1.0 size (no tick); or, after the tick, an unknown
 *         command or flag, an out-of-range duty or a bad @p out.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] @p in is larger than this
 *         library knows with a non-zero tail (no tick, no effect).
 * @par Blocking
 * As keel_tick().
 * @par Thread safety
 * The control owner only.
 * @since 1.0
 */
KEEL_API int32_t keel_cycle(keel_device *device, const keel_cycle_input *in, keel_snapshot *out);

/**
 * @brief Request motor permission (an explicit operator or application decision).
 *
 * From DISARMED, re-asserts GPIO26 LOW and coast, then starts the
 * acknowledgement: GPIO26 LOW for at least ::KEEL_ACK_LEVEL_MS and until
 * READY is HIGH, then HIGH, LOW and HIGH for ::KEEL_ACK_LEVEL_MS each, with
 * all motor inputs and the servo request LOW throughout. Returns immediately;
 * arming completes over about 0.6-0.85 s of continuing ticks and the state
 * becomes ARMED, or the device fail-stops (::KEEL_E_ACK_TIMEOUT,
 * ::KEEL_E_FEEDBACK). Motion commands sent while arming are refused, never
 * queued. Never call it automatically after a fault.
 *
 * @param[in] device Handle.
 * @retval KEEL_OK               Arming started, or already ARMING/ARMED (no-op).
 * @retval KEEL_E_CONFLICT       [REJECTED] Not the control owner's thread, or
 *         a servo burst is active.
 * @retval KEEL_E_STATE          [REJECTED] Less than ::KEEL_REARM_HOLDOFF_MS
 *         since the last fault latch (::KEEL_SNAP_REARM_HOLDOFF).
 * @retval KEEL_E_FAULTED        [FAULT] The device is FAULTED; call
 *         keel_clear_fault() first.
 * @retval KEEL_E_IO             [FAULT] Could not re-assert the outputs LOW.
 * @retval KEEL_E_PWM            [FAULT] The backend had latched a PWM fault.
 * @retval KEEL_E_INTERNAL       [FAULT] Clock or backend contract failure.
 * @retval KEEL_E_INVALID_HANDLE [HANDLE]
 * @retval KEEL_E_NOT_OWNER      [HANDLE]
 * @par Thread safety
 * The control owner only (any thread before the first tick).
 * @since 1.0
 */
KEEL_API int32_t keel_request_arm(keel_device *device);

/**
 * @brief Category-0 stop: remove motor permission now (non-latching).
 *
 * Lowers GPIO26 at once through the backend, before taking any lock (the AND
 * gate removes bridge enable), then under the control lock re-runs the whole
 * safe stop: GPIO26 LOW, coast, servo pulse and request LOW, and discards
 * pending commands, brake and reversal. From DISARMED, ARMING, ARMED and
 * SERVO the state becomes DISARMED and the heartbeat continues, so sensor
 * power stays on. In FAULTED the outputs, including the heartbeat, are
 * re-asserted LOW and the state stays FAULTED. Driving again requires
 * keel_request_arm(). For "stop moving but stay armed" use
 * keel_set_motors(device, 0, 0) or keel_brake(); for a latching stop use
 * keel_emergency_stop().
 *
 * @param[in] device Handle.
 * @retval KEEL_OK               Stopped (any state; idempotent).
 * @retval KEEL_E_IO             [FAULT] A stop action failed; every other
 *         action was still attempted, keel_fault::cleanup_status is updated and
 *         the device is FAULTED.
 * @retval KEEL_E_PWM            [FAULT] The stop succeeded, but the backend had
 *         latched a PWM fault, which is now recorded.
 * @retval KEEL_E_INTERNAL       [FAULT] Backend contract failure found during the stop.
 * @retval KEEL_E_INVALID_HANDLE [HANDLE]
 * @retval KEEL_E_NOT_OWNER      [HANDLE]
 * @par Blocking
 * GPIO26 falls within one backend write, before any lock is taken. The call
 * then takes the control lock, which no call holds across a sleep, an I2C
 * transaction or any other blocking wait.
 * @par Thread safety
 * Any thread; always accepted ("stop wins").
 * @since 1.0
 */
KEEL_API int32_t keel_disarm(keel_device *device);

/**
 * @brief Latching Category-0 stop for stop buttons and Ctrl-C/SIGTERM handling.
 *
 * Performs the keel_disarm() stop, stops the heartbeat (so the MCU also
 * removes motor, servo and sensor power on its own) and latches FAULTED with
 * cause ::KEEL_CAUSE_USER_STOP (record status ::KEEL_E_FAULTED). Leaving it
 * needs keel_clear_fault() from the control owner, then keel_request_arm().
 * If the device is already FAULTED, the stop is re-run and the existing
 * record is kept. The fault record names the state the stop ended (for
 * example ARMED), not the DISARMED state the stop passes through.
 *
 * @param[in] device Handle.
 * @retval KEEL_OK               Stopped and latched (state FAULTED).
 * @retval KEEL_E_IO             [FAULT] A stop action failed; every other
 *         action was still attempted.
 * @retval KEEL_E_PWM            [FAULT] Stopped and latched, but the backend
 *         had already latched a PWM fault, which is the recorded cause.
 * @retval KEEL_E_INTERNAL       [FAULT] Backend contract failure found during the stop.
 * @retval KEEL_E_INVALID_HANDLE [HANDLE]
 * @retval KEEL_E_NOT_OWNER      [HANDLE]
 * @par Blocking
 * As keel_disarm().
 * @par Signal safety
 * No libkeel function is async-signal-safe, this one included: called from a
 * signal handler it can deadlock on a lock the interrupted thread holds. In a
 * handler only set a <tt>volatile sig_atomic_t</tt> flag (or write to a
 * pipe), and call keel_emergency_stop() from a normal thread: the loop, or a
 * thread that waits with sigwait() or reads a signalfd.
 * @par Thread safety
 * Any thread, any state.
 * @since 1.0
 */
KEEL_API int32_t keel_emergency_stop(keel_device *device);

/**
 * @brief Leave FAULTED without arming (an explicit decision, never a retry loop).
 *
 * Re-asserts every safety output LOW, resets the backend fault latch, marks
 * the record ::KEEL_FAULT_CLEARED (it stays readable), forgets the control
 * owner and returns to DISARMED. The next tick restarts the heartbeat and
 * becomes the new control owner; the MCU restores sensor power about
 * 0.1-0.2 s later. Motor permission still requires keel_request_arm(), which
 * stays refused until ::KEEL_REARM_HOLDOFF_MS after the latch. Sticky causes
 * (::KEEL_FAULT_STICKY) cannot be cleared; close and reopen the device.
 *
 * @param[in] device Handle.
 * @retval KEEL_OK               Cleared, or not FAULTED (no-op).
 * @retval KEEL_E_CONFLICT       [REJECTED] Not the control owner's thread.
 * @retval KEEL_E_FAULTED        [FAULT] The recorded cause is sticky; nothing
 *         changed.
 * @retval KEEL_E_IO             [FAULT] Re-asserting the safe outputs failed;
 *         the device stays FAULTED.
 * @retval KEEL_E_PWM            [FAULT] The backend could not reset its PWM
 *         latch; the device stays FAULTED.
 * @retval KEEL_E_INTERNAL       [FAULT] Backend contract failure.
 * @retval KEEL_E_INVALID_HANDLE [HANDLE]
 * @retval KEEL_E_NOT_OWNER      [HANDLE]
 * @par Thread safety
 * The control owner only (any thread while no owner is recorded).
 * @since 1.0
 */
KEEL_API int32_t keel_clear_fault(keel_device *device);

/**
 * @brief Current coarse state (cached; no I/O).
 * @param[in]  device    Handle.
 * @param[out] out_state Receives a ::keel_state value.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL output pointer.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_get_state(const keel_device *device, uint32_t *out_state);

/**
 * @brief Cached device state and diagnostics (no I/O, consumes nothing).
 * @param[in]  device Handle.
 * @param[out] out    Snapshot; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread; takes only the control lock, briefly.
 * @since 1.0
 */
KEEL_API int32_t keel_get_snapshot(const keel_device *device, keel_snapshot *out);

/**
 * @brief Fault record (cached; no I/O).
 *
 * The first fault since the last keel_clear_fault(). After a clear the record
 * stays readable with ::KEEL_FAULT_CLEARED set until the next latch.
 *
 * @param[in]  device Handle.
 * @param[out] out    Record; @c out->status is ::KEEL_OK and @c out->cause
 *                    ::KEEL_CAUSE_NONE when no fault was ever recorded.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_get_fault(const keel_device *device, keel_fault *out);

/**
 * @brief Fresh read of READY and ARMED (diagnostic).
 *
 * Emits no heartbeat, renews nothing and evaluates no feedback rule. A read
 * failure, or a backend fault latch found by the read, fail-stops like any
 * control-path failure.
 *
 * @param[in]  device     Handle.
 * @param[out] out_inputs Receives ::keel_input bits.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL output pointer.
 * @retval KEEL_E_IO               [FAULT] GPIO read failed.
 * @retval KEEL_E_PWM              [FAULT] The backend had latched a PWM fault.
 * @retval KEEL_E_INTERNAL         [FAULT] Backend contract failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_read_safety_inputs(keel_device *device, uint32_t *out_inputs);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_motion Motion
 * @brief Drive, coast and brake. Every accepted drive or brake command lives
 *        for the configured command timeout (default 500 ms) and is then
 *        coasted without a fault (::KEEL_SNAP_COMMAND_EXPIRED); re-send it
 *        every cycle while it should stay in force.
 * @{
 */

/**
 * @brief Set both motor duties.
 *
 * Signed permille: positive is forward (left forward drives BCM11, right
 * forward drives BCM10, the opposite input of each bridge held LOW), zero
 * coasts. (0, 0) is a stop: accepted from any thread in every state; while
 * ARMED it coasts both motors synchronously, drops the stored command,
 * pending reversal and brake, and then checks the backend's fault latch, so
 * a PWM failure found in the meantime is reported (::KEEL_E_PWM) rather than
 * a stale ARMED; elsewhere it does nothing.
 *
 * A non-zero command needs ARMED and the control owner's thread. It reads
 * READY/ARMED fresh, stores the command with expiry now + command timeout,
 * and drives. A direction change on a wheel that has not been idle (coast or
 * brake) for ::KEEL_REVERSAL_DWELL_MS is accepted: both motors coast at once
 * and the first tick or command at least ::KEEL_REVERSAL_DWELL_MS later
 * applies it (::KEEL_SNAP_REVERSAL_PENDING). A newer command replaces a
 * pending one. Cancels a brake.
 *
 * A duty outside -1000..1000 is refused; while ARMED the motors coast and the
 * stored command, pending reversal and brake are dropped, so the next tick
 * renews nothing, and the backend's fault latch is checked as for (0, 0).
 *
 * @param[in] device Handle.
 * @param[in] left   Left duty, -1000..1000 permille.
 * @param[in] right  Right duty, -1000..1000 permille.
 * @retval KEEL_OK                 Applied, deferred for reversal, or (0, 0).
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] A duty is out of range; if ARMED
 *         both motors coast. No fault.
 * @retval KEEL_E_CONFLICT         [REJECTED] Non-zero duty from a thread other
 *         than the control owner, or during a servo burst.
 * @retval KEEL_E_STATE            [REJECTED] Non-zero duty while DISARMED or
 *         ARMING; nothing changes (arming continues).
 * @retval KEEL_E_FAULTED          [FAULT] Non-zero duty while FAULTED.
 * @retval KEEL_E_DEADLINE         [FAULT] ARMED but the tick window has expired.
 * @retval KEEL_E_FEEDBACK         [FAULT] Fresh READY/ARMED read is not 1/1.
 * @retval KEEL_E_IO               [FAULT] Control I/O or synchronous motor
 *         write failure.
 * @retval KEEL_E_PWM              [FAULT] The PWM worker had failed.
 * @retval KEEL_E_INTERNAL         [FAULT] Clock, interlock or backend
 *         contract failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * About 0.1 ms (one input read and one line update).
 * @par Thread safety
 * Non-zero: the control owner only. (0, 0): any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_set_motors(keel_device *device, int32_t left, int32_t right);

/**
 * @brief Brake both motors (all four bridge inputs HIGH) while staying armed.
 *
 * Lasts for the command timeout or until the next motion command, and counts
 * as idle time for a later reversal. The MCU counts a brake as PWM activity,
 * so a servo burst is refused until the motors have coasted for
 * ::KEEL_SERVO_IDLE_MS. Every stop path (disarm, fault, close) coasts instead
 * of braking. Without ::KEEL_CAP_BRAKE the call is refused and, while ARMED,
 * the motors coast.
 *
 * @param[in] device Handle.
 * @retval KEEL_OK
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] No ::KEEL_CAP_BRAKE (coasts if ARMED).
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the control owner's thread,
 *         or a servo burst is active.
 * @retval KEEL_E_STATE            [REJECTED] Not ARMED (DISARMED or ARMING).
 * @retval KEEL_E_FAULTED          [FAULT] Device latched.
 * @retval KEEL_E_DEADLINE         [FAULT] Tick window expired.
 * @retval KEEL_E_FEEDBACK         [FAULT] Fresh READY/ARMED read is not 1/1.
 * @retval KEEL_E_IO               [FAULT] Control I/O or synchronous motor
 *         write failure.
 * @retval KEEL_E_PWM              [FAULT] The PWM worker had failed.
 * @retval KEEL_E_INTERNAL         [FAULT] Clock, interlock or backend
 *         contract failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * The control owner only.
 * @since 1.0
 */
KEEL_API int32_t keel_brake(keel_device *device);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_servo Servo (J5)
 * @brief Timed servo bursts under the MCU servo lease. Motors and servo are
 *        mutually exclusive and a burst ends disarmed. Requires
 *        ::KEEL_CAP_SERVO, which keel_r3 does not offer (J5 is not fitted on
 *        R3); the simulator implements the full contract.
 * @{
 */

/**
 * @brief Start a servo burst.
 *
 * Preconditions: ARMED; the control owner's thread; both motors coasting (not
 * driven, not braked, no pending reversal) for at least ::KEEL_SERVO_IDLE_MS;
 * at least ::KEEL_SERVO_COOLDOWN_MS since open, since READY was last seen LOW
 * and since the servo-request LOW write that ended the last burst. Raises the
 * servo request and returns; the state becomes SERVO and the stored motor
 * command and timeout are discarded. Ticks then confirm the grant (ARMED
 * reads 0 and READY 1 within ::KEEL_SERVO_GRANT_MS) and only then start the
 * 50 Hz pulse. The backend also ends the burst by itself at the lease end,
 * independently of ticks. When the burst ends (lease, keel_servo_end(),
 * keel_disarm(), fault, close) the pulse stops, then the request and GPIO26
 * go LOW, the state becomes DISARMED and the cooldown restarts. The servo has
 * no holding torque between bursts.
 *
 * @param[in] device   Handle.
 * @param[in] pulse_us Pulse width, ::KEEL_SERVO_PULSE_MIN_US..::KEEL_SERVO_PULSE_MAX_US.
 * @param[in] lease_ms Burst length, ::KEEL_SERVO_LEASE_MIN_MS..::KEEL_SERVO_LEASE_MAX_MS.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] Pulse or lease out of range.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] No servo capability.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the control owner's thread; a
 *         motor is driven or braked or a reversal is pending; or a burst is
 *         already active (a lease cannot be extended).
 * @retval KEEL_E_STATE            [REJECTED] Not ARMED.
 * @retval KEEL_E_AGAIN            [REJECTED] Cooldown running, or motors idle for
 *         less than ::KEEL_SERVO_IDLE_MS; see
 *         keel_snapshot::servo_cooldown_remaining_ms.
 * @retval KEEL_E_FAULTED          [FAULT] Device latched.
 * @retval KEEL_E_FEEDBACK         [FAULT] Fresh READY/ARMED read is not 1/1.
 * @retval KEEL_E_IO               [FAULT] Control I/O failure.
 * @retval KEEL_E_PWM              [FAULT] The PWM worker had failed.
 * @retval KEEL_E_INTERNAL         [FAULT] Clock, interlock or backend
 *         contract failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * The control owner only.
 * @since 1.0
 */
KEEL_API int32_t keel_servo_start(keel_device *device, uint32_t pulse_us, uint32_t lease_ms);

/**
 * @brief Change the pulse width of the active burst (never extends it).
 *
 * Before the grant is confirmed the new width is stored and used when the
 * pulse starts.
 *
 * @param[in] device   Handle.
 * @param[in] pulse_us Pulse width, ::KEEL_SERVO_PULSE_MIN_US..::KEEL_SERVO_PULSE_MAX_US.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] Out of range.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] No servo capability.
 * @retval KEEL_E_CONFLICT         [REJECTED] Not the control owner's thread.
 * @retval KEEL_E_STATE            [REJECTED] No burst active.
 * @retval KEEL_E_FAULTED          [FAULT] Device latched.
 * @retval KEEL_E_IO               [FAULT] Control I/O failure.
 * @retval KEEL_E_PWM              [FAULT] The backend had latched a fault.
 * @retval KEEL_E_INTERNAL         [FAULT] Backend contract failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * The control owner only.
 * @since 1.0
 */
KEEL_API int32_t keel_servo_set_pulse(keel_device *device, uint32_t pulse_us);

/**
 * @brief End the active burst early (state becomes DISARMED, cooldown restarts).
 * @param[in] device Handle.
 * @retval KEEL_OK               Ended, or no burst active (no-op; also without
 *         ::KEEL_CAP_SERVO).
 * @retval KEEL_E_IO             [FAULT] A stop write failed.
 * @retval KEEL_E_PWM            [FAULT] The burst ended, but the backend had
 *         latched a fault, which is now recorded.
 * @retval KEEL_E_INTERNAL       [FAULT] Backend contract failure.
 * @retval KEEL_E_INVALID_HANDLE [HANDLE]
 * @retval KEEL_E_NOT_OWNER      [HANDLE]
 * @par Thread safety
 * Any thread ("stop wins").
 * @since 1.0
 */
KEEL_API int32_t keel_servo_end(keel_device *device);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_ranging Ultrasonic ranging (J1, HC-SR04)
 * @brief TRIG is BCM13 and ECHO is BCM25. Measurements never return a
 *        sentinel distance: an unpowered sensor is ::KEEL_RANGE_UNAVAILABLE.
 *        Sensor power is switched by the MCU and needs a running heartbeat,
 *        not arming. Ranging never latches a fault. Requires ::KEEL_CAP_RANGE.
 * @{
 */

/**
 * @brief Trigger a measurement (non-blocking, about 15 us).
 *
 * When keel_snapshot::sensor_power is not PERMITTED or CONFIRMED (heartbeat
 * stopped, not yet qualified, or READY LOW), TRIG is not driven: the call
 * returns ::KEEL_OK with an immediate ::KEEL_RANGE_UNAVAILABLE result (a new
 * sequence number; any pending measurement is abandoned). Because the cached
 * READY can be a whole tick old, a powered sensor is confirmed by a fresh
 * read of READY just before TRIG is driven; that read is control I/O, so a
 * failure or a fault the backend has latched is reported as for
 * keel_read_safety_inputs().
 *
 * @param[in] device Handle.
 * @retval KEEL_OK
 * @retval KEEL_E_UNSUPPORTED     [REJECTED] No ranging capability.
 * @retval KEEL_E_AGAIN           [REJECTED] A measurement is pending, less than
 *         ::KEEL_RANGE_INTERVAL_MS since the last trigger, or ECHO is still HIGH.
 * @retval KEEL_E_IO              [FAULT] The fresh READY read failed.
 * @retval KEEL_E_PWM             [FAULT] The backend had latched a PWM fault.
 * @retval KEEL_E_INTERNAL        [FAULT] Backend contract failure.
 * @retval KEEL_E_PERIPHERAL      [PERIPHERAL] Trigger or edge setup failed.
 * @retval KEEL_E_INVALID_HANDLE  [HANDLE]
 * @retval KEEL_E_NOT_OWNER       [HANDLE]
 * @par Thread safety
 * Any thread; uses the range lock, never delays ticks.
 * @since 1.0
 */
KEEL_API int32_t keel_range_start(keel_device *device);

/**
 * @brief Collect the latest measurement (non-blocking).
 * @param[in]  device Handle.
 * @param[out] out    Result: ::KEEL_RANGE_PENDING until complete (at most
 *                    ::KEEL_RANGE_TIMEOUT_MS after the trigger),
 *                    ::KEEL_RANGE_IDLE if none started.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] No ranging capability.
 * @retval KEEL_E_PERIPHERAL       [PERIPHERAL] Edge events could not be read.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_range_poll(keel_device *device, keel_range *out);

/**
 * @brief Trigger and wait for one measurement (blocking convenience).
 *
 * Never waits for the trigger spacing (returns ::KEEL_E_AGAIN instead); waits
 * at most ::KEEL_RANGE_MEASURE_MAX_MS in total, without holding the control
 * lock, and wakes when keel_close() starts. Unpowered sensors give an
 * immediate ::KEEL_RANGE_UNAVAILABLE as for keel_range_start(), including
 * its fresh READY read. Loops use keel_range_start() and keel_range_poll()
 * instead.
 *
 * @param[in]  device Handle.
 * @param[out] out    Completed result (never PENDING or IDLE).
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] No ranging capability.
 * @retval KEEL_E_CONFLICT         [REJECTED] Called by the control owner while
 *         ARMING, ARMED or SERVO.
 * @retval KEEL_E_AGAIN            [REJECTED] A measurement is pending, the
 *         spacing has not elapsed, or ECHO is still HIGH.
 * @retval KEEL_E_IO               [FAULT] The fresh READY read failed, or the
 *         backend's sleep failed (cause ::KEEL_CAUSE_CLOCK, sticky).
 * @retval KEEL_E_PWM              [FAULT] The backend had latched a PWM fault.
 * @retval KEEL_E_INTERNAL         [FAULT] Backend contract failure, including
 *         a sleep that broke the port contract.
 * @retval KEEL_E_PERIPHERAL       [PERIPHERAL] I/O failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE] Also when the device was closed
 *         during the wait.
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * At most ::KEEL_RANGE_MEASURE_MAX_MS.
 * @par Thread safety
 * Any thread except the control owner while ARMING, ARMED or SERVO.
 * @since 1.0
 */
KEEL_API int32_t keel_range_measure(keel_device *device, keel_range *out);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_leds LEDs
 * @brief RGB LEDs (I2C) and button indicator LEDs (GPIO). Usable in every
 *        state, including FAULTED (for example to show an error colour).
 * @{
 */

/**
 * @brief Set one RGB LED.
 * @param[in] device Handle.
 * @param[in] index  ::keel_led index, 0..5.
 * @param[in] rgb24  0x00RRGGBB, linear 8-bit channels; bits 24-31 are ignored,
 *                   so Java Color.getRGB() and .NET Color.ToArgb() values
 *                   can be passed directly.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] Index out of range.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] No ::KEEL_CAP_RGB.
 * @retval KEEL_E_PERIPHERAL       [PERIPHERAL] I2C failure; the cached frame
 *         keeps the last value written successfully.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * One I2C frame (about 2 ms at 100 kHz; 20 ms timeout per transaction).
 * @par Thread safety
 * Any thread; uses the LED lock, never delays ticks or stops.
 * @since 1.0
 */
KEEL_API int32_t keel_set_rgb(keel_device *device, uint32_t index, uint32_t rgb24);

/**
 * @brief Set all six RGB LEDs to one colour (one I2C frame).
 * @param[in] device Handle.
 * @param[in] rgb24  0x00RRGGBB; bits 24-31 ignored.
 * @retval KEEL_OK
 * @retval KEEL_E_UNSUPPORTED    [REJECTED] No ::KEEL_CAP_RGB.
 * @retval KEEL_E_PERIPHERAL     [PERIPHERAL] I2C failure.
 * @retval KEEL_E_INVALID_HANDLE [HANDLE]
 * @retval KEEL_E_NOT_OWNER      [HANDLE]
 * @par Blocking
 * As keel_set_rgb().
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_set_rgb_all(keel_device *device, uint32_t rgb24);

/**
 * @brief Set the first @p count RGB LEDs in one atomic I2C frame.
 * @param[in] device Handle.
 * @param[in] rgb24  Array of @p count colours (0x00RRGGBB), RGB1 first.
 * @param[in] count  1..6; LEDs beyond @p count keep their colour.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL array or count out of range.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] No ::KEEL_CAP_RGB.
 * @retval KEEL_E_PERIPHERAL       [PERIPHERAL] I2C failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Blocking
 * As keel_set_rgb().
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_set_rgb_frame(keel_device *device, const uint32_t *rgb24, uint32_t count);

/**
 * @brief Colour last written successfully to one RGB LED (cached; no I/O).
 * @param[in]  device    Handle.
 * @param[in]  index     ::keel_led index, 0..5.
 * @param[out] out_rgb24 Receives 0x00RRGGBB.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] Index out of range or NULL pointer.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_get_rgb(const keel_device *device, uint32_t index, uint32_t *out_rgb24);

/**
 * @brief Switch button indicator LEDs (on/off only; no PWM on these pins).
 * @param[in] device Handle.
 * @param[in] mask   ::keel_button bits to change; others keep their state.
 * @param[in] values New levels for the bits in @p mask (1 = lit); bits outside
 *                   @p mask are ignored.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] @p mask has bits above 0xF.
 * @retval KEEL_E_IO               [FAULT] GPIO failure (these lines share the
 *         control request with GPIO26 and the heartbeat).
 * @retval KEEL_E_PWM              [FAULT] The backend had latched a PWM fault.
 * @retval KEEL_E_INTERNAL         [FAULT] Backend contract failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_set_button_leds(keel_device *device, uint32_t mask, uint32_t values);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_buttons_group Buttons
 * @brief Debounced buttons A, B, X and Y.
 * @{
 */

/**
 * @brief Read the buttons and take the accumulated edges.
 *
 * Performs a fresh input read and the 20 ms debounce, returns the held
 * buttons and every press and release edge since the previous take, then
 * clears those edges. A button held at open becomes a press after 20 ms.
 * Ticking every 10 ms is enough for no press to be lost; for a peek without
 * consuming, use keel_get_snapshot().
 *
 * @param[in]  device Handle.
 * @param[out] out    Result; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_IO               [FAULT] GPIO read failed (the same request
 *         carries READY/ARMED).
 * @retval KEEL_E_PWM              [FAULT] The backend had latched a PWM fault
 *         (for example a worker overrun since the last tick).
 * @retval KEEL_E_INTERNAL         [FAULT] Backend contract failure.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread; works in every state, including FAULTED.
 * @since 1.0
 */
KEEL_API int32_t keel_take_buttons(keel_device *device, keel_buttons *out);

/** @} */

/* ======================================================================== */
/**
 * @defgroup keel_sim Simulator
 * @brief Stimulus and ground truth for ::KEEL_BACKEND_SIMULATOR devices. On
 *        other devices these return ::KEEL_E_UNSUPPORTED and do nothing.
 * @{
 */

/**
 * @brief Apply a simulator stimulus.
 * @param[in] device Simulator handle.
 * @param[in] item   ::keel_sim_item.
 * @param[in] value  Item-specific value (see ::keel_sim_item).
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] Unknown item or bad value.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] Not a simulator device.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_sim_set(keel_device *device, uint32_t item, int32_t value);

/**
 * @brief Read the simulator's ground truth (pin levels, MCU stage and faults).
 * @param[in]  device Simulator handle.
 * @param[out] out    State; @c out->struct_size must be set.
 * @retval KEEL_OK
 * @retval KEEL_E_INVALID_ARGUMENT [REJECTED] NULL or bad @c struct_size.
 * @retval KEEL_E_UNSUPPORTED      [REJECTED] Not a simulator device.
 * @retval KEEL_E_INVALID_HANDLE   [HANDLE]
 * @retval KEEL_E_NOT_OWNER        [HANDLE]
 * @par Thread safety
 * Any thread.
 * @since 1.0
 */
KEEL_API int32_t keel_sim_get_state(const keel_device *device, keel_sim_state *out);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* KEEL_KEEL_H */
