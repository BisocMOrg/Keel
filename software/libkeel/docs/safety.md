<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Safety for application developers

This guide explains what libkeel does to keep the robot safe, what it expects from your program, and how to
read every status it returns. The rules are the same in C, C++, Java, C# and Python. The header
`include/keel/keel.h` is the contract and [spec.md](spec.md) the full specification; the tables below are
generated from the header and from the table the test suite replays, and a test fails if they ever differ.

## 1. Three layers of protection

1. **The hardware gate.** The motor bridges are enabled only while the safety MCU's motor permit AND the Pi's
   ENABLE line (BCM26) are both HIGH. The gate's output is read back on ARMED (BCM21).
2. **The safety MCU.** It grants motor permission only after an arming handshake and only while it sees a
   heartbeat from the Pi: a falling edge on BCM18 every 20-80 ms. Without a heartbeat it removes motor, servo and
   sensor power on its own. It reports its health on READY (BCM20).
3. **libkeel.** It emits the heartbeat only from your control loop, never from a background thread, so the
   heartbeat certifies that your loop is alive. It checks READY and ARMED before every motion command and on
   every tick, enforces command timeouts, and performs a Category-0 stop (enable off, motors coasting) on any
   inconsistency.

If your process crashes or is killed, the heartbeat stops and the MCU removes motor power. The design bound is
about 86 ms after the last heartbeat; on the robot, ARMED fell 51-71 ms after the process was killed.

## 2. States

| State | Motor enable | Heartbeat | Motors | Leave with |
|---|---|---|---|---|
| `DISARMED` | off | running (sensor power available) | coast | `keel_request_arm` |
| `ARMING` | handshake levels | running | coast | completes by itself in about 0.6-0.85 s of ticks; `keel_disarm` |
| `ARMED` | on | running | coast, drive or brake | `keel_disarm`, a servo burst, a fault |
| `SERVO` | on (servo permit, motors locked out) | running | coast | ends by itself at the lease end; `keel_servo_end` |
| `FAULTED` | off | stopped | coast | `keel_clear_fault` (unless the cause is sticky) |

A device starts `DISARMED`. Nothing ever arms automatically: arming is an explicit operator or application
decision, and so is clearing a fault.

## 3. The control loop contract

Your program owns a loop that runs every 10 ms on one dedicated OS thread:

```
next = now_us()
loop:
    next += 10 ms
    ok = step()                                    # read sensors, decide, check your own I/O
    rc = keel_cycle({cycle_id: ++id, health: ok ? KEEL_HEALTH_ALL : bits actually verified,
                     command, left, right})
    handle rc by its class (section 6)
    keel_sleep_until(next)
```

- **Timing.** Gaps of up to 70 ms between ticks are accepted while arming, armed or in a servo burst; a longer
  gap, or a heartbeat fall more than 74 ms after the previous one, is a fault (`TICK_LATE`, `HEARTBEAT_LATE`).
  Plan for a worst-case stall of 40 ms (`KEEL_STALL_BUDGET_MS`) inside your loop. While DISARMED a late tick only
  restarts the heartbeat.
- **Health.** Pass `KEEL_HEALTH_ALL` only when this cycle's work really completed. Anything else stops the robot
  on purpose (`HEALTH_FLAGS`).
- **cycle_id** strictly increases over the device's lifetime.
- **One thread.** The first successful tick after open or `keel_clear_fault` makes its thread the control owner;
  a tick from any other thread is a fault (`TICK_THREAD_CHANGED`). Use a dedicated OS thread, never a timer pool,
  async task or Java virtual thread.
- **The device clock.** Use `keel_now_us` and `keel_sleep_until`, not your language's sleep: the same loop then
  runs unchanged on the simulator's virtual clock.
- **Resend commands.** Every drive or brake command lives for the command timeout (500 ms by default, 20-1000 ms
  with `keel_open_options.command_timeout_ms`) and then coasts, without a fault (`KEEL_SNAP_COMMAND_EXPIRED`).

## 4. Moving

- `keel_request_arm` starts the handshake from DISARMED; keep cycling until the state is `ARMED`. Motion
  commands sent while arming are refused, never queued.
- `keel_set_motors(dev, left, right)` or a `KEEL_CYCLE_DRIVE` command: signed permille, -1000..1000, positive is
  forward. Zero coasts.
- **Reversal.** A wheel that changes direction without having been idle for 20 ms is not reversed at once: both
  motors coast, the call returns OK with `KEEL_SNAP_REVERSAL_PENDING`, and the first tick or command 20 ms later
  applies it.
- `keel_brake` holds all bridge inputs HIGH for the command timeout. No stop path ever brakes.
- A duty outside -1000..1000 is refused with `KEEL_E_INVALID_ARGUMENT` and, while ARMED, the motors coast: a
  refused motion command can never leave the wheels driving.

## 5. Stopping

| Call | Effect | Latches | From | Leave with |
|---|---|---|---|---|
| `keel_set_motors(dev, 0, 0)` | both motors coast, stays ARMED | no | any thread | the next command |
| `keel_brake` | bridges braked for the command timeout, stays ARMED | no | the loop thread | the next command |
| `keel_disarm` | Category 0: enable off at once, motors coast; the heartbeat continues | no | any thread | `keel_request_arm` |
| `keel_emergency_stop` | as `keel_disarm`, then the heartbeat stops and the device latches FAULTED (`USER_STOP`) | yes | any thread | `keel_clear_fault`, then arm |
| a detected fault | as `keel_emergency_stop`, with the detected cause | yes | - | `keel_clear_fault` unless sticky |
| `keel_close` | as `keel_emergency_stop`, then LEDs off and every line released | - | any thread | reopen |

`keel_disarm` lowers the enable line before it waits for any lock, so a stop is never delayed by another thread.
Map your stop button and Ctrl-C or SIGTERM to `keel_emergency_stop`; use `keel_disarm` in normal
program flow. Always end with `keel_close`.

**Never call libkeel from a signal handler.** No libkeel function is async-signal-safe: a handler that calls
`keel_emergency_stop` can deadlock on a lock the interrupted thread holds, and then nothing lowers the enable line
until the MCU's heartbeat window closes. In the handler only set a `volatile sig_atomic_t` flag (or write to a
pipe); the loop checks the flag and calls `keel_emergency_stop`, or a dedicated thread waits for the signal with
`sigwait()` or a `signalfd` and calls it. The tools in `tools/` use the flag pattern (`tools/tool_common.c`).

## 6. Threads: stop from anywhere, go only from the loop

- **Stop calls** are accepted from every thread in every state: `keel_disarm`, `keel_emergency_stop`,
  `keel_set_motors(dev, 0, 0)`, `keel_servo_end`, `keel_close`.
- **Go calls** from a thread other than the control owner are refused with `KEEL_E_CONFLICT` and change nothing:
  `keel_request_arm`, `keel_clear_fault`, non-zero `keel_set_motors`, `keel_brake`, `keel_servo_start`,
  `keel_servo_set_pulse`.
- **Queries, LEDs, buttons and ranging** work from any thread. `keel_range_measure` blocks for up to 35 ms, so the
  control owner may not call it while arming, armed or in a servo burst (`KEEL_E_CONFLICT`); use
  `keel_range_start` and `keel_range_poll` from the loop instead.

A user-interface thread posts its intent (for example a target speed) to the loop, which sends it with the next
`keel_cycle`.

## 7. Statuses and what to do about them

Every status belongs to exactly one class, and the class alone tells your program what state the device is in:

| Class | The device after the call | Your program |
|---|---|---|
| OK | did what was asked | continue |
| REJECTED | unchanged (except that a refused motion command coasts while ARMED); `snapshot.last_rejection` is set | log it; fix the call. From `keel_cycle`, `KEEL_E_INVALID_ARGUMENT` and `KEEL_E_UNSUPPORTED` mean a programming error: stop the loop |
| FAULT | `FAULTED`: stopped, heartbeat off; `keel_get_fault` has the cause | stop the motion logic, report the cause, wait for an operator |
| PERIPHERAL | unchanged: an LED or ultrasonic failure | log it; the robot is still safe to drive |
| SETUP | `keel_open` created no device; the open report says why | show the report's hint |
| HANDLE | the handle is closed, stale or belongs to another process | stop using it |

Two call-specific rules: every non-OK result of `keel_open` means "no device, see the report", and the status of
`keel_close` is diagnostic only (log it, never treat it as an error to handle). `keel_status_class()` gives the
class of any value; values the library does not define read as FAULT.

<!-- status-table:begin -->
| Status | Value | Class | Meaning |
|---|---|---|---|
| `KEEL_OK` | 0 | OK | Success. |
| `KEEL_E_INVALID_ARGUMENT` | -1 | REJECTED | NULL pointer, `struct_size` below the 1.0 size, non-zero reserved field, unknown enumerator or flag, or a value out of range. |
| `KEEL_E_IO` | -2 | FAULT | A synchronous control-path I/O operation failed: ENABLE, heartbeat, servo request or button-LED write, READY/ARMED/button read, or a motor command written by the calling thread. The OS error is in `keel_fault.os_error`. From `keel_close()`: the final stop could not be confirmed (diagnostic only; the MCU heartbeat timeout still applies). |
| `KEEL_E_STATE` | -3 | REJECTED | Not allowed in the current state: motion, brake or servo start while not ARMED, a servo pulse change without a burst, or `keel_request_arm()` within `KEEL_REARM_HOLDOFF_MS` of a fault latch (`KEEL_SNAP_REARM_HOLDOFF` is then set). |
| `KEEL_E_HEALTH` | -4 | FAULT | The application reported a health value other than `KEEL_HEALTH_ALL`, `cycle_id` did not strictly increase, or a tick came from a thread other than the control owner. |
| `KEEL_E_DEADLINE` | -5 | FAULT | Application timing failed while arming, armed or in a servo burst: a tick gap over `KEEL_TICK_WINDOW_MS` or a heartbeat fall later than `KEEL_HEARTBEAT_LATE_MS`. |
| `KEEL_E_FEEDBACK` | -6 | FAULT | READY/ARMED feedback disagrees with the commanded state (READY lost, ARMED lost or unexpected, early ARMED during the acknowledgement, servo grant missing, both permits at once). |
| `KEEL_E_AGAIN` | -7 | REJECTED | Temporarily refused; retry later: servo cooldown running or motors coasting for less than `KEEL_SERVO_IDLE_MS`; ultrasonic measurement pending, less than `KEEL_RANGE_INTERVAL_MS` since the last trigger, or ECHO still HIGH. Never used for safety refusals. |
| `KEEL_E_NO_MEMORY` | -8 | SETUP | Memory allocation failed while opening. |
| `KEEL_E_PWM` | -9 | FAULT | The motor PWM generator failed on its own thread: worker overrun, sustained lateness, stall, or a motor-line write error inside the worker. |
| `KEEL_E_FAULTED` | -10 | FAULT | The device is latched FAULTED and nothing was done; or, from `keel_clear_fault()`, the recorded cause is sticky. See `keel_get_fault()`. Also the record status of a `keel_emergency_stop()` latch. |
| `KEEL_E_IN_USE` | -11 | SETUP | A GPIO line is held by another process, another libkeel device, or a kernel function such as SPI0 (see the open report). |
| `KEEL_E_PERMISSION` | -12 | SETUP | Permission denied on a device node; see the open report hint (keel-setup adds the user to the right groups). |
| `KEEL_E_NOT_FOUND` | -13 | SETUP | A required device, kernel interface or backend is missing: no GPIO chip labelled pinctrl-bcm2711 (not a Pi 4), a kernel without GPIO character device v2, or a backend not built into this library. |
| `KEEL_E_UNSUPPORTED` | -14 | REJECTED | Not supported here: a capability this device lacks (see `keel_get_capabilities()`), a simulator call on hardware, or an input struct larger than this library knows with a non-zero tail. |
| `KEEL_E_CONFLICT` | -15 | REJECTED | Conflicts with the control owner or an active instruction: a go call from a thread other than the control owner; motion or brake during a servo burst; servo start while a motor is driven or braked, a reversal is pending or a burst is active; arming during a servo burst; `keel_range_measure()` from the control owner while ARMING, ARMED or SERVO. |
| `KEEL_E_PERIPHERAL` | -16 | PERIPHERAL | A non-safety peripheral failed (RGB LED driver I2C or the ultrasonic lines). The safety state is unchanged. |
| `KEEL_E_ACK_TIMEOUT` | -17 | FAULT | Arming did not complete within `KEEL_ARM_TIMEOUT_MS` (READY never HIGH, or ARMED never rose). |
| `KEEL_E_INVALID_HANDLE` | -18 | HANDLE | NULL, closed, stale or forged device handle; or the device was closed while this call waited. |
| `KEEL_E_NOT_OWNER` | -19 | HANDLE | Called from a process that did not open the device (for example a forked child). Nothing was touched. |
| `KEEL_E_INTERNAL` | -20 | FAULT | Internal invariant violated: the clock went backwards, a backend interlock refused a command, or a backend broke its contract. Indicates a defect; the cause is sticky until close. |
| `KEEL_E_SETUP_IO` | -21 | SETUP | An I/O operation failed while opening (requesting lines, driving outputs to their safe levels, starting the PWM worker); everything acquired was released. See the open report. |
<!-- status-table:end -->

## 8. Faults

A FAULT-class detection records the fault before anything is cleaned up, performs the Category-0 stop with the
heartbeat stopped, latches `FAULTED` and returns the primary status. The record (`keel_get_fault`) holds the
status, the cause, the state and arming phase, the last input levels, the OS error, a cause-specific `detail`, the
time and whether the stop itself succeeded (`cleanup_status`). Only the first fault since the last clear is
recorded.

**Recovery:**

1. Find out why: `keel_get_fault`, then the table below.
2. `keel_clear_fault` from the loop thread re-runs the stop, resets the backend, keeps the record readable with
   `KEEL_FAULT_CLEARED` and returns to `DISARMED`. The next tick restarts the heartbeat; sensor power returns
   about 0.1-0.2 s later.
3. `keel_request_arm` is refused with `KEEL_E_STATE` (`KEEL_SNAP_REARM_HOLDOFF`) for 1 s after the latch, so a
   retry loop cannot re-arm a faulting robot at full rate.
4. **Sticky causes** (`ARMED_UNEXPECTED`, `INTERLOCK`, `INTERNAL`, `CLOCK`, `HAL_CONTRACT`) mean a board or
   library defect: `keel_clear_fault` returns `KEEL_E_FAULTED` until the device is closed and reopened. Do not
   drive the robot again before the defect is understood.

Never clear or re-arm automatically after a fault.

<!-- cause-table:begin -->
| # | Cause | Status | Sticky | Meaning and `detail` |
|---|---|---|---|---|
| 0 | `NONE` | - | - | No fault recorded. |
| 1 | `HEALTH_FLAGS` | `KEEL_E_HEALTH` | no | Health flags were not `KEEL_HEALTH_ALL`; detail = missing bits. |
| 2 | `CYCLE_NOT_INCREASING` | `KEEL_E_HEALTH` | no | cycle_id did not increase; detail = rejected cycle_id. |
| 3 | `TICK_THREAD_CHANGED` | `KEEL_E_HEALTH` | no | A tick came from a thread other than the control owner; detail = the calling thread's id. |
| 4 | `TICK_LATE` | `KEEL_E_DEADLINE` | no | Tick gap over `KEEL_TICK_WINDOW_MS` while arming, armed or in a servo burst; detail = gap in us. |
| 5 | `HEARTBEAT_LATE` | `KEEL_E_DEADLINE` | no | Heartbeat fall more than `KEEL_HEARTBEAT_LATE_MS` after the previous one while arming, armed or in a servo burst; detail = interval in us. |
| 6 | `READY_LOST` | `KEEL_E_FEEDBACK` | no | READY fell after the first acknowledgement level, while armed or in a servo burst. |
| 7 | `ARMED_LOST` | `KEEL_E_FEEDBACK` | no | ARMED fell while armed. |
| 8 | `ARMED_UNEXPECTED` | `KEEL_E_FEEDBACK` | yes | ARMED HIGH more than `KEEL_ARMED_GRACE_MS` after GPIO26 went LOW; the AND gate no longer removes enable. |
| 9 | `ACK_NO_READY` | `KEEL_E_ACK_TIMEOUT` | no | READY never HIGH during the arming window. |
| 10 | `ACK_NO_ARMED` | `KEEL_E_ACK_TIMEOUT` | no | ARMED never rose after the final acknowledgement level. |
| 11 | `ACK_FEEDBACK` | `KEEL_E_FEEDBACK` | no | ARMED rose early during the acknowledgement, or a second stale completion was seen; detail = `keel_ack_phase`. |
| 12 | `SERVO_NOT_GRANTED` | `KEEL_E_FEEDBACK` | no | ARMED still HIGH (or READY LOW) `KEEL_SERVO_GRANT_MS` after the servo request. |
| 13 | `DUAL_PERMIT` | `KEEL_E_FEEDBACK` | no | ARMED HIGH during a granted servo burst (both MCU permits). |
| 14 | `PWM_OVERRUN` | `KEEL_E_PWM` | no | One PWM worker wake more than 30 ms late; detail = lateness in us. |
| 15 | `PWM_DEGRADED` | `KEEL_E_PWM` | no | PWM excess lateness over 20 ms within 100 ms; detail = excess in us. |
| 16 | `PWM_STALL` | `KEEL_E_PWM` | no | The PWM worker's scheduled wake is more than 25 ms in the past while a drive or brake command is unexpired; detail = age in us. |
| 17 | `PWM_WRITE` | `KEEL_E_PWM` | no | A motor-line write inside the PWM worker failed; os_error set. |
| 18 | `GPIO_WRITE` | `KEEL_E_IO` | no | A control output write failed; detail = logical output mask. |
| 19 | `GPIO_READ` | `KEEL_E_IO` | no | A control input read failed. |
| 20 | `MOTOR_WRITE` | `KEEL_E_IO` | no | A synchronous motor command write failed. |
| 21 | `CLOCK` | `KEEL_E_INTERNAL` | yes | The device clock went backwards or failed; detail = step back in us. |
| 22 | `INTERLOCK` | `KEEL_E_INTERNAL` | yes | A backend interlock refused a conflicting low-level command (library defect). |
| 23 | `HAL_CONTRACT` | `KEEL_E_INTERNAL` | yes | A backend returned a status outside the FAULT class on a safety path, or flagged a fault it could not describe; detail = the raw status, sign-extended to 64 bits. |
| 24 | `INTERNAL` | `KEEL_E_INTERNAL` | yes | Another internal invariant; detail = internal code. |
| 25 | `USER_STOP` | `KEEL_E_FAULTED` | no | `keel_emergency_stop()` was called. |
<!-- cause-table:end -->

Common causes and their usual remedy:

- `TICK_LATE`, `HEARTBEAT_LATE`: the loop stalled. Look for blocking I/O, logging, garbage-collection pauses or
  CPU contention in `step()`.
- `READY_LOST`: the MCU withdrew permission (it saw a fault of its own, or lost the heartbeat).
- `ACK_NO_READY`: the MCU never reported ready: it is not programmed, its RUN switch is off, or it is latched.
- `PWM_OVERRUN`, `PWM_DEGRADED`: the motor PWM thread was starved. Check that the real-time limit applies
  (`KEEL_CAP_REALTIME_PWM`, `snapshot.timing_class`) and look for CPU-heavy work at high priority.
- `GPIO_WRITE`, `GPIO_READ`, `MOTOR_WRITE`: an OS call failed; `os_error` has the errno.

## 9. Sensors and LEDs

- **Ranging** results are statuses, never sentinel distances: `VALID` carries 20-4000 mm; `NO_ECHO` means
  "nothing detected", never "the path is clear"; `UNAVAILABLE` means the sensor is unpowered or unplugged;
  `INVALID` means the measurement was disturbed. The sensor is powered by the MCU while the heartbeat runs, so
  ranging works while DISARMED but not while FAULTED. Ranging never latches a fault.
- **LEDs** work in every state, including FAULTED (show an error colour). An I2C failure returns
  `KEEL_E_PERIPHERAL` and never stops the robot. A missing LED driver clears `KEEL_CAP_RGB` at open instead of
  failing it.

## 10. The request-versus-state table

The table gives the result of every call in every state, from the control owner and from another thread. The
arbitration test replays every reachable cell against the simulator and checks the status, the recorded cause,
the state afterwards, the side effects and that the heartbeat keeps running after every refusal.

Legend:
- "owner" is the control owner, or any thread while none is recorded; "other" is another thread of the process
  while an owner is recorded; "any" means both agree.
- `E_X` means `KEEL_E_X`. "-> STATE" is a state change; a cause in brackets is the cause the call records.
- "HB" says whether the heartbeat keeps running afterwards, given on-time ticks.
- Cells assume working I/O; an I/O failure is a fault with its own cause.
- An invalid handle always gives `E_INVALID_HANDLE` and touches nothing.

<!-- arbitration-table:begin -->
| Call | Condition | Thread | DISARMED | ARMING | ARMED | SERVO | FAULTED |
|---|---|---|---|---|---|---|---|
| `keel_tick` | on time, health ALL, cycle_id increasing [1] | owner | OK; sample, pulse; HB yes | OK; sample, pulse, ack; HB yes | OK; sample, pulse, lease; HB yes | OK; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_tick` | on time, health ALL, cycle_id increasing | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | more than 70 ms after the previous tick, which ran in the same state [2] | owner | OK; sample, pulse, hb restart; HB yes | E_DEADLINE (TICK_LATE) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_DEADLINE (TICK_LATE) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_DEADLINE (TICK_LATE) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | more than 70 ms after the previous tick, which ran in the same state | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | heartbeat fall lands more than 74 ms after the previous fall (previous tick in the same state) [3] | owner | OK; sample, pulse, hb restart; HB yes | E_DEADLINE (HEARTBEAT_LATE) -> FAULTED; sample, pulse, Cat-0 stop, hb stop, latch; HB no | E_DEADLINE (HEARTBEAT_LATE) -> FAULTED; sample, pulse, Cat-0 stop, hb stop, latch; HB no | E_DEADLINE (HEARTBEAT_LATE) -> FAULTED; sample, pulse, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | heartbeat fall lands more than 74 ms after the previous fall (previous tick in the same state) | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | health is not KEEL_HEALTH_ALL | owner | E_HEALTH (HEALTH_FLAGS) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (HEALTH_FLAGS) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (HEALTH_FLAGS) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (HEALTH_FLAGS) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | health is not KEEL_HEALTH_ALL | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | cycle_id not greater than the last accepted one | owner | E_HEALTH (CYCLE_NOT_INCREASING) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (CYCLE_NOT_INCREASING) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (CYCLE_NOT_INCREASING) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (CYCLE_NOT_INCREASING) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_tick` | cycle_id not greater than the last accepted one | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | in is NULL or in->struct_size is below the 1.0 size [4] | any | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB no |
| `keel_cycle` | in is larger than this library knows and its tail is not zero [5] | any | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB no |
| `keel_cycle` | valid in, command NONE (tick on time) [6] | owner | OK; sample, pulse; HB yes | OK; sample, pulse, ack; HB yes | OK; sample, pulse, lease; HB yes | OK; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command NONE (tick on time) | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command DRIVE with a valid non-zero duty [7] | owner | E_STATE; sample, pulse; HB yes | E_STATE; sample, pulse, ack; HB yes | OK; sample, pulse, lease, drive; HB yes | E_CONFLICT; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command DRIVE with a valid non-zero duty | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command DRIVE with (0, 0) | owner | OK; sample, pulse; HB yes | OK; sample, pulse, ack; HB yes | OK; sample, pulse, coast; HB yes | OK; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command DRIVE with (0, 0) | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command BRAKE | owner | E_STATE; sample, pulse; HB yes | E_STATE; sample, pulse, ack; HB yes | OK; sample, pulse, lease, brake; HB yes | E_CONFLICT; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in, command BRAKE | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in; unknown command or flag, duty out of range, or bad out [8] | owner | E_INVALID_ARGUMENT; sample, pulse; HB yes | E_INVALID_ARGUMENT; sample, pulse, ack; HB yes | E_INVALID_ARGUMENT; sample, pulse, coast; HB yes | E_INVALID_ARGUMENT; sample, pulse, grant; HB yes | E_FAULTED; sample; HB no |
| `keel_cycle` | valid in; unknown command or flag, duty out of range, or bad out | other | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_HEALTH (TICK_THREAD_CHANGED) -> FAULTED; sample, Cat-0 stop, hb stop, latch; HB no | E_FAULTED; sample; HB no |
| `keel_request_arm` | no fault latched within the last 1 s | owner | OK -> ARMING; ack; HB yes | OK; none; HB yes | OK; none; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_request_arm` | no fault latched within the last 1 s | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_request_arm` | a fault latched less than 1 s ago [9] | owner | E_STATE; hold-off; HB yes | unreachable | unreachable | unreachable | E_FAULTED; none; HB no |
| `keel_request_arm` | a fault latched less than 1 s ago | other | E_CONFLICT; none; HB yes | unreachable | unreachable | unreachable | E_CONFLICT; none; HB no |
| `keel_disarm` | any [10] | any | OK; Cat-0 stop; HB yes | OK -> DISARMED; Cat-0 stop; HB yes | OK -> DISARMED; Cat-0 stop; HB yes | OK -> DISARMED; Cat-0 stop, servo end; HB yes | OK; Cat-0 stop, hb stop; HB no |
| `keel_emergency_stop` | any [11] | any | OK (USER_STOP) -> FAULTED; Cat-0 stop, hb stop, latch; HB no | OK (USER_STOP) -> FAULTED; Cat-0 stop, hb stop, latch; HB no | OK (USER_STOP) -> FAULTED; Cat-0 stop, hb stop, latch; HB no | OK (USER_STOP) -> FAULTED; Cat-0 stop, hb stop, latch, servo end; HB no | OK; Cat-0 stop, hb stop; HB no |
| `keel_clear_fault` | recorded cause not sticky (or not FAULTED) [12] | owner | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK -> DISARMED; Cat-0 stop, clear; HB yes |
| `keel_clear_fault` | recorded cause not sticky (or not FAULTED) | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_clear_fault` | sticky cause (ARMED_UNEXPECTED, INTERLOCK, INTERNAL, CLOCK, HAL_CONTRACT) [13] | owner | unreachable | unreachable | unreachable | unreachable | E_FAULTED; none; HB no |
| `keel_clear_fault` | sticky cause (ARMED_UNEXPECTED, INTERLOCK, INTERNAL, CLOCK, HAL_CONTRACT) | other | unreachable | unreachable | unreachable | unreachable | E_CONFLICT; none; HB no |
| `keel_set_motors` | (0, 0) | any | OK; none; HB yes | OK; none; HB yes | OK; coast; HB yes | OK; none; HB yes | OK; none; HB no |
| `keel_set_motors` | valid non-zero duty, no reversal [14] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | OK; drive; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_set_motors` | valid non-zero duty, no reversal | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_set_motors` | valid; a wheel changes direction without 20 ms idle [15] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | OK; reversal; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_set_motors` | valid; a wheel changes direction without 20 ms idle | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_set_motors` | a duty outside -1000..1000 [16] | any | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; coast; HB yes | E_INVALID_ARGUMENT; none; HB yes | E_INVALID_ARGUMENT; none; HB no |
| `keel_brake` | KEEL_CAP_BRAKE [17] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | OK; brake; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_brake` | KEEL_CAP_BRAKE | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_servo_start` | KEEL_CAP_SERVO; valid arguments; motors coasting 20 ms; cooldown elapsed [18] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | OK -> SERVO; servo request; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_servo_start` | KEEL_CAP_SERVO; valid arguments; motors coasting 20 ms; cooldown elapsed | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_servo_start` | KEEL_CAP_SERVO; a motor driven or braked, or a reversal pending [19] | owner | unreachable | unreachable | E_CONFLICT; none; HB yes | unreachable | unreachable |
| `keel_servo_start` | KEEL_CAP_SERVO; a motor driven or braked, or a reversal pending | other | unreachable | unreachable | E_CONFLICT; none; HB yes | unreachable | unreachable |
| `keel_servo_start` | KEEL_CAP_SERVO; cooldown running, or motors coasting for less than 20 ms | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | E_AGAIN; none; HB yes | E_CONFLICT; none; HB yes | E_FAULTED; none; HB no |
| `keel_servo_start` | KEEL_CAP_SERVO; cooldown running, or motors coasting for less than 20 ms | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_servo_start, keel_servo_set_pulse` | no KEEL_CAP_SERVO (keel_r3 on R3: J5 not fitted) [20] | any | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | E_UNSUPPORTED; none; HB yes | unreachable | E_UNSUPPORTED; none; HB no |
| `keel_servo_set_pulse` | KEEL_CAP_SERVO; pulse in range [21] | owner | E_STATE; none; HB yes | E_STATE; none; HB yes | E_STATE; none; HB yes | OK; pulse width; HB yes | E_FAULTED; none; HB no |
| `keel_servo_set_pulse` | KEEL_CAP_SERVO; pulse in range | other | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB no |
| `keel_servo_end` | any | any | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK -> DISARMED; servo end; HB yes | OK; none; HB no |
| `keel_range_start` | KEEL_CAP_RANGE; sensor power PERMITTED or CONFIRMED; idle; spacing elapsed; ECHO LOW [22] | any | OK; trigger, read; HB yes | OK; trigger, read; HB yes | OK; trigger, read; HB yes | OK; trigger, read; HB yes | unreachable |
| `keel_range_start` | KEEL_CAP_RANGE; sensor power OFF or UNKNOWN [23] | any | OK; unavailable; HB yes | OK; unavailable; HB yes | OK (loop stalled); unavailable; HB no | OK (loop stalled); unavailable; HB no | OK; unavailable; HB no |
| `keel_range_start` | KEEL_CAP_RANGE; sensor powered; measurement pending, under 60 ms since the trigger, or ECHO HIGH | any | E_AGAIN; none; HB yes | E_AGAIN; none; HB yes | E_AGAIN; none; HB yes | E_AGAIN; none; HB yes | unreachable |
| `keel_range_poll` | KEEL_CAP_RANGE | any | OK; result; HB yes | OK; result; HB yes | OK; result; HB yes | OK; result; HB yes | OK; result; HB no |
| `keel_range_measure` | KEEL_CAP_RANGE; spacing elapsed; sensor powered unless FAULTED [24] | owner | OK; trigger, wait <= 35 ms, read; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | E_CONFLICT; none; HB yes | OK; unavailable; HB no |
| `keel_range_measure` | KEEL_CAP_RANGE; spacing elapsed; sensor powered unless FAULTED | other | OK; trigger, wait <= 35 ms, read; HB yes | OK; trigger, wait <= 35 ms, read; HB yes | OK; trigger, wait <= 35 ms, read; HB yes | OK; trigger, wait <= 35 ms, read; HB yes | OK; unavailable; HB no |
| `keel_set_rgb, keel_set_rgb_all, keel_set_rgb_frame` | KEEL_CAP_RGB; valid arguments [25] | any | OK; I2C frame; HB yes | OK; I2C frame; HB yes | OK; I2C frame; HB yes | OK; I2C frame; HB yes | OK; I2C frame; HB no |
| `keel_set_button_leds` | mask within KEEL_BUTTON_ALL [26] | any | OK; button LEDs; HB yes | OK; button LEDs; HB yes | OK; button LEDs; HB yes | OK; button LEDs; HB yes | OK; button LEDs; HB no |
| `keel_take_buttons` | valid out [27] | any | OK; read, take edges; HB yes | OK; read, take edges; HB yes | OK; read, take edges; HB yes | OK; read, take edges; HB yes | OK; read, take edges; HB no |
| `keel_read_safety_inputs` | valid out | any | OK; read; HB yes | OK; read; HB yes | OK; read; HB yes | OK; read; HB yes | OK; read; HB no |
| `keel_get_state, keel_get_snapshot, keel_get_fault, keel_get_capabilities, keel_get_rgb, keel_now_us` | valid out [28] | any | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK; none; HB yes | OK; none; HB no |
| `keel_sleep_until` | deadline at most 10 s ahead [29] | any | OK; sleep; HB yes | OK; sleep; HB yes | OK; sleep; HB yes | OK; sleep; HB yes | OK; sleep; HB no |
| `keel_sim_set, keel_sim_get_state` | simulator device; valid item [30] | any | OK; sim; HB yes | OK; sim; HB yes | OK; sim; HB yes | OK; sim; HB yes | OK; sim; HB no |
| `keel_close` | any [31] | any | OK -> closed; Cat-0 stop, hb stop, release; HB no | OK -> closed; Cat-0 stop, hb stop, release; HB no | OK -> closed; Cat-0 stop, hb stop, release; HB no | OK -> closed; Cat-0 stop, hb stop, servo end, release; HB no | OK -> closed; Cat-0 stop, hb stop, release; HB no |
| `any call` | from a process that did not open the device (forked child) [32] | any | E_NOT_OWNER; none; HB yes | E_NOT_OWNER; none; HB yes | E_NOT_OWNER; none; HB yes | E_NOT_OWNER; none; HB yes | E_NOT_OWNER; none; HB no |

Notes:

1. ARMING becomes ARMED when the acknowledgement completes; SERVO becomes DISARMED when the lease ends. The first successful tick records the control owner.
2. A tick whose previous tick ran while DISARMED (the first after keel_request_arm) restarts the heartbeat instead of failing.
3. A fall predicted more than 70 ms after the previous one gives the same result before pulsing.
4. No tick runs; the heartbeat ages until the next valid tick.
5. No tick runs; bindings treat this as fatal for the loop.
6. Identical to the on-time keel_tick row; out receives the snapshot.
7. Reversal deferral applies as for keel_set_motors.
8. Checked after the tick; while ARMED the motors coast. Bindings treat this as fatal.
9. Arming cannot be in progress within 1 s of a latch, so only DISARMED and FAULTED occur.
10. ENABLE falls before any lock is taken; the whole safe stop is re-run in every state.
11. Record status E_FAULTED, cause USER_STOP; in FAULTED the existing record is kept.
12. The heartbeat resumes at the next tick, which records the new control owner.
13. A sticky record exists only while FAULTED; close and reopen to recover.
14. The command lives for the command timeout (default 500 ms), then coasts without a fault.
15. The first tick or command at least 20 ms after the coast applies the pending command.
16. Arguments are checked before the thread, so any thread's invalid command coasts while ARMED.
17. Without the capability: KEEL_E_UNSUPPORTED from any thread, coasting while ARMED.
18. The pulse starts on the tick that confirms the grant (ARMED 0, READY 1 within 10 ms).
19. Motors only drive, brake or hold a pending reversal while ARMED.
20. The capability is checked before the thread and the state; keel_servo_end stays a no-op.
21. Never extends the lease.
22. FAULTED has no heartbeat, so the sensor is never powered there.
23. On-time ticks keep READY HIGH and the heartbeat qualified in ARMED and SERVO. There the sensor is unpowered only after the loop stalled past 70 ms (or the MCU dropped READY since the last tick, which the fresh READY read finds): the state shown is the cached one until the next tick latches, and the heartbeat has already stopped.
24. The control owner may not block while a heartbeat deadline is armed.
25. An I2C failure returns KEEL_E_PERIPHERAL and never latches.
26. A GPIO failure is KEEL_E_IO and latches (the lines share the control request).
27. A backend latch found by the read is recorded first (for example E_PWM after a worker overrun).
28. Cached reads: no I/O.
29. Returns KEEL_E_INVALID_HANDLE if keel_close starts during the sleep.
30. On other devices: KEEL_E_UNSUPPORTED, nothing done.
31. The Category-0 stop runs under the control lock before the LED and range locks are taken.
32. Checked atomically before any lock; the parent's device is untouched.
<!-- arbitration-table:end -->
