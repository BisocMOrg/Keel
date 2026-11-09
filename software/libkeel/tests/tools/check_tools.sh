#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Smoke checks of the installed tools: keel-demo and keel-live against the
# simulator (no hardware), their precondition and usage exits, and
# keel-doctor against fake system trees.
#
# Usage: check_tools.sh <directory with the tools> <case>
set -eu

[ "$#" -eq 2 ] || { echo "usage: $0 <tool directory> <case>" >&2; exit 2; }
bin=$1
name=$2
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT INT TERM
log="$out/log"

fail() {
    echo "FAIL: $*" >&2
    echo "---- output ----" >&2
    cat "$log" >&2 2>/dev/null || true
    exit 1
}

# run <expected exit status> <command...>: output goes to $log.
run() {
    expected=$1
    shift
    status=0
    "$@" >"$log" 2>&1 || status=$?
    [ "$status" -eq "$expected" ] || fail "$* exited $status, expected $expected"
}

has() {
    grep -q -- "$1" "$log" || fail "missing: $1"
}

passed() {
    has "\"test\":\"$1\",\"status\":\"PASS\""
}

clean_summary() {
    has '"type":"summary"'
    has '"failed":0'
}

# A fake Raspberry Pi 4 tree: everything but the GPIO chip in place.
pi4_root() {
    root="$out/pi4"
    mkdir -p "$root/proc/device-tree/soc/spi@7e204000" "$root/proc/sys/kernel" \
        "$root/sys/class/i2c-dev" "$root/dev" "$root/sys/devices/system/cpu/cpu0/cpufreq" \
        "$root/sys/devices/platform/soc/soc:firmware"
    printf 'Raspberry Pi 4 Model B Rev 1.5\000' >"$root/proc/device-tree/model"
    printf 'raspberrypi,4-model-b\000brcm,bcm2711\000' >"$root/proc/device-tree/compatible"
    printf 'disabled\000' >"$root/proc/device-tree/soc/spi@7e204000/status"
    echo "6.8.0-1012-raspi" >"$root/proc/sys/kernel/osrelease"
    : >"$root/dev/i2c-1"
    echo ondemand >"$root/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
    echo 0x0 >"$root/sys/devices/platform/soc/soc:firmware/get_throttled"
}

# A tree with every problem the doctor knows about.
bad_root() {
    root="$out/bad"
    mkdir -p "$root/proc/device-tree/soc/spi@7e204000" "$root/proc/sys/kernel" "$root/dev" \
        "$root/sys/devices/system/cpu/cpu0/cpufreq" "$root/sys/devices/platform/soc/soc:firmware"
    printf 'Raspberry Pi 5 Model B Rev 1.0\000' >"$root/proc/device-tree/model"
    printf 'raspberrypi,5-model-b\000brcm,bcm2712\000' >"$root/proc/device-tree/compatible"
    printf 'okay\000' >"$root/proc/device-tree/soc/spi@7e204000/status"
    echo "5.4.0-1080-raspi" >"$root/proc/sys/kernel/osrelease"
    : >"$root/dev/gpiochip0"
    : >"$root/dev/spidev0.0"
    echo powersave >"$root/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
    echo 0x50005 >"$root/sys/devices/platform/soc/soc:firmware/get_throttled"
}

check() {
    has "\"check\":\"$1\",\"status\":\"$2\""
}

case $name in
demo_drive)
    run 0 "$bin/keel-demo" --simulator --drive 300 -300 200
    has "state: ARMING"
    has "state: ARMED"
    has "state: DISARMED"
    ;;
demo_duration)
    run 0 "$bin/keel-demo" --simulator --duration 300
    ;;
demo_usage)
    run 2 "$bin/keel-demo" --drive 2000 0 100
    run 2 "$bin/keel-demo" --simulator --gpio-chip /dev/gpiochip0
    run 0 "$bin/keel-demo" --help
    has "Usage:"
    ;;
live_preconditions)
    # Without the operator's confirmations nothing is opened.
    run 77 "$bin/keel-live" --io
    has '"test":"hardware_preconditions","status":"SKIP"'
    has "no GPIO or I2C access"
    run 77 "$bin/keel-live" --motors --programmed
    has "wheels-raised"
    ;;
live_usage)
    run 2 "$bin/keel-live" --io --timeout 10
    run 2 "$bin/keel-live" --simulator --kill-test
    run 2 "$bin/keel-live" --io --leds
    run 2 "$bin/keel-live" --pwm-jitter 0
    run 0 "$bin/keel-live" --help
    has "Usage:"
    ;;
live_io)
    run 0 "$bin/keel-live" --simulator --io
    passed hardware_open
    passed io_health_loop
    passed io_ready_disarmed
    clean_summary
    ;;
live_leds)
    run 0 "$bin/keel-live" --simulator --leds --hold 50
    passed RGB6_blue
    passed indicator_Y
    has '"status":"NEEDS_OBSERVATION"'
    clean_summary
    ;;
live_buttons)
    run 0 "$bin/keel-live" --simulator --buttons --timeout 20
    passed button_A_press_release
    passed button_B_press_release
    passed button_X_press_release
    passed button_Y_press_release
    clean_summary
    ;;
live_safety)
    run 0 "$bin/keel-live" --simulator --safety
    for test in zero_duty_ack armed_feedback stop_host_state stop_hardware_feedback \
        zero_duty_rearm mcu_timeout_feedback_after_120ms expired_health_rejection \
        rearm_refused_while_faulted clear_fault rearm_holdoff rearm_after_clear \
        emergency_stop_latch emergency_stop_feedback clear_after_emergency_stop; do
        passed "$test"
    done
    clean_summary
    ;;
live_motors)
    run 0 "$bin/keel-live" --simulator --motors
    for motion in left_forward left_reverse right_forward right_reverse; do
        passed "motor_${motion}_stop_feedback"
        passed "motor_${motion}_command"
    done
    clean_summary
    ;;
live_range)
    run 0 "$bin/keel-live" --simulator --range
    passed range_sensor_power
    passed range_measurements
    has "Median 500 mm"
    clean_summary
    ;;
live_pwm_jitter)
    # Real-time priority is usually refused here, so the verdicts depend on
    # the machine; the run itself and a consistent exit status are checked.
    status=0
    "$bin/keel-live" --simulator --pwm-jitter 2 >"$log" 2>&1 || status=$?
    has '"event":"pwm_jitter_summary"'
    has '"test":"pwm_jitter_p99_9"'
    if grep -q '"status":"FAIL"' "$log"; then expected=1; else expected=0; fi
    [ "$status" -eq "$expected" ] || fail "exit status $status, expected $expected"
    has "\"exit_code\":$expected}"
    ;;
doctor_pi4_tree)
    pi4_root
    run 1 "$bin/keel-doctor" --json --root "$root"
    check platform PASS
    check kernel PASS
    check spi0 PASS
    check i2c_dev PASS
    check i2c_bus PASS
    check cpufreq PASS
    check throttling PASS
    check gpio_chip FAIL
    check gpio_lines SKIP
    has '"type":"summary"'
    ;;
doctor_bad_tree)
    bad_root
    run 1 "$bin/keel-doctor" --json --root "$root"
    check platform FAIL
    check kernel FAIL
    check spi0 FAIL
    check i2c_dev WARN
    check i2c_bus WARN
    check cpufreq WARN
    check throttling WARN
    check gpio_chip FAIL
    has "is not a GPIO character device"
    has "Raspberry Pi 5"
    # Every check that did not pass names a remedy.
    if grep -E '"status":"(FAIL|WARN)"' "$log" | grep -q '"remedy":""'; then
        fail "a failed check has no remedy"
    fi
    ;;
doctor_user)
    # --user judges an account from the databases (install.sh runs as root).
    pi4_root
    me=$(id -un)
    run 1 "$bin/keel-doctor" --json --root "$root" --user "$me"
    check i2c_bus PASS
    check sched_fifo SKIP
    has "run keel-doctor as $me"
    has "\"check\":\"groups\",\"status\":\"[A-Z]*\",\"detail\":\"user $me, device groups:"
    has "\"check\":\"rtprio\",\"status\":\"WARN\",\"detail\":\"no /etc/security/limits.d"
    mkdir -p "$root/etc/security/limits.d"
    : >"$root/etc/security/limits.d/90-keel-rtprio.conf"
    run 1 "$bin/keel-doctor" --json --root "$root" --user "$me"
    if getent group keel 2>/dev/null | grep -Eq "[:,]$me(,|$)"; then
        check rtprio PASS
    else
        has "not in the keel group"
    fi
    # The node is the caller's own 0644 file: another account may not write
    # it, whatever this process may do.
    chmod 0644 "$root/dev/i2c-1"
    if [ "$me" != nobody ] && id nobody >/dev/null 2>&1; then
        run 1 "$bin/keel-doctor" --json --root "$root" --user nobody
        check i2c_bus WARN
    fi
    run 2 "$bin/keel-doctor" --user "no-such-user-$$"
    has "no such user"
    ;;
doctor_text)
    pi4_root
    run 1 "$bin/keel-doctor" --root "$root"
    has "keel-doctor: "
    has "remedy: "
    run 0 "$bin/keel-doctor" --help
    run 2 "$bin/keel-doctor" --bogus
    ;;
*)
    echo "unknown case: $name" >&2
    exit 2
    ;;
esac
echo "PASS $name"
