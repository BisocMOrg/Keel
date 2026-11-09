#!/usr/bin/env bash
# flash_safety_mcu.sh -- Keel R3 baseboard: program the safety MCU U12
# (STM32C011F4P6) from the Raspberry Pi through the SW5 PROG switch.
#
#   Usage: sudo ./flash_safety_mcu.sh [--keep-option-bytes] FIRMWARE.hex
#
# Before running: robot powered, SW5 all four poles ON ("PROG"), robot/host
# program stopped (nothing may own BCM0, BCM1, BCM7 or BCM26).
#
# Sequence: Pi check -> tool check -> line check -> MOTOR_EN (BCM26) LOW ->
# SWD probe + SW5 pole-4 interlock (refuse unless an STM32C011 answers) ->
# option bytes -> program -> verify -> reset -> boot/IWDG check -> next step.
#
# Exit codes: 0 ok, 1 usage/environment, 2 GPIO busy, 3 no SWD link / wrong
# part, 4 option bytes, 5 program/verify, 6 boot check.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG="${KEEL_OCD_CFG:-$SCRIPT_DIR/openocd_keel.cfg}"
LOG="${KEEL_FLASH_LOG:-/tmp/keel_flash_$(date +%Y%m%d_%H%M%S).log}"

BCM_SWDIO=0
BCM_SWCLK=1
BCM_NRST=7
BCM_MOTOR_EN=26

KEEP_OPTR=0
HEX=""

RECOVER='set all four SW5 poles OFF (RUN), then power-cycle the robot'
# Codes >= 3 happen after OpenOCD has touched the MCU: it may be left halted,
# and only a power-on clears the debug halt (it survives watchdog resets).
die() {
	local code=$1; shift
	echo "ERROR: $*" >&2
	if [ "$code" -ge 3 ]; then
		echo "       If you stop here: $RECOVER (the MCU may be left halted until power-on)." >&2
	fi
	echo "Log: $LOG" >&2
	exit "$code"
}
info() { echo "==> $*"; }
warn() { echo "WARNING: $*" >&2; }

for a in "$@"; do
	case "$a" in
		--keep-option-bytes) KEEP_OPTR=1 ;;
		-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
		-*) die 1 "unknown option $a" ;;
		*) [ -z "$HEX" ] || die 1 "only one firmware image is accepted"; HEX="$a" ;;
	esac
done
[ -n "$HEX" ] || die 1 "usage: $0 [--keep-option-bytes] FIRMWARE.hex"
[ -r "$HEX" ] || die 1 "cannot read $HEX"
HEX="$(readlink -f "$HEX")"
[ -r "$CFG" ] || die 1 "OpenOCD config not found: $CFG"
: >"$LOG"

# ------------------------------------------------------------ 1. on a Pi? --
MODEL="$(tr -d '\0' </proc/device-tree/model 2>/dev/null || true)"
case "$MODEL" in
	"Raspberry Pi 4"*|"Raspberry Pi Compute Module 4"*) ;;
	"Raspberry Pi 5"*|"Raspberry Pi Compute Module 5"*) warn "$MODEL: Keel R3 is qualified on Pi 4; continuing" ;;
	"") die 1 "not running on a Raspberry Pi (no /proc/device-tree/model)" ;;
	*) die 1 "unsupported board '$MODEL' (needs Raspberry Pi 4)" ;;
esac
info "Board: $MODEL"

# ------------------------------------------------------------ 2. tools -----
for t in gpiodetect gpioinfo python3; do
	command -v "$t" >/dev/null 2>&1 || die 1 "missing '$t' (sudo apt install openocd gpiod python3)"
done
if command -v pinctrl >/dev/null 2>&1; then PINTOOL=pinctrl
elif command -v raspi-gpio >/dev/null 2>&1; then PINTOOL=raspi-gpio
else die 1 "missing 'pinctrl' (sudo apt install raspi-utils)"; fi

# A supplied build must be pinned, manifest-verified, and pass no-init capability
# checks. The explicit prefix is not a string bypass of the distro-package guard.
if [ -n "${KEEL_OPENOCD_PREFIX:-}" ]; then
    OCD="$KEEL_OPENOCD_PREFIX/bin/openocd"
    OCD_SCRIPTS="$KEEL_OPENOCD_PREFIX/share/openocd/scripts"
    python3 "$SCRIPT_DIR/verify_openocd.py" verify "$KEEL_OPENOCD_PREFIX" --config "$CFG" ||
        die 1 "supplied OpenOCD build failed provenance/capability verification"
else
    command -v openocd >/dev/null 2>&1 || die 1 "OpenOCD missing; use build_openocd.sh in this folder"
    OCD="$(readlink -f "$(command -v openocd)")"
    OCD_PKG="$(dpkg-query -W -f='${Version}' openocd 2>/dev/null || true)"
    case "$OCD_PKG" in
        *rpt*) ;;
        *) die 1 "OpenOCD '$OCD_PKG' is not a Raspberry Pi archive build; build the pinned source with build_openocd.sh in this folder and supply KEEL_OPENOCD_PREFIX" ;;
    esac
    dpkg-query -L openocd | grep -Fx "$OCD" >/dev/null || die 1 "selected OpenOCD is not owned by the checked package"
    OCD_SCRIPTS=/usr/share/openocd/scripts
    [ -r "$OCD_SCRIPTS/target/stm32c0x.cfg" ] || die 1 "STM32C0 target configuration missing"
    ADAPTERS="$("$OCD" -c noinit -c 'adapter list' -c shutdown 2>&1)" || die 1 "adapter capability check failed"
    [[ $ADAPTERS == *linuxgpiod* ]] || die 1 "this OpenOCD was built without linuxgpiod"
    info "OpenOCD package $OCD_PKG (Raspberry Pi build)"
fi

# ------------------------------------------------------- 3. hex sanity -----
# Intel HEX only, data only in main flash 0x08000000..0x08003FFF (16 KB,
# STM32C011F4); refuse any option-byte/OTP/system-memory records. The image
# end is checked again against the probed flash size after the SWD probe.
IMG_INFO="$(python3 "$SCRIPT_DIR/check_hex.py" "$HEX")" || die 1 "firmware image rejected (see message above)"
# Tcl braces quote the image argument below; reject names that could escape them.
case "$HEX" in
    *'{'*|*'}'*|*'\'*|*$'\n'*|*$'\r'*) die 1 "unsupported firmware path characters" ;;
esac
img_re='^(image: .*) end=(0x[0-9a-f]+)$'
[[ $IMG_INFO =~ $img_re ]] || die 1 "hex check gave no image range: $IMG_INFO"
IMG_END="${BASH_REMATCH[2]}"
info "${BASH_REMATCH[1]}"

# ------------------------------------------------------ 4. GPIO lines ------
GDETECT="$(gpiodetect)" || die 1 "gpiodetect failed"
CHIP="$(awk '/\[pinctrl-bcm2711\]|\[pinctrl-rp1\]/ {print $1; exit}' <<<"$GDETECT")"
[ -n "$CHIP" ] || die 1 "no pinctrl-bcm2711/pinctrl-rp1 gpiochip found"
CHIPNUM="${CHIP#gpiochip}"
GINFO="$(gpioinfo -c "$CHIP" 2>/dev/null || gpioinfo "$CHIP")"
for l in $BCM_SWDIO $BCM_SWCLK $BCM_NRST $BCM_MOTOR_EN; do
	row="$(printf '%s\n' "$GINFO" | grep -E "^[[:space:]]*line[[:space:]]+$l:" || true)"
	if [[ $row == *'[used]'* || $row == *consumer=* ]]; then
		die 2 "BCM$l is claimed by another driver/process: $row
       BCM7 busy -> dtparam=spi=on (CE1); BCM0/1 busy -> i2c0/i2c_vc overlay or HAT driver;
       BCM26 busy -> stop the robot program. Then retry."
	fi
done
info "GPIO lines free on $CHIP"

CONFIG_TXT=/boot/firmware/config.txt; [ -r "$CONFIG_TXT" ] || CONFIG_TXT=/boot/config.txt
if [ -r "$CONFIG_TXT" ] && ! grep -qE '^[[:space:]]*force_eeprom_read[[:space:]]*=[[:space:]]*0' "$CONFIG_TXT"; then
	warn "add 'force_eeprom_read=0' to $CONFIG_TXT (stops the boot-time HAT EEPROM probe on SWDIO/SWCLK)"
fi
[ -e /dev/i2c-0 ] && warn "/dev/i2c-0 exists (i2c0mux active): do not touch I2C bus 0 while flashing"

# ------------------------------------------------ 5. MOTOR_EN (BCM26) LOW --
pin_low() {
	local st
	if [ "$PINTOOL" = pinctrl ]; then
		pinctrl set "$1" op dl && st="$(pinctrl get "$1")" && [[ $st == *'| lo'* ]]
	else
		raspi-gpio set "$1" op dl && st="$(raspi-gpio get "$1")" && [[ $st == *'level=0'* ]]
	fi
}
pin_park() { # $1 = bcm, $2 = pu|pd
	if [ "$PINTOOL" = pinctrl ]; then pinctrl set "$1" ip "$2"
	else raspi-gpio set "$1" ip "$2"; fi
}
pin_low $BCM_MOTOR_EN || die 2 "could not drive MOTOR_EN (BCM26) low"
info "MOTOR_EN (BCM26) driven LOW and read back LOW"

# Park the SWD/NRST lines like the target's own pulls whatever happens next.
park_lines() {
	pin_park $BCM_SWDIO pu || true   # PA13 SWDIO has an internal pull-up
	pin_park $BCM_SWCLK pd || true   # PA14 SWCLK/BOOT0 has an internal pull-down
	pin_park $BCM_NRST  pu || true   # NRST: R30 47k pull-up; never float low
}
trap park_lines EXIT

ocd() { # run OpenOCD with the board config plus the given commands
	local out rc=0
	out="$("$OCD" -s "$OCD_SCRIPTS" -c "set KEEL_GPIOCHIP $CHIPNUM" -f "$CFG" -c init "$@" -c shutdown 2>&1)" || rc=$?
	printf '%s\n' "$out" >>"$LOG"
	printf '%s\n' "$out" | grep '^KEEL:' || true
	OCD_OUT="$out"
	return "$rc"
}
has() { [[ $OCD_OUT == *"$1"* ]]; }   # match the last OpenOCD output, no pipe

# ------------------------------------------------------- 6. SWD probe ------
info "Probing SWD link (connect under reset) and SW5 pole-4 interlock..."
ocd -c keel_probe || die 3 "OpenOCD probe command failed"
if ! has 'KEEL: PROBE OK'; then
	die 3 "SWD probe failed (no STM32C011 DEV_ID 0x443, or the MCU was reset while halted).
       Check: all FOUR SW5 poles ON (PROG), including pole 4 -- otherwise the U13
       watchdog is running or MOTOR_FAULT is low and resets the MCU mid-flash;
       board powered (SAFE_3V3 present); nothing else on BCM0/1/7.
       Recovery path: bottom pads TP39-TP43 with an external probe."
fi
probe_re='PROBE OK optr=(0x[0-9a-fA-F]+) flash_kb=([0-9]+)'
[[ $OCD_OUT =~ $probe_re ]] || die 3 "probe answered but OPTR/flash size could not be parsed (see log)"
OPTR="${BASH_REMATCH[1]}"
FLASH_KB="${BASH_REMATCH[2]}"
if (( IMG_END > 0x08000000 + FLASH_KB * 1024 )); then
	die 1 "image ends at $IMG_END, past the probed ${FLASH_KB} KB flash (0x08000000 + ${FLASH_KB}*1024).
       Nothing was written; $RECOVER (the MCU is left halted until power-on)."
fi

# ------------------------------------------------------ 7. option bytes ----
if [ $(( OPTR & 0xFF )) -ne $(( 0xAA )) ]; then
	die 4 "readout protection active (OPTR=$OPTR). Not handled by this script; do not force it."
fi
if [ $(( OPTR & 0x1F400000 )) -ne $(( 0x1F400000 )) ]; then
	if [ "$KEEP_OPTR" -eq 1 ]; then
		die 4 "OPTR=$OPTR: need nBOOT_SEL=1 nBOOT1=1 nBOOT0=1 NRST_MODE=3 RAM_PARITY_CHECK=1 (rerun without --keep-option-bytes)"
	fi
	info "Setting boot/reset/parity option bits (OPTR=$OPTR)..."
	ocd -c keel_fix_optr || die 4 "OpenOCD option update command failed"
	has 'KEEL: OPTR OK' || die 4 "option byte update failed"
fi
info "Option bytes OK (main-flash boot, NRST bidirectional, SRAM parity disabled, RDP level 0)"

# ------------------------------------------ 8. program + verify + reset ----
info "Programming $(basename "$HEX")..."
ocd -c "keel_program {$HEX}" || die 5 "OpenOCD programming command failed"
has 'KEEL: PROGRAM OK' || die 5 "flash programming failed"
has 'KEEL: VERIFY OK'  || die 5 "verify failed: flash does not match $HEX"
# This value is freshly read after programming/reset, never copied from PROBE.
final_optr_re='FINAL OPTR OK (0x[0-9a-fA-F]+)'
[[ $OCD_OUT =~ $final_optr_re ]] || die 4 "fresh final option-byte verification missing"
FINAL_OPTR="${BASH_REMATCH[1]}"
(( (FINAL_OPTR & 0x1F4000FF) == 0x1F4000AA )) || die 4 "final OPTR=$FINAL_OPTR fails boot/reset/parity/RDP verification"
if has 'IWDG reset loop'; then
	die 6 "firmware self-test failed (IWDG reset loop): the image starts but never feeds the IWDG"
fi
has 'KEEL: BOOT OK'    || die 6 "MCU did not start from main flash after reset"
has 'KEEL: DEBUG RELEASED' || warn "debug state not released (see log); the power cycle below clears it"

park_lines
trap - EXIT
pin_low $BCM_MOTOR_EN || true
info "Programmed, verified and restarted. MOTOR_EN left LOW. Log: $LOG"
cat <<EOF

NEXT STEP: $RECOVER.
  (Pole 4 re-enables the U13 watchdog and releases the permit clamp; poles
   1-3 disconnect the Pi from SWD/NRST. The power cycle clears the debug
   state the programmer leaves behind and re-reads the option bytes.)
EOF
