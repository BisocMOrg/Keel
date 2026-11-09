#ifndef SAFETY_CORE_H
#define SAFETY_CORE_H
#include <stdint.h>
#include <stdbool.h>

enum safety_stage { S_DISARMED, S_WAIT_H1, S_HIGH1, S_WAIT_L2,
 S_LOW2, S_WAIT_H2, S_HIGH2, S_ARMED, S_SERVO, S_LATCHED };
enum safety_fault { F_BOOT=1u, F_HEARTBEAT=2u, F_EXTERNAL=4u,
 F_ACK=8u, F_PWM=16u, F_SERVO_TIMEOUT=32u, F_COOLDOWN=64u,
 F_RAIL=128u, F_FEEDBACK=256u, F_INTERNAL=512u, F_LOOP=1024u };
typedef struct {
 uint32_t now_ms;
 bool enable, heartbeat, servo_request, hardware_fault_high;
 uint8_t pwm_levels;
 bool pwm_edge_seen; /* latched EXTI activity, including pulses between polls */
 bool motor_feedback, servo_feedback, host_rail_ok, internal_ok;
 bool hardware_fault_edge; /* EXTI-latched pulse even if input already recovered */
 uint8_t enable_edges, heartbeat_falls;
 bool heartbeat_event_mode;
 /* Only sensor_permit reads it; motor/servo/ready/watchdog never do. */
 bool sensor_strike_lockout; /* = sensor_guard_lockout() of the working record, re-read every pass */
} safety_input;
typedef struct { bool motor_permit, servo_permit, sensor_permit, ready, watchdog_level; } safety_output;
typedef struct {
 enum safety_stage stage;
 uint32_t faults, last_faults, last_step, stage_since, low_since,
 last_hb_fall, servo_since, servo_not_before, wd_last_fall,
 wd_high_since, changed_at, boot_at;
 uint16_t valid_hb_edges;
 bool last_hb, last_request, low_timing, have_hb, started;
 bool watchdog_high;
 safety_output output;
} safety_state;
void safety_init(safety_state *s, uint32_t now_ms);
safety_output safety_step(safety_state *s, const safety_input *i);

/* Sensor-branch strike limiter. U30 (TPS2553-1, latch-off) FAULT is on the
 * common fault bus. Either it resets the MCU through NRST (the reset releases
 * PA12 and U30 unlatches), or EXTI6 catches it and drops PA12 before NRST acts,
 * so the core only latches F_EXTERNAL (a switch-on sag of SAFE3 latches F_RAIL
 * the same way). Heartbeat revalidation or a fresh ACK start would then
 * re-enable a shorted branch indefinitely. A record kept in .noinit SRAM counts
 * one strike for each switch-on of PA12 that is followed within
 * SENSOR_STRIKE_WINDOW_MS by either an external pin reset (at boot) or a new
 * core latch with F_EXTERNAL or F_RAIL (in the running boot); a switch-on is
 * charged at most once. At SENSOR_STRIKE_LIMIT strikes the sensor stays off
 * until a power-on reset (PWRRSTF) or an invalid record; no host action,
 * including a complete ACK, clears it. SENSOR_HEALTHY_MS of continuous sensor
 * power clears strikes below the limit. Pure functions: firmware.c owns the
 * storage, RCC_CSR2 decoding and the per-pass call order. */
#define SENSOR_GUARD_MAGIC 0x53e7c011u
#define SENSOR_STRIKE_WINDOW_MS 50u
#define SENSOR_HEALTHY_MS 2000u
#define SENSOR_STRIKE_LIMIT 3u
#define SENSOR_AGE_NEVER 0xffffffffu
enum sensor_reset {
 SR_POWER, /* PWRRSTF: POR/PDR/BOR. SRAM is not trusted or read. */
 SR_PIN,   /* external NRST only: PINRSTF and no other reset flag */
 SR_OTHER  /* IWDG/WWDG/software/option-byte/low-power reset: keep, never strike */
};
#define SENSOR_TRIP_FAULTS (F_EXTERNAL|F_RAIL)
typedef struct {
 uint32_t magic;       /* SENSOR_GUARD_MAGIC */
 uint32_t rise_age_ms; /* ms since PA12 last went HIGH while that switch-on is not yet charged
                          with a strike; SENSOR_AGE_NEVER if none since boot or already charged */
 uint32_t strikes;     /* 0..SENSOR_STRIKE_LIMIT */
 uint32_t sensor_high; /* PA12 state recorded at the last main-loop pass: 0 or 1 */
 uint32_t check;       /* inverted rotate-xor of all preceding words */
} sensor_guard;
bool sensor_guard_valid(const sensor_guard *g);
/* Newest valid copy wins (slot_a is written first); both invalid => fresh record. */
sensor_guard sensor_guard_boot(const sensor_guard *slot_a, const sensor_guard *slot_b,
 enum sensor_reset cause);
/* True when the safety_step just run newly latched (stage_before was not S_LATCHED)
 * with F_EXTERNAL or F_RAIL. Reads the state only; safety_step is not changed. */
bool sensor_guard_trip(enum safety_stage stage_before, const safety_state *after);
/* Call once per main-loop pass, after safety_step and BEFORE PA12 is driven with
 * the value about to be applied. trip = sensor_guard_trip() of this pass; it is
 * charged against the switch-on recorded before this pass. */
void sensor_guard_tick(sensor_guard *g, bool sensor_high, uint32_t elapsed_ms, bool trip);
bool sensor_guard_lockout(const sensor_guard *g); /* true also for an invalid working copy */
#endif
