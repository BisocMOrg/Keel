# Keel safety MCU firmware

Firmware for **U12, the STM32C011F4P6 safety controller** on the Keel R3
baseboard, and the tools to program it from the Raspberry Pi 4 through the
on-board SW5 programming switch.

| Release image | Contents | SHA-256 |
|---|---|---|
| `release/safety_controller.hex` | Intel HEX, 4,360 data bytes at `0x08000000..0x08001107` | `1608d866ce248fc12e9eead1dd6d9f6626d3cf7842c615226223ef04c19ffe5d` |

The image uses 4,360 of 16,384 bytes of flash and 172 bytes of static RAM plus
stack. It is built from `src/`, `platform/` and `vendor/` with Arm GNU Toolchain
14.3.Rel1. A rebuild with that toolchain, on Windows or Linux, gives a
byte-identical HEX file.

**Status.** This is the release image. It was programmed through SW5 and
verified on 2026-11-07 on an R3 board: the motors arm and drive in both
directions, and the safety chain passes 17 of 17 tests (a stop drops ARMED
within 9-37 us, heartbeat loss removes motor power, and the re-arm hold-off and
the emergency-stop latch work as specified). ARMED falls 51-71 ms after the host
process is killed. The external-watchdog timing and the other measurements in
[`docs/qualification.md`](docs/qualification.md) are not yet characterised.
This is an engineering safety interlock, not a certified safety device.

Boards are assembled with U12 blank. Program it before normal operation, and
keep the wheels off the ground until it has been programmed and checked.

## What the MCU does

U12 runs from `SAFE_3V3`, its own TPS70933 regulator (U16) fed from the board's
5 V supply and independent of the Pi's 3.3 V rail. It decides whether the motors, the servo supply and the ultrasonic-sensor supply
may be powered. It has no UART, no host configuration protocol, no heap and no
secrets, and interrupt handlers never service a watchdog.

- **Motor permission.** PA11 drives `MCU_MOTOR_PERMIT`. The 74LVC1G11 gate U14
  forms `MOTOR_EN_SAFE = MCU_MOTOR_PERMIT AND MOTOR_EN (Pi GPIO26) AND MOTOR_FAULT`,
  which drives the DRV8411 (U1) `nSLEEP`. The MCU reads `MOTOR_EN_SAFE` back on PA8.
- **Servo permission.** PA7 drives `MCU_SERVO_PERMIT`. Gate U27 combines it with
  GPIO26 and the fault bus to switch the TPS2553-1 servo supply (U15). The MCU
  reads the switch enable back on PA6.
- **Sensor power.** PA12 enables the TPS2553-1 sensor supply (U30).
- **Status to the Pi.** PA4 (`MCU_READY`) reaches GPIO20 through buffer U28.
  GPIO21 shows the actual `MOTOR_EN_SAFE` level.
- **Watchdog service.** PA5 kicks the TPS3436 window watchdog U13 every 50 ms,
  from the healthy foreground loop only. U13's `WDO` pulls the fault bus low.
- **Fault bus.** `MOTOR_FAULT` is open drain with a 22 kΩ pull-up. Four parts
  drive it: the U11 supervisor, the U13 watchdog output, and the U15 and U30
  switch `FAULT` outputs. A LOW level disables both gates directly. It also resets
  U12 through R31 (1 kΩ) into `NRST`, and the MCU sees it on PB6. **The DRV8411
  `nFAULT` (U1 pin 8) is not connected:** this bus gates the driver's own
  `nSLEEP`, so a short `nFAULT` pulse at driver wake-up would disable the driver
  again and keep the motors from arming.
- **Supply check.** The internal VREFINT channel measures `SAFE_3V3`. It must
  read 3.10-3.50 V.

## Pin map (U12, TSSOP-20)

The seven Pi inputs and the fault bus reach U12 through U29 (SN74LVC541A), a
buffer powered from `SAFE_3V3`.

| Pin | Port | Net | Use |
|---:|---|---|---|
| 1 | PB7 | `SENSE_HOST_HEARTBEAT` | Pi GPIO18 heartbeat; EXTI7 falling edges |
| 2 | PC14 | `SENSE_MOTOR_RP` | Pi GPIO10 motor input; EXTI14 |
| 3 | PC15 | `SENSE_MOTOR_RN` | Pi GPIO9 motor input; EXTI15 |
| 4 | VDD/VDDA | `SAFE_3V3` | supply |
| 5 | VSS | `GND` | ground |
| 6 | PF2-NRST | `SAFETY_NRST` | reset: 47 kΩ pull-up, 1 nF, fault bus via 1 kΩ, SW5 pole 3 |
| 7 | PA0 | `SENSE_MOTOR_LN` | Pi GPIO11 motor input; EXTI0 |
| 8 | PA1 | `SENSE_MOTOR_LP` | Pi GPIO8 motor input; EXTI1 |
| 9 | PA2 | `SENSE_SERVO_REQUEST` | Pi GPIO19 servo request; level input |
| 10 | PA3 | `SENSE_MOTOR_EN` | Pi GPIO26 motor enable; EXTI3 both edges |
| 11 | PA4 | `MCU_READY` | output to Pi GPIO20; HIGH = healthy and ready |
| 12 | PA5 | `MCU_WD_SERVICE` | output to U13 `WDI` |
| 13 | PA6 | `SERVO_POWER_EN_FB` | actual servo-switch enable; level input |
| 14 | PA7 | `MCU_SERVO_PERMIT_RAW` | servo permission output (1 kΩ to gate U27) |
| 15 | PA8 | `MOTOR_EN_SAFE_FB` | actual motor enable; level input |
| 16 | PA11 | `MCU_MOTOR_PERMIT_RAW` | motor permission output (1 kΩ to gate U14) |
| 17 | PA12 | `SENSOR_POWER_EN` | sensor supply enable (U30) |
| 18 | PA13 | `SAFETY_SWDIO` | SWD data; SW5 pole 2 to Pi GPIO0 |
| 19 | PA14-BOOT0 | `SAFETY_SWCLK` | SWD clock; SW5 pole 1 to Pi GPIO1 |
| 20 | PB6 | `SENSE_MOTOR_FAULT` | fault bus; EXTI6 falling edges |

All five outputs are push-pull on GPIOA and are written together in a single BSRR
write. No internal pulls, timers or alternate functions are used. The table
matches `src/firmware.c` and the R3 schematic
([`hardware/kicad/Keel.kicad_sch`](../../hardware/kicad/Keel.kicad_sch), part U12).

## Safety policy

The policy is fixed in `src/safety_core.c`; no host command can change it.

- **Heartbeat.** Falling edges on GPIO18 must be 20-80 ms apart. Two valid
  falls qualify the heartbeat. The host places them 30-70 ms apart and emits
  them only from the application's real control loop.
- **Arming (ACK).** First GPIO26 must be LOW for at least 100 ms. It then goes
  HIGH, LOW and HIGH, each level held at least 100 ms. Throughout, all four motor
  inputs and the servo request must stay LOW and the heartbeat must remain valid.
  Any PWM edge or level during the sequence latches `F_PWM`. A wrong enable edge
  or a short phase latches `F_ACK`. Only the end of the final HIGH grants the
  motor permission. A held or re-asserted enable never re-arms by itself.
- **While armed.** The motor permission holds only while all of these are true:
  the heartbeat is valid, the fault bus is HIGH, `SAFE_3V3` is in range, the
  internal checks pass and the read-back matches the permission. Otherwise the
  firmware latches a fault and drops the permissions, READY and sensor power.
  GPIO26 LOW disarms without a fault. Any fault needs a fresh ACK.
- **Servo.** A servo request is accepted only after a successful ACK, with all
  motor inputs LOW. The lease lasts at most 500 ms and is followed by a 2 s
  cooldown, which also applies after reset. Ending the request disarms. The
  firmware never asserts motor and servo permissions together. This exclusion is
  a firmware invariant, not a separate hardware interlock.
- **Sensor power.** PA12 needs the same health conditions, but not GPIO26 or
  arming. It also waits for the first watchdog kick after each reset. A strike
  limiter kept in `.noinit` SRAM holds the sensor supply off after three trips,
  each within 50 ms of a switch-on. The lockout ends only at a power-on reset, or
  when the record is invalid, for example after SWD programming. Two seconds of
  healthy sensor power clear strikes below the limit.
- **Fault edges.** On every falling edge of the fault bus, the EXTI6 interrupt
  handler immediately drives the permissions, READY and PA12 LOW.
- **Startup.** At startup the firmware drives all outputs LOW and starts the
  IWDG (8,192 LSI ticks, about 256 ms). It then checks `NRST_MODE`, a CRC-32 of
  the image and the ADC. If any of these fails, the outputs stay LOW and neither
  watchdog is serviced, so the external watchdog resets the MCU.

Fault bits (`F_*`, `src/safety_core.h`): 1 BOOT, 2 HEARTBEAT, 4 EXTERNAL, 8 ACK,
16 PWM, 32 SERVO_TIMEOUT, 64 COOLDOWN, 128 RAIL, 256 FEEDBACK, 512 INTERNAL,
1024 LOOP. The full contract, including the host-side timing, is in
[`docs/mcu_integration.md`](docs/mcu_integration.md).

libkeel, the C library for the Pi in
[`software/libkeel/`](../../software/libkeel/), is the reference host
implementation of this contract. `keel_cycle` emits the heartbeat from the
application's control loop, `keel_request_arm` runs the acknowledgement,
`keel_set_motors` drives only while READY and ARMED are HIGH, and `keel_disarm`
and `keel_emergency_stop` stop. Its simulator steps a hash-guarded copy of
`src/safety_core.c` and `src/safety_core.h` every simulated millisecond, so the
library's tests run against this exact policy.

## Build

You need [Arm GNU Toolchain 14.3.Rel1](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
and Python 3 with `pyelftools==0.33` (`tools/requirements.txt`). The tests also
need `unicorn==2.1.4` (`tests/requirements.txt`). From the repository root:

```sh
cd firmware/safety-mcu
python3 -m venv build/.venv
build/.venv/bin/python -m pip install -r tools/requirements.txt -r tests/requirements.txt
export ARM_GNU_BIN=/path/to/arm-gnu-toolchain-14.3.rel1/bin
build/.venv/bin/python tools/build_firmware.py
```

On Windows, use `build\.venv\Scripts\python.exe` and set `$env:ARM_GNU_BIN`.
All build and test output goes to `build/`, which git ignores. The build writes
`build/safety_controller.{hex,bin,elf,map}` and `build/firmware_build.json`. It
compiles with `-Werror`, inserts the image CRC-32 into `.safety_crc`, and never
writes to `release/`. The ELF and map contain build-machine paths, so they are
not published. Compare a rebuild with the release image:
`cmp build/safety_controller.hex release/safety_controller.hex`. objcopy writes
Intel HEX with CRLF line endings on every host, and git keeps them unchanged.

Host checks, with no hardware access, from `firmware/safety-mcu`. The emulator
tests check the built image, so run `tools/build_firmware.py` first.

```sh
(cd release && sha256sum -c SHA256SUMS.txt)
bash -n programming/flash_safety_mcu.sh && bash -n programming/build_openocd.sh
python3 tests/programming/test_programming.py   # HEX validator, option-byte procedures; needs python3-tk
build/.venv/bin/python tests/unit/run_unit_tests.py           # 2,832,503 policy assertions in Unicorn
build/.venv/bin/python tests/emulator/test_firmware_boot.py   # 21 boot and 11 main-loop cases
build/.venv/bin/python tests/emulator/test_startup.py         # 5 startup regressions
```

The tests write their results to `build/`. The CI workflow
[`.github/workflows/firmware.yml`](../../.github/workflows/firmware.yml) runs the
build, these checks and the comparison of the rebuilt image with the release HEX.
The host side of the contract is tested in libkeel, against the same policy core
(`software/libkeel/tests/core/test_reference.c` and `test_servo_range.c`).

## Programming from the Pi through SW5

SW5 is a 4-pole DIP switch. **All four poles ON = PROG. All four OFF = RUN.**

| Pole | ON connects | Effect |
|---:|---|---|
| 1 | Pi GPIO1 (header 28), via 1 kΩ, to `SAFETY_SWCLK` | SWD clock |
| 2 | Pi GPIO0 (header 27), via 1 kΩ, to `SAFETY_SWDIO` | SWD data |
| 3 | Pi GPIO7 (header 26), via 1 kΩ, to `SAFETY_NRST` | reset |
| 4 | `PROG_N` to GND | disables the U13 watchdog and clamps both MCU permissions LOW (D12) |

1. **Prepare the Pi.** Stop every robot process: nothing may own GPIO0, GPIO1,
   GPIO7 or GPIO26. Disable SPI0 (`dtparam=spi=off`), because GPIO7 is SPI0 CE1.
   Disable the I2C0 / `i2c_vc` overlays. Add `force_eeprom_read=0` to
   `config.txt`, which stops the HAT EEPROM probe on GPIO0/GPIO1. Raise the wheels.
2. **Install the tools:** `gpiod`, `python3`, and `pinctrl` (raspi-utils) or
   `raspi-gpio`. You also need OpenOCD with `linuxgpiod` and STM32C0 support. Use
   either the Raspberry Pi OS package (its version contains `rpt`) or the pinned
   build below. Upstream or Debian OpenOCD 0.12.0 lacks STM32C0 flash support.
3. **Set all four SW5 poles ON (PROG)** with the board powered normally.
4. **Run the wrapper** from the repository root:

   ```sh
   sudo bash firmware/safety-mcu/programming/flash_safety_mcu.sh \
     firmware/safety-mcu/release/safety_controller.hex
   # with the pinned OpenOCD build instead of the Raspberry Pi OS package:
   sudo env KEEL_OPENOCD_PREFIX=/opt/keel-openocd \
     bash firmware/safety-mcu/programming/flash_safety_mcu.sh \
     firmware/safety-mcu/release/safety_controller.hex
   ```

   The wrapper, `programming/flash_safety_mcu.sh`, does the following:
   - checks the Pi model, the tools and the HEX file (`check_hex.py`);
   - refuses if any of those GPIO lines is claimed;
   - drives GPIO26 LOW;
   - probes the MCU with connect-under-reset and checks the pole-4 interlock;
   - fixes the option bytes if needed (see below);
   - programs and verifies the image, resets the MCU and checks that it boots
     from main flash without an IWDG reset loop;
   - releases the debug state.

   It parks GPIO0/1/7 on exit. Require `PROGRAM OK`, `VERIFY OK`,
   `FINAL OPTR OK` and `BOOT OK` in the `KEEL:` lines, and exit status 0.
   Exit codes are 1 usage/environment, 2 GPIO busy, 3 no SWD link or wrong part,
   4 option bytes, 5 program/verify and 6 boot check. A log is written to
   `/tmp/keel_flash_<time>.log`; override with `KEEL_FLASH_LOG`. `KEEL_OCD_CFG`
   selects another OpenOCD board file.
5. **Set all four SW5 poles OFF (RUN), then power-cycle the robot completely.**
   The debug state left by programming survives watchdog resets, and only a
   power-on clears it. Pole 4 OFF re-enables the external watchdog. Then confirm
   that GPIO20 (READY) goes HIGH while the host heartbeat runs, before arming.

If the wrapper stops with an error, follow its recovery message. Do not force
readout-protection removal, OTP writes or debug locks; the scripts never make
those changes. The bottom pads TP39-TP43 (`SAFE_3V3`, GND, NRST, SWDIO, SWCLK)
give an external-probe recovery path.

### Pinned OpenOCD build

On distributions without the Raspberry Pi OpenOCD package, build Raspberry Pi's
fork at commit `acff23ffd100479d6481a1155123774b79a990b9`:

```sh
bash firmware/safety-mcu/programming/build_openocd.sh /opt/keel-openocd [BUILD_DIRECTORY]
```

The build needs `git build-essential autoconf automake libtool pkg-config
libgpiod-dev libusb-1.0-0-dev python3`. Choose a prefix the build user can
write to; the script never calls `sudo`. It checks the source commit and the
`jimtcl` submodule, builds with `linuxgpiod` and the bundled Jim Tcl, and then
records a manifest with `verify_openocd.py record`. The manifest is
`<prefix>/share/keel/openocd-build.json` and holds the binary and script-tree
hashes. Before the wrapper touches a GPIO, it re-verifies that manifest and runs
`noinit` capability checks. The manifest only shows that the files match the
local build; it is not a signed attestation.

## Option bytes

The firmware needs these settings and refuses to run without `NRST_MODE`:

| Field | Required | Why |
|---|---|---|
| RDP | `0xAA` (level 0) | the scripts never change readout protection |
| nBOOT_SEL, nBOOT1, nBOOT0 | 1, 1, 1 | always boot main flash; PA14-BOOT0 is shared with SWCLK |
| NRST_MODE | 3 | bidirectional reset pin |
| RAM_PARITY_CHECK | 1 (parity check disabled) | keeps the sensor strike record across resets |

Acceptance test: `(OPTR & 0x1F4000FF) == 0x1F4000AA`. For example, the observed
`0xFFFFFEAA` passes. If a bit in `0x1F400000` is wrong, the wrapper writes only
those bits; it never writes RDP. It re-checks the pole-4 interlock just before
the write and confirms after reload that no other bit changed. After programming
and reset it reads OPTR again and reports `FINAL OPTR OK 0x...`.
`--keep-option-bytes` refuses instead of writing.

To read OPTR without programming, first set SW5 to PROG and drive GPIO26 LOW
(`sudo pinctrl set 26 op dl`), then run:

```sh
sudo openocd -s /usr/share/openocd/scripts -c "set KEEL_GPIOCHIP 0" \
  -f firmware/safety-mcu/programming/openocd_keel.cfg -c init -c keel_probe -c shutdown
```

It prints `KEEL: DEV_ID=0x443 REV_ID=... FLASH_KB=16 OPTR=0x........`. The probe
leaves the MCU halted, so set SW5 to RUN and power-cycle afterwards. For the
pinned build, use `<prefix>/bin/openocd -s <prefix>/share/openocd/scripts`.

## Diagnostic RAM words

The release image keeps its diagnostic state at these RAM addresses. They can be
read over SWD with the same PROG setup (SW5 ON, GPIO26 LOW, nothing on
GPIO0/1/7). Another image may place them elsewhere.

| Symbol | Address | Meaning |
|---|---|---|
| `startup_diagnostic` | `0x20000060` | 1-5: last startup stage passed (5 = ADC ready). Bit 31 set: that stage failed. |
| `firmware_diagnostic` | `0x20000064` | Bits 0-10: latched faults. Bits 16-17: sensor strikes. Bit 31: strike lockout. `0x48415244` marks a HardFault. |
| `measured_safe_mv` | `0x20000068` | last `SAFE_3V3` reading in mV |
| `controller` | `0x2000001c` | core state (`stage` +0, `faults` +4, `last_faults` +8) |
| strike record | `0x20000084`, `0x20000098` | two 20-byte copies in `.noinit` |

After any debug session, set SW5 to RUN and power-cycle.

## Files

| Path | Contents |
|---|---|
| `release/` | release image `safety_controller.hex` and its SHA-256, `SHA256SUMS.txt` |
| `src/` | firmware sources: `firmware.c` (pins, peripherals, interrupt handlers, main loop) and the safety policy `safety_core.c`, `safety_core.h` |
| `platform/` | `stm32c011f4.ld` (memory layout, `.noinit` and `.safety_crc` sections) and `freestanding_runtime.c` (`memset`, `memcpy` and the startup hooks of the `-nostdlib` link) |
| `vendor/cmsis/` | Arm CMSIS-Core headers and their licence, `LICENSE.txt` |
| `vendor/st/` | ST STM32C0 device headers, system file and startup code, and their licence, `LICENSE.txt` |
| `tools/` | `build_firmware.py` and its Python requirements |
| `programming/flash_safety_mcu.sh` | programming wrapper, run on the Pi |
| `programming/openocd_keel.cfg` | OpenOCD board file: `linuxgpiod` SWD on GPIO0/1/7 and the `keel_*` procedures |
| `programming/check_hex.py` | strict offline Intel HEX check (main flash only, vectors present) |
| `programming/build_openocd.sh`, `programming/verify_openocd.py` | pinned OpenOCD build and its manifest check |
| `tests/unit/` | policy unit tests `test_safety_core.c` and `run_unit_tests.py`, which builds them for Cortex-M0+ and runs them in Unicorn |
| `tests/emulator/` | tests of the built image in Unicorn: boot and main-loop cases (`test_firmware_boot.py`), startup regressions (`test_startup.py`) |
| `tests/programming/` | offline tests of `check_hex.py` and the option-byte procedures (`test_programming.py`) |
| `tests/requirements.txt` | Python requirements of the unit and emulator tests |
| `docs/` | integration contract (`mcu_integration.md`); hardware results and the characterisation tests (`qualification.md`) |
| `build/` | build and test output, not tracked |

## Licences

- Files written for Keel are released under the **MIT** licence, Copyright (c)
  2026 BisocM (see the repository `LICENSE`). These are all files in `src/`,
  `platform/`, `tools/`, `programming/` and `tests/`, and the documentation in
  this folder.
- **STMicroelectronics CMSIS device files** in `vendor/st/` are under Apache-2.0
  (`vendor/st/LICENSE.txt`): `stm32c011xx.h`, `stm32c0xx.h`,
  `system_stm32c0xx.c/.h` and `startup_stm32c011xx.s`. The startup file is
  modified: it adds a dummy SRAM read for erratum ES0569 2.2.5, marked in the
  file.
- **Arm CMSIS-Core headers** in `vendor/cmsis/` are under Apache-2.0
  (`vendor/cmsis/LICENSE.txt`): `cmsis_compiler.h`, `cmsis_gcc.h`,
  `cmsis_version.h`, `core_cm0plus.h` and `mpu_armv7.h`.

The release image contains code compiled from the ST system and startup files.
Keep these licence files with any copy of the image. OpenOCD is not part of this
repository: `programming/build_openocd.sh` downloads and builds it, and it keeps
its own licence.
