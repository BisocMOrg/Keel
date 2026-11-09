# Keel R3 bring-up

This checklist brings up a newly assembled Keel R3 board. Work through it in order.

It assumes the board was assembled as the BOM says:
- the safety MCU U12 is fitted **blank**;
- SW5 is delivered with all four poles OFF (RUN), with its tape seal on.

Other references:
- `hardware/docs/test_points.md`: the pad map. All 28 pads are on the underside, in six labelled rows. The silkscreen
  prints the TP number and a short name, for example `10 FLT`.
- `firmware/`: the U12 image, the Pi flash script and its OpenOCD configuration.
- `software/`: the host software for the Pi.

## 0. What you need

- **Bench and instruments:**
  - a bench supply set to 5.10 V, with a 0.1-0.3 A current limit;
  - a multimeter;
  - a 4-channel scope with short ground springs;
  - 2.54 mm pogo pins or fine probe tips;
  - an inline USB power meter.
- **Pi 4 and mounting:**
  - a Raspberry Pi 4 with its own 5.1 V / 3 A USB-C supply;
  - a 2x20 **booster header**: the stack must be at least 18.2 mm, 19.5 mm nominal;
  - four M2.5 standoffs, at most 6.0 mm across corners, for H3-H6.
- **For the functional tests:**
  - a stand that keeps the wheels off the ground;
  - an HC-SR04 ultrasonic module;
  - a servo, and a 1x3 2.54 mm header for J5 (not fitted);
  - an electronic load;
  - polyimide tape.
- **The Pi's microSD card**, prepared before the Pi first boots on the board:
  - The robot's operating system, 64-bit Ubuntu 26.10 for Raspberry Pi, with `gpiod`, `raspi-utils` and `python3`
    installed and OpenOCD from the pinned build (`firmware/safety-mcu/README.md`, "Pinned OpenOCD build"). The same
    card then runs the robot. Raspberry Pi OS with its `openocd`, `gpiod`, `raspi-utils` and `python3` packages also
    serves for the bring-up, but the host software's release packages are built for Ubuntu 26.10.
  - OpenOCD must be the pinned build or the Raspberry Pi OS package (version `0.12.0+rpt...`). Upstream and Debian
    0.12.0 cannot flash the STM32C011, and the flash script refuses them.
  - In the Pi's `config.txt`, add `force_eeprom_read=0`. Otherwise the HAT EEPROM probe toggles GPIO0/GPIO1, which
    are SWDIO/SWCLK while SW5 is at PROG.
  - Keep SPI0 off (no `dtparam=spi=on`; it claims GPIO7). Do not use `dtparam=i2c_vc=on` or `dtoverlay=i2c0`.
    Enable I2C1 (`dtparam=i2c_arm=on`) for the LED and expansion tests.
  - Copy the U12 image, the flash script and its OpenOCD configuration from `firmware/` into one folder. Check the
    image checksum against the one published in `firmware/`.

## 1. On arrival: inspect, no power

- **SW5:** all four poles OFF (RUN), tape seal intact.
- **D1-D4 (Wurth 150060WS75000):** the body mark (anode) is at pad 2, the end away from the silkscreen bracket. A
  reversed LED stays dark and is not damaged.
- **U17-U26 (TPD4E05U06 arrays; U24 and U26 are on the bottom):** each pin-1 dot sits on the silk dot. A part rotated
  180 degrees still solders and the board still works, but that connector has no ESD protection. X-ray does not show
  it.
- **D8-D11 (TPD1E05U06):** orientation as on the assembly drawing. A reversed part clamps its button line low, so the
  button reads as always pressed.
- **U2 (eFuse):** no bridge between pad 5 (PI_5V) and pad 6 (SYS_5V). They are 0.2 mm apart. A bridge silently
  bypasses the eFuse.
- **Fine pitch and small parts:**
  - C5 (0201, next to U1 pins 12/13) is not tombstoned.
  - U10 (0.5 mm pitch), U12 and U29 (TSSOP-20) have no bridges. Look hardest at U12 pins 13/14 and 15/16. Each pair
    is an MCU permit output next to its read-back input. If a pair bridges, R48/R49 keep the gate in control, and test
    5.2a catches it.
- **C4 and the mechanical joints:**
  - C4: + on pad 1 (VM_MOTOR).
  - SW1-SW4: full fillets. Their pads are about 1.1 mm from the board flanks.
  - J2, J3, X1, X3: tab fillets. Give each a light tug.
- **PI1 and J1:** seated flat, with full fill. PI1 pins 2 and 4 are the board's 5 V input and carry all of its current.
- **Bottom side:**
  - C21 under U12, R35/R36 under U1, and RN9 are present.
  - RGB1-RGB6 match sheet 2 of `hardware/assembly/Keel_R3_assembly_drawing.pdf`.
  - The 28 test pads are clean and bare.

## 2. Unpowered checks with a meter (SW5 OFF)

| Between | Expected | If not |
|---|---|---|
| TP1 (PI_5V) and TP2 (SYS_5V) | **not** about 0 Ohm | U2 pads 5/6 bridged: the eFuse is bypassed |
| TP2 and TP6 (GND) | about 1 kOhm, rising while capacitors charge (R12 bleed) | 0 Ohm = short on SYS_5V |
| TP39 (SAFE_3V3) and TP40 (GND) | about 470 Ohm (R25 bleed) | 0 Ohm = short on SAFE_3V3 |
| TP45 (SERVO_5V) and TP47 (GND) | about 1 kOhm (R26) | |
| J1 pin 1 (SENSOR_5V, right-hand pin) and J1 pin 4 | about 10 kOhm (R46) | |
| TP1, TP3 (VM), TP4 (PI_3V3), TP5 (EXP_3V3) to GND | no short (well above 100 Ohm, climbing) | |
| TP1 and TP7 / TP8 (I2C) | open | |
| PI1 pin 28 and TP43, pin 27 and TP42, pin 26 and TP41 | open with SW5 OFF; about 1.0 kOhm (RN11) with SW5 ALL ON | SW5 or RN11 fault |

Set SW5 back to ALL OFF after the last row.

## 3. First power: no Pi, U12 blank, SW5 at RUN

A factory-blank STM32C011 runs ST's system bootloader. Its pins were checked against ST AN2606 (STM32C011 section; on
TSSOP-20 its USART1 sits on PA11/PA12) and against the netlist:
- **PA11 = USART1 TX, driven HIGH.** PA11 is MCU_MOTOR_PERMIT_RAW, so a blank U12 at RUN grants the motor permit.
  Motor power then depends only on MOTOR_EN (Pi GPIO26, R45 pull-down) and the fault bus. U13 pulls the bus LOW for
  about 25 ms in every 150 ms. Nothing drives MOTOR_EN in this section (no Pi), and section 4 runs no host software
  before U12 is flashed. The servo permit (PA7) is not a bootloader pin and stays LOW.
- **PA12 = USART1 RX, input with pull-up.** PA12 is SENSOR_POWER_EN. The pull-up against RN10 (10k) gives 0.5-0.95 V,
  inside U30's 0.66-1.1 V enable threshold. So SENSOR_5V may come on while U12 is blank. This is harmless.
- **PB6/PB7 = I2C1 (open drain).** They face U29's outputs. The bootloader pulls SDA only to acknowledge its own
  address, and every fault-bus LOW resets U12, so no address can be clocked in. There is no contention.

Take the readings below, power off, and then flash U12 (section 4).

- **Supply:** a bench supply at **5.10 V, limit 0.1 A**, into PI1 pins 2/4 (+) and 6 (GND), or into TP1 (+) and TP6
  (GND). TP1 and TP6 are 2.54 mm apart; do not bridge them.
- **Never** use the bench supply while a Pi is fitted. Never feed TP39.

| Point | Expected (blank U12, SW5 at RUN) |
|---|---|
| Supply current | About 15-20 mA (estimate: 5 mA in R12, 7 mA in R25, plus quiescent current). Record it. Above 50 mA, stop. |
| TP2 SYS_5V | Within about 60 mV of the supply. It ramps up slowly (C6 sets dV/dt; about 12 ms). |
| TP3 VM_MOTOR | Just below TP2: the D5 Schottky drop at the sleeping driver's microamp load. |
| TP39 SAFE_3V3 | 3.234-3.366 V. If it reads 0 V, look for flux or leakage at U16 pin 3 (EN floats on an internal pull-up). |
| TP5 EXP_3V3 | 3.3 V. |
| TP20 I2C_OVP_REF | 3.45 V +/- 0.2 V. |
| TP18 / TP19 EXP_I2C | About 3.05 V each (1.8k to SAFE_3V3 against the 22k pull-down). |
| TP4 PI_3V3 | About 0 V: there is no phantom rail. |
| TP9, TP21, TP45, TP46 | About 0 V (MOTOR_EN, HOST_ARMED, SERVO_5V, SERVO_POWER_EN). |
| RN10 pin 2 (MCU_MOTOR_PERMIT) | About 3.0 V between TP10 pulses: the bootloader holds PA11 HIGH. TP21 at 0 V shows that U14 still holds the driver asleep, because MOTOR_EN is LOW. |
| J1 pin 1 (SENSOR_5V) | 0 V, or on / pulsing with the U12 resets on some boards (PA12 pull-up). After flashing, the firmware controls it. |
| TP12 / TP13 | 0 V. |
| TP44 WDI | No pulses: a blank U12 never kicks the watchdog. |
| **TP10 MOTOR_FAULT** | **Repeating LOW pulses of 22.5-27.5 ms, one every ~125-181 ms** (a 102.4-153.6 ms watchdog frame plus the pulse). This is U13 timing out: the hardware half of the watchdog test (5.3a). The first LOW after power-up can be U11 releasing SYS_5V (12-28 ms). |
| TP41 NRST | Follows each TP10 LOW, through R31. Each pulse resets U12. |

**Now, with the power still on, set SW5 ALL ON (PROG).**
- TP10 stops pulsing and stays HIGH. PROG pulls U13's WD_EN low, which disables the watchdog and releases WDO.
- The supply current rises by about 1.5 mA (R37 2.2k to GND through pole 4), plus about 3 mA through R28 and D12.
- RN10 pin 2 drops from about 3.0 V to about 0.3 V. D12 clamps the motor permit against the bootloader's PA11 drive
  through R28 (1k). This proves the PROG clamp under real drive (the 74LVC1G11 VIL is 0.8 V).

Set SW5 back to ALL OFF. The pulses return within one frame. Power off.

## 4. First flash of U12 through the Pi

1. With everything unpowered, fit the Pi 4 upside down on the booster header and the H3-H6 standoffs. Remove the bench
   supply.
2. Power the Pi from its own supply through the inline meter. The board takes PI_5V from PI1 pins 2/4. TP4 reads 3.3 V.
   TP10 still pulses as in section 3, and GPIO20 (READY) is LOW.
3. Make sure nothing owns GPIO0, GPIO1, GPIO7 or GPIO26: no host program, and no SPI0 or i2c0 overlay. The script
   checks this and exits with code 2 if a line is busy. Until U12 is flashed, MOTOR_EN (GPIO26) is the only input that
   keeps the motor driver asleep (section 3). Run no host or motor software before step 8.
4. **Set SW5 ALL ON (PROG).** TP10 goes steady HIGH.
   - All four poles must be ON. Poles 1-3 connect SWCLK, SWDIO and NRST. Pole 4 disables the watchdog and clamps both
     permits.
   - The script refuses to run without pole 4. It halts the core for 400 ms and fails if NRST pulsed (the watchdog is
     still running) or an internal watchdog reset happened in that time.
5. Run the flash script with `sudo`, giving it the image file; with the pinned OpenOCD build, also set
   `KEEL_OPENOCD_PREFIX` (`firmware/safety-mcu/README.md`). In order, it:
   - checks the board, the OpenOCD build and the image size;
   - checks that the GPIO lines are free, then drives MOTOR_EN LOW and reads it back;
   - probes the part (DEV_ID 0x443, 16 KB flash) and prints the option register (OPTR);
   - refuses a read-protected part;
   - sets the boot, reset and RAM parity option bits (OPTR bits 24-28 and bit 22) if they need it. With
     `--keep-option-bytes` it does not write them; it stops with code 4 instead;
   - clears the empty-flash flag (PROGEMPTY), programs, verifies and restarts the MCU, and checks that it runs from
     main flash;
   - reads OPTR again and checks it: main-flash boot, bidirectional NRST, SRAM parity check off and readout protection
     level 0. Any other value stops the script with code 4;
   - leaves MOTOR_EN LOW.
6. **Record the OPTR value** from the `KEEL: FINAL OPTR OK 0x...` line. The script has already checked that
   `(OPTR & 0x1F4000FF) == 0x1F4000AA`:
   - RDP byte 0xAA;
   - bits 24-28 set (nBOOT_SEL, nBOOT1, nBOOT0 = 1 and NRST_MODE = 3);
   - bit 22 set (RAM_PARITY_CHECK = 1, which means SRAM parity checking is off). The firmware needs this to keep the
     sensor strike record across resets (section 5.9).

   Do not change option bytes without a review.
7. **Exit codes:** 1 environment or usage, 2 GPIO busy, 3 no SWD link or wrong part, 4 option bytes, 5 program or
   verify, 6 boot check. After any code of 3 or more, set SW5 ALL OFF and power-cycle before you retry. The MCU may stay
   halted until a power-on.
8. **Set SW5 ALL OFF (RUN)**, shut the Pi down and remove its power. Only a full power cycle clears the debug state and
   reloads the option bytes.
9. Power up again and go to 5.0.

A successful boot check only shows that the MCU left the bootloader. Health shows on TP44 (5.0). If the image failed
its own start-up checks, it would never kick the watchdog. U13 would then reset it every ~125-181 ms, and TP10 would
pulse exactly as in section 3.

**Recovery:** if the Pi route fails, use an external SWD probe on TP39 (VTref), TP40 (GND), TP41 (NRST), TP42 (SWDIO)
and TP43 (SWCLK). Keep SW5 ALL ON, so the watchdog is off and WDI needs no drive. Power the board from the Pi or through
TP1, never through TP39.

## 5. Functional and safety tests

These tests need a host program on the Pi that follows the safety handshake (`software/`). The host must produce the
heartbeat from its real control loop, every 70 ms or faster. A separate timer thread is not acceptable: it would keep
the heartbeat alive after the control loop died.

Pi GPIO map:

| Function | Signal | Pi GPIO | Header pin |
|---|---|---|---|
| Heartbeat | HOST_HEARTBEAT | 18 | 12 |
| Servo request | SERVO_REQUEST | 19 | 35 |
| READY (HIGH = healthy) | HOST_FAULT | 20 | 38 |
| ARMED (TP21) | HOST_ARMED | 21 | 40 |
| Motor enable (TP9) | MOTOR_EN | 26 | 37 |
| Motor PWM | MOTOR_RP / RN / LN / LP | 10 / 9 / 11 / 8 | 19 / 21 / 23 / 24 |
| LEDs D1 / D2 / D3 / D4 | LED_A / B / X / Y | 23 / 22 / 17 / 27 | 16 / 15 / 11 / 13 |
| Buttons SW1 / SW2 / SW3 / SW4 | SW_A / B / X / Y | 5 / 6 / 16 / 24 | 29 / 31 / 36 / 18 |
| Ultrasonic | US_TRIG / US_ECHO | 13 / 25 | 33 / 22 |
| Servo signal | SERVO_SIG | 12 | 32 |
| Expansion interrupt | I2C_INT | 4 | 7 |
| Host I2C | I2C_SDA / I2C_SCL | 2 / 3 | 3 / 5 |
| PROG (SW5 ON only) | SWDIO / SWCLK / NRST | 0 / 1 / 7 | 27 / 28 / 26 |

GPIO14 and GPIO15 (pins 8 and 10) are not connected.

### 5.0 Programmed MCU, no host program running

- **TP44 (WDI):** a 1 ms HIGH pulse, with a falling edge every 50 ms. The firmware kicks the watchdog without a Pi
  heartbeat, because it has checked that every output is off.
- **TP10:** steady HIGH. **TP41:** about 3.3 V. **GPIO20 / TP21:** LOW.
- **First kick after reset.** Trigger on TP41 rising (at power-up, or after the TP10 short in 5.2c). The first TP44
  falling edge should come about 80-85 ms later (an emulation estimate). It **must** come before 102.4 ms. Record it at
  power-up and after a fault-bus reset.

### 5.1 Heartbeat, READY and sensor power

- Start the host loop. GPIO20 goes HIGH once the heartbeat qualifies.
- SENSOR_5V (J1 pin 1) switches on to about 5 V after two valid heartbeat falls and the first WDI kick. It stays on
  while the robot is disarmed.
- Stop the host loop. SENSOR_5V drops within about 80 ms of the last heartbeat fall. So does the motor permit, if the
  robot is armed (5.4).

### 5.2 Motor enable interlock (gates U14 and U27)

**a. Permit HIGH and MOTOR_EN LOW: nSLEEP goes LOW.**
1. Arm at zero PWM. The arming handshake completes 601-840 ms after it starts, and TP21 (ARM) goes HIGH.
2. Scope setup:
   - CH1 on TP9 (MOTOR_EN), as the trigger (falling edge);
   - CH2 on TP21 (the buffered MOTOR_EN_SAFE);
   - optionally CH3 on U1 pin 1 (nSLEEP, top side).
3. Disarm, so the host drops GPIO26.

Expected: TP21 and U1 pin 1 fall within 1 us of TP9. The gate acts before the MCU can: the MCU runs a 1 ms loop and
still holds its permit HIGH. With MOTOR_EN LOW, the motors cannot run, whatever the permit does.

Then arm again. The handshake must complete and GPIO20 must stay HIGH. If U12 pins 15/16 (or 13/14) were bridged, the
MOTOR_EN_SAFE (or SERVO_POWER_EN) read-back would follow the permit instead of the gate. The firmware would latch a
feedback fault and arming would fail.

**b. MOTOR_EN HIGH without the handshake: no enable.**
- Skip the handshake. Raise GPIO26 and apply PWM, as host software without the safety handshake would.
- TP21 stays LOW and the wheels do not move. The MCU latches a PWM fault, so GPIO20 goes LOW and sensor power goes off.
- To recover, hold GPIO26 LOW and the PWM at zero for 100 ms.

**c. Fault bus LOW: enables off.**
- While armed, short TP10 (FLT) to TP26 (GND, the neighbouring pad) for about 0.1 s.
- TP21 falls at once. TP41 pulses LOW, which resets U12. GPIO20 goes LOW.
- After release, TP44 resumes (measure 5.0 here). Sensor power returns after two heartbeat falls and the first kick.
  The motors need a fresh handshake.
- One extra watchdog pulse on TP10 shortly after this reset is an expected case. Record how often it occurs.

### 5.3 Watchdog timeout: MOTOR_FAULT goes LOW

- **a. Hardware path.** This was done in section 3 with the blank MCU. Record the TP10 pulse width (22.5-27.5 ms) and
  period, and confirm that SW5 at PROG stops the pulses.
- **b. Foreground stall with the firmware.**
  - This needs an external SWD probe on TP39-TP43 with **SW5 ALL OFF**, so U13 stays enabled. The Pi route cannot run
    this test, because PROG disables U13 by design.
  - Attach without a reset (hot-plug) and halt the core. TP44 stops. TP10 goes LOW for 22.5-27.5 ms, within
    102.4-153.6 ms of the last TP44 falling edge. TP21 and TP41 go LOW with it.
  - The core stays debug-halted through those resets, so detach and power-cycle afterwards.
  - Without a probe, record this item as not characterised (section 6).
- **c. First kick against the frame.** See 5.0.

### 5.4 Motors and current limit (wheels off the ground)

- **Direction:** plug the motors into J2 (left) and J3 (right). Arm, then drive forward at low duty. Both wheels turn
  forward, and a left turn turns left. If one wheel runs backwards, fix it in software or re-pin the ZH housing.
- **Current limit:** at 100 % duty, hold one wheel for 2 s at most.
  - Scope TP12 (right motor, R35) or TP13 (left motor, R36) against TP25. The shunt voltage chops at **180-230 mV**,
    so the current is regulated at **0.22-0.28 A** per motor (DRV8411 current regulation with the 0.82 Ohm shunts).
  - An inline meter in the motor lead never reads more than that.
  - **TP10 stays HIGH.** U1 nFAULT is not connected (`hardware/README.md`, "Design note: U1 nFAULT is not
    connected"). The DRV8411 limits and retries on its own.
- **Heartbeat loss:** while driving, stop the heartbeat. TP21 and the motors drop within about 80 ms.

### 5.5 LEDs

- **D1-D4:** GPIO23, 22, 17 and 27 HIGH light D1, D2, D3 and D4. They are dim: under 1 mA through the 1k elements in
  RN1/RN2. A dark LED means reversed polarity (section 1).
- **Underlighting:** `i2cdetect -y 1` shows **0x54**, the IS31FL3218 (U3) behind buffer U7, powered from SYS_5V.
  - Fill red, then green, blue and white. All six bottom LEDs (RGB1-RGB6) show each colour, at most 6.34 mA per
    channel (R7 12k).
  - If 0x54 is missing, or I2C reports `OSError 121`, check TP2 (SYS_5V) and U2 first.
  - No GPIO enables the underlighting. U3 SDB is tied to SYS_5V. GPIO7 is the PROG reset line and is disconnected at
    RUN.

### 5.6 Buttons

GPIO5, 6, 16 and 24 (SW1, SW2, SW3, SW4) read HIGH at idle with the Pi pull-ups, and LOW when pressed. A button that
always reads pressed means D8-D11 is reversed.

### 5.7 Sensors and expansion

- **Ultrasonic:**
  - Plug the HC-SR04 into J1 with the transducers facing forward. Seen from the top with the front away from you, J1
    pin 1 (SENSOR_5V) is the right-hand pin and GND is on the left.
  - J1 is not keyed. A reversed module trips U30, whose limit is 108-154 mA.
  - The reading works while disarmed. Use a timeout of at least 25 ms. Treat a timeout, or anything above 400 cm, as
    "sensor unavailable", never as "no obstacle".
- **Expansion I2C:**
  - A device on X1 or X3 shows up in `i2cdetect -y 1` (through U6 and U10). TP18/TP19 idle at 3.0-3.3 V and must pull
    below 0.45 V.
  - TP20 reads 3.45 V +/- 0.2 V, and still does after 50 power cycles (U10 partial-power check).
  - Plug an I2C device in while both motors run. If U2 latches off, do not hot-plug expansion devices while armed.

### 5.8 Servo

- J5 is not fitted. Solder a 1x3 2.54 mm header: pin 1 GND, pin 2 SERVO_5V, pin 3 signal (GPIO12 through U8 and the
  330 Ohm R6).
- After the arming handshake, a servo request does three things: the host zeroes the four motor PWM inputs (GPIO26
  stays HIGH), raises GPIO19, and holds the request for its lease.
- Expected:
  - TP46 goes HIGH and TP45 reaches about 5 V, while TP21 (ARMED) is LOW.
  - The burst lasts at most the 500 ms MCU lease, followed by a 2 s cooldown. A request during the cooldown is not
    granted, and the firmware flags it.
- With an electronic load on J5 pins 2-1 during a burst, U15 latches off above its **444-568 mA** limit (R27 52.3k).
  Its FAULT pulls the fault bus LOW, and a fresh handshake is needed afterwards.
- U15's FAULT also gates its own enable through U27 (`hardware/docs/design_notes.md`, "Known limits and
  next-revision items"). Record any FAULT edge on TP10 at servo switch-on. None is expected.

### 5.9 Sensor-branch short and strike limiter

With a healthy heartbeat, short J1 pin 1 to pin 4 through a load that draws more than 154 mA.
- Expect exactly **three** switch-on/trip cycles. SENSOR_5V then stays off (lock-out), while arming, the motors and the
  servo still work.
- Only a power-on reset clears the lock-out.
- Capture TP10, TP41, TP39 and J1 pin 1. For each trip, record whether NRST fired or the MCU only caught the edge.

### 5.10 Supply and stress

- U2 trips on over-voltage at a PI_5V of **5.28-5.47 V**. It switches back on only after PI_5V falls below about
  4.8-5.0 V. On a normal 5.1 V supply, a trip therefore keeps SYS_5V off until the power is cycled. This fails safe and
  is intended. Use a supply that sits at 5.0-5.1 V, not a 5.25 V power bank.
- **Voltage under load:** measure TP1 and TP2 with no load, then at worst-case load: Pi busy, both motors at their
  limit, all RGB LEDs white. Target PI_5V at most 5.2 V and SYS_5V at least 4.9 V. TP10 must never pulse from U11
  (threshold 4.65 V).
- **Transients:** scope PI_5V at motor stops, servo and LED load steps, and over 10 cable hot-plugs. Compare against the
  5.28 V minimum trip.
- **If trips happen in normal use:** change R9 to 34.0k and R10 to 4.02k (both 0.1 %, for example ERA-3ARB3402V and
  ERA-3ARB4021V). That moves the trip to 5.67-5.88 V.
- **MOTOR_FAULT noise:** scope TP10 with a short ground spring while both motors run PWM at their limit. Any dip below
  about 1 V is a problem.
- **Thermal:** use a thermal camera on U2, D5, U4, U3 and U1 at full load.
- **Board:** measure the finished thickness, and the bow and twist.

## 6. Verification status and coverage

Verified on hardware on 2026-11-07, on an R3 board running the release firmware and libkeel 1.0:
- The motors arm and drive in both directions.
- The safety chain passes 17 of 17 tests: a stop drops ARMED within 9-37 us, heartbeat loss removes motor power
  through the MCU, and the re-arm hold-off and the emergency-stop latch work as specified.
- ARMED falls 51-71 ms after the host process is killed (design bound about 86 ms).
- Motor PWM timing over a 600 s soak under load: 99.9th-percentile lateness 216 us, no overruns.
- The RGB underlights and the button LEDs work (checked visually), and the buttons read correctly.

Not yet characterised: ultrasonic ranging (5.7; no sensor was fitted during the tests), the servo path (5.8; J5 is
not fitted), long-term thermal behaviour, ESD and EMC, and the items in the right-hand column below.

| Area | Covered here | Not yet characterised |
|---|---|---|
| Start-up, slow ramps and reset | 3, 4 (OPTR), 5.0, 5.1 | Slow-ramp and brown-out captures; boot source and fitted MCU revision |
| External watchdog and foreground stall | 3 and 5.3a (hardware), 5.0 (first kick), 5.3b (stall) | 5.3b needs an external probe; too-fast and too-slow WDI cases |
| Clock, ADC and internal watchdog | none | Internal oscillator and SAFE_3V3 monitor edges at 3.10/3.50 V over temperature |
| Interrupts and handshake aliasing | 5.2b | Phase-swept PWM, enable and heartbeat edge injection |
| Shortest hardware fault pulse | 5.2c, 5.8, 5.9 | Short pulses (1-10 us) on the real sources |
| Partial power and wrong logic rail | 3 (TP4 about 0 V) | Pi on / SAFE off and SAFE on / Pi off currents; phantom rails |
| Servo and sensor branch faults | 5.8, 5.9 | Temperature, reverse current and capacitance envelope |
| Servo actuation | 5.8 | Real servo stall and travel; rail decay |
| Load budget and rail energy | 5.4, 5.10 | Stalled and back-driven motors against the eFuse |
| Host integration | 5.1, 5.7 | Ranging calls with the sensor fitted (host timing under load: see above) |
| PWM zeroing before a servo request | 5.8 | Four-channel capture of the motor PWM lines at the Pi and the MCU |
| Assembly, EMC and environment | 1, 5.10 | EMC and ESD test campaign |

Keep the board revision, the BOM and firmware checksums, the instrument settings and the raw traces for every item.

## 7. Before driving on floors

- **Insulate the underside.** All 28 test pads and the PI1/J1 solder joints are bare metal on the bottom. Cover them
  with polyimide tape, and peel it back when you need a pad. Handle the robot by its edges or the top plate. A static
  discharge into a bare pad bypasses the connector ESD protection.
- **Watch two bridges.** TP46 (SERVO_POWER_EN) sits next to TP9 (MOTOR_EN), and TP45 (SERVO_5V) sits next to TP47
  (GND).
- **Leave SW5 at RUN.** It is the only operating state. At PROG, the watchdog is off and the robot cannot arm.

## 8. Re-flashing later

Repeat section 4, steps 3-9:
1. Stop the host program.
2. Set SW5 ALL ON.
3. Run the flash script.
4. Set SW5 ALL OFF.
5. Power-cycle.

Flipping pole 4 back to RUN can, rarely, land the first WDI edge inside U13's 0.8-1.2 ms closed window. The chance is
about 2.4 %, a little more with contact bounce. That gives one harmless watchdog reset, with everything off. It is
expected, and the power cycle makes it moot.
