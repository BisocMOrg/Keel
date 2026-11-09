# Keel design notes

Short reasons for the main design choices in Keel R3. The hardware overview is in `hardware/README.md`. The firmware
side of the safety contract is in `firmware/`.

## Safety goals

- No motor or servo power without three things at once: the Pi asks for it, the safety MCU allows it, and no fault is
  active.
- A crashed Pi, a crashed safety MCU, a reset or a recognised fault removes actuator power.
- After any of these, power comes back only after a fresh arming handshake.

## Hardware gates, not firmware, have the last word

- U14 drives the motor driver's nSLEEP: MOTOR_EN (Pi GPIO26) AND the MCU motor permit AND the fault bus HIGH.
- U27 drives the servo switch enable with the same rule and the MCU servo permit.
- Both are single 74LVC1G11 3-input AND gates with Schmitt inputs. They have no state and no firmware.
- MOTOR_EN and both permits have pull-downs, so a floating or unpowered source means "off".
- The Pi can always turn the motors off by dropping GPIO26, whatever the MCU does. Test 5.2a in
  `hardware/docs/bringup.md` checks this.
- Motor and servo are mutually exclusive by firmware only. Two HIGH permits would give two HIGH enables. There is no
  separate exclusion circuit.

## One fault bus, and the MCU cannot drive it

- MOTOR_FAULT is an open-drain bus with a 22k pull-up to SAFE_3V3.
- Sources: U11 (SYS_5V below 4.65 V), U13 (watchdog timeout), U15 and U30 (switch over-current).
- A LOW bus turns off both gates directly. It also resets U12 through R31 (1k into NRST, with 47k and 1 nF).
- U12 only reads the bus (through U29) and is reset by it. It cannot hold the bus HIGH or LOW, so a stuck MCU
  cannot mask a fault.
- Because a reset clears the MCU's permits, every fault ends in a fresh handshake.
- Rule: a part's fault output never gates that part's own enable. This is why U1's nFAULT is not connected. The
  DRV8411 pulls nFAULT LOW briefly each time it wakes, and on the bus that pulse would drop its own nSLEEP through
  U14, so the driver would never stay awake. U15 does not follow the rule yet (see "Known limits and next-revision
  items").

## Safety MCU and external window watchdog

- U12 is an STM32C011 (Arm Cortex-M0+, TSSOP-20). It is small and fully used: all 15 application GPIOs are
  allocated.
- It runs from its own 3.3 V rail, SAFE_3V3, not from the Pi's 3.3 V.
- It checks the Pi heartbeat (GPIO18), the arming handshake on GPIO26, the four PWM lines, the servo request (GPIO19)
  and the actual gate outputs. Only then does it raise a permit.
- U13 (TPS3436, fixed-timing pinout C) is an external window watchdog:
  - the MCU must give a falling edge on WDI about every 50 ms, from its foreground loop only (no interrupt kicks it);
  - an edge too early (inside the 0.8-1.2 ms closed window) or too late (after the 102.4-153.6 ms frame) counts as a
    failure;
  - on failure, WDO pulls the fault bus LOW for 22.5-27.5 ms, which also resets the MCU.
- The MCU kicks the watchdog only when its own checks pass, and either the Pi heartbeat is fresh or every output is
  verified off. So a disarmed board recovers without a Pi, but power needs a live Pi.
- The firmware also runs the MCU's internal watchdog.

## Read-back isolation (R48, R49)

U12 reads MOTOR_EN_SAFE and SERVO_POWER_EN back through 10k resistors. On the TSSOP-20 these read-back pins sit next to
the MCU's own permit outputs. Without the resistors, a solder bridge there would connect a permit straight to a gated
enable, and the MCU could hold nSLEEP HIGH against the gate. With 10k in series, the gate keeps control. A bridge then
only corrupts the read-back, which the firmware sees as a feedback fault.

## Host contract (summary)

- Heartbeat: falling edges on GPIO18, 20-80 ms apart, produced by the host's real control loop.
- Arming handshake on GPIO26: LOW for at least 100 ms, then HIGH 100 ms, LOW 100 ms, HIGH 100 ms, with zero PWM and the
  servo request LOW throughout. Arming completes 601-840 ms after the start.
- Servo: a fresh request after arming, all four motor PWM inputs LOW, a 500 ms lease and a 2 s cooldown.
- Status: GPIO20 HIGH = healthy (READY). GPIO21 HIGH = motors armed. GPIO21 is LOW during a servo burst.
- The authoritative contract and its limits are in `firmware/`.

## Programming from the Pi (SW5)

- SW5 is a sealed 4-pole DIP switch. Poles 1-3 connect Pi GPIO1, GPIO0 and GPIO7 to SWCLK, SWDIO and NRST through 1k
  (RN11). Pole 4 pulls PROG_N LOW.
- PROG_N disables U13 and, through D12 (BAT54C), clamps both permits LOW. So at PROG the watchdog cannot reset the MCU
  mid-flash, and nothing can power the actuators.
- U12 is fitted blank and programmed on the finished robot. This needs no fixture or probe. It also tests SW5, RN11 and
  the reset path, which factory programming would not.
- At RUN the programming lines are disconnected. Keep SPI0 and the Pi's HAT EEPROM probe off anyway, because at PROG
  GPIO0, GPIO1 and GPIO7 carry SWD and reset.
- Known limit: a blank MCU's bootloader drives the motor permit HIGH (see "Known limits and next-revision items").
  The bring-up order handles it (`hardware/docs/bringup.md`, section 3).

## Power

- **Input:** the board takes 5 V from Pi header pins 2/4. D7 (bidirectional TVS) clamps it.
- **eFuse U2:** switches PI_5V to SYS_5V with under- and over-voltage cut-off. The over-voltage trip is 5.28-5.47 V,
  and it re-enables only below about 4.8-5.0 V. A trip on a normal supply therefore stays off until a power cycle. This
  fails safe and is intended.
- **SAFE_3V3:** a separate LDO (U16, TPS70933) from SYS_5V feeds the safety domain (U11-U14, U27, U29) and the
  expansion side of the I2C repeater U6. Its EN pin floats on the internal pull-up, which TI allows. A 470 Ohm bleed
  (R25) keeps the rail near 0 V when off, even with small leakage from other domains.
- **Supervisor U11:** pulls the fault bus LOW if SYS_5V sags below 4.65 V, so a brown-out stops the actuators.
- **Motor rail:** VM_MOTOR comes from SYS_5V through Schottky D5. D5 stops motor back-EMF from feeding SYS_5V. C4
  (47 uF low-ESR) and D6 (unidirectional TVS) hold the rail.
- **Expansion rail:** U4 (LT1763-3.3) makes EXP_3V3, so accessories do not load the Pi's 3.3 V rail.
- **Copper:** 1 oz on all six layers. The SYS_5V trunk runs on In3, and PI_5V links on In2/In3.

## Motor driver

- U1 is a DRV8411 dual H-bridge with external current sensing. The 0.82 Ohm shunts (R35, R36) set the current limit to
  0.22-0.28 A per motor.
- U1 sleeps (nSLEEP LOW) unless the gate U14 allows it. Sleep is the safe default at power-up.
- DRV8833PWPR is a drop-in alternate. C36/C37 are fitted on pins 11/14, which are not connected on the DRV8411 but are
  VCP/VINT on the DRV8833.
- On each bridge, the inputs and outputs are swapped together (the truth table is symmetric). That lets the outputs run
  straight to J2/J3 on the top layer, with no vias.
- nFAULT (pin 8) is not connected (see "One fault bus" above), so U1's own faults are not reported. The DRV8411 still
  protects itself: it retries after an over-current, shuts down when too hot and locks out on under-voltage.

## Servo and sensor branches

- Both branches use a TPS2553-1: active-high enable, adjustable limit, latch-off on overload, FAULT to the fault bus.
- Servo (U15): limit 444-568 mA (R27 52.3k). Power only in short timed bursts: a 500 ms lease and a 2 s cooldown, and
  never while the motors are armed. Continuous servo or LED-strip power is not supported.
- Sensor (U30): limit 108-154 mA (210k). The MCU switches it on only with a fresh heartbeat and no active fault. It does
  not need arming, so the ultrasonic sensor works while the motors are off.
- Sensor strike limiter: a shorted sensor branch would otherwise be retried forever. The firmware counts trips. After
  three, it keeps SENSOR_5V off until a power-on reset. A fresh handshake does not clear it.
- The servo signal pull-up runs from SERVO_5V, and the sensor pull-up from SENSOR_5V, so a disabled accessory is not
  back-powered through its signal line.

## Interfaces and protection

- All Pi inputs to the safety MCU pass through U29 (SN74LVC541A) on SAFE_3V3. Its inputs tolerate 5.5 V and are
  safe with no supply (Ioff), so the MCU never sees a Pi pin directly.
- Status to the Pi (READY, ARMED) goes through U28, powered from the Pi's 3.3 V, with 1k series resistors.
- The expansion I2C bus is buffered (U6) and protected against over-voltage (U10, TPD2S703-Q1). This keeps a faulty
  accessory away from the Pi's I2C pins. The LED driver has its own buffer (U7) on the 5 V side.
- Every expansion, sensor and servo connector has a TPD4E05U06 clamp array next to it. The four buttons have
  TPD1E05U06 diodes.

## Layout

- Six layers: F (parts and short buses), In1 (solid GND), In2 (north-south signals), In3 (east-west signals and the
  SYS_5V trunk), In4 (solid GND), B (bottom LEDs, LED buses, test pads).
- Two solid GND planes give every signal layer a continuous return path.
- Buses run as bundles at a constant pitch and change layer together through planned via fields.
- 611 vias, all 0.6/0.3 mm, all tented both sides. Tenting is required: with open vias, 32 pairs of safety-net copper
  and supply copper from other parts would sit closer than 1 mm.
- Over 200 GND stitching vias form an edge fence, a ring around the Pi's antenna area and a 10 mm lattice. The area
  under the Pi's wireless antenna has no copper on any layer.
- The safety chain runs in one direction: Pi header, U29, U12, the gate column (U14, U27, U13, U11), with U16 below.
  No fault or enable pad sits within 1 mm of another part's supply pad.
- All 28 test pads are on the bottom, on the Pi header's 2.54 mm grid, in six rows with their own GND pad.
- The Pi 4 sits upside down on a 19.5 mm stack. A 3D fit check found no collisions; the closest part under the Pi is
  6.4 mm away.

Accepted layout deviations:
- The X1/X3 ESD arrays hang on 8.5-10 mm stubs, because their pins face away from the connectors. Next revision:
  rotate U22/U23 by 180 degrees, swap their identical channels, and run straight top-layer straps.
- A SAFE_3V3 via pair sits between C31 and R25, inside the regulator's local area.
- A small cosmetic overlap of PI_5V copper near D7 and PI1 pin 4.
- C21, RN9 and R35/R36 sit on the bottom inside the test-pad field. A test fixture needs pockets for them.

## Silkscreen

Short labels only. "ON = PROG / OFF = RUN" at SW5. "M2.5" only at the six M2.5 holes (H3-H6, H11, H12). Block names at
the main blocks. The test pads carry number and short name. R48, C25 and U13 have their designators on the assembly
drawing only.

## Known limits and next-revision items

- **U15 gates its own enable.** U15 (servo power switch) drives the fault bus, and the bus gates U15's own EN through
  U27. This does not follow the rule in "One fault bus". It has not caused a lock-out: U30, the same part, switched
  on without a fault edge in the hardware tests, and the MCU drops the servo permit on the first fault edge anyway.
  The servo test (`hardware/docs/bringup.md`, section 5.8) checks it. Next revision: apply the rule to U15.
- **U1 faults are not reported.** To report them, route nFAULT to its own MCU input and ignore it during the wake
  time.
- **Blank-MCU window at first power.** MCU_MOTOR_PERMIT_RAW is on U12 pin PA11. On the TSSOP-20 package, the
  STM32C011 factory bootloader uses PA11 as USART1 TX and drives it HIGH. Until U12 is first programmed, the motor
  channel is gated only by MOTOR_EN and the pulsed fault bus. The bring-up procedure covers this: no host software
  runs before the first flash. Next revision: move the permit off PA11, or keep that rule.
- **Silkscreen and assembly drawing.** U17-U26: a short silk bar at the end away from pin 1 looks like a pin-1 mark;
  pin 1 is the end with the silk dot. D1-D4: the silk bracket marks the cathode pad, but the LED's body mark is the
  anode. The DNP cross-outs of JP1-JP5 on the assembly drawing run over the fitted U17-U21. The BOM notes and the
  assembly notes in `hardware/README.md` cover all three. Next revision: fix the artwork.
- **BOM metadata.** The schematic datasheet fields of R9, R10 and R19 point to other parts' spec sheets, and two
  resistor entries spell the manufacturer's name differently. The manufacturer part numbers in the BOM are correct
  and govern.
- **Single-source watchdog.** U13 (TPS3436CABFDDDFRQ1) has no alternate. Its timing code and pinout are fixed.
- **eFuse over-voltage trip.** A U2 trip on a normal supply stays off until a power cycle (see "Power"). If trips
  happen in normal use, change R9 to 34.0k and R10 to 4.02k (both 0.1 %). That moves the trip to 5.67-5.88 V.
