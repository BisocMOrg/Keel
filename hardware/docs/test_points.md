# Keel R3 test pads

Keel has 28 bare test pads, all on the underside (B.Cu). Each is a 1.5 mm round pad with an open mask window, finished
ENIG. There is no drill, no paste and no part on them.

`hardware/docs/test_points.csv` holds the same data in machine-readable form, including the legend positions. It was
generated from `hardware/kicad/Keel.kicad_pcb`. The power-up order and the expected values in context are in
`hardware/docs/bringup.md`.

## How to find a pad

- **Legend.** The bottom silkscreen prints the TP number and a short name beside each pad, for example `10 FLT`
  (TP10, MOTOR_FAULT). There is no "TP" prefix. The legend sits 2.6-3.8 mm from the pad centre.
- **Rows.** The pads form six labelled rows: POWER, MOTOR, SERVO, SWD, STATUS and I2C. Each row has its own GND pad.
  The row name is printed at the west end of the row.
- **Grid.** Every pad sits on the 2.54 mm grid of the Pi header PI1 (pin 1 at 79.786, 102.874). The closest pads are
  2.54 mm apart. One fixture plate with standard 2.54 mm pogo pins reaches all 28.
- **Numbering.** The numbers are not contiguous. TP11, TP14-TP17, TP22, TP23 and TP27-TP38 do not exist.
- **Coordinates.**
  - *Board* coordinates are KiCad's, in mm: X to the right, Y downward, seen from the top. Subtract (50.0, 40.0) to get
    the position from the front-left corner of the outline.
  - *Gerber/CPL* coordinates are (X, -Y). The Gerbers, the placement file and the fabrication drawing use them.
  - Seen from the underside, X is mirrored. With the front of the board pointing away from you, the POWER row is on
    your right.

## Rows

| Row | Y (board) | Pads, west to east (legend) |
|---|---|---|
| POWER | 113.034 | TP2 (2 SYS5V), TP6 (6 GND), TP1 (1 PI5V) |
| MOTOR | 105.414 | TP13 (13 ISB), TP12 (12 ISA), TP25 (25 GND), TP3 (3 VM) |
| SERVO | 100.334 | TP45 (45 S5V), TP47 (47 GND) |
| SWD | 125.734 | TP39 (39 3V3S), TP40 (40 GND), TP41 (41 RST), TP43 (43 CLK), TP42 (42 DIO) |
| STATUS | 143.514 | TP44 (44 WDI), TP4 (4 3V3P), TP26 (26 GND), TP10 (10 FLT), TP21 (21 ARM), TP9 (9 MEN), TP46 (46 SVEN) |
| I2C | 113.034 | TP7 (7 SDA), TP8 (8 SCL), TP24 (24 GND), TP18 (18 ESDA), TP19 (19 ESCL), TP20 (20 OREF), TP5 (5 E3V3) |

- The SWD row runs 3V3S, GND, RST, CLK, DIO. CLK comes before DIO.
- The watchdog input WDI (TP44) is at the west end of the STATUS row, not in the SWD row.
- No supply pad sits next to an enable pad. TP4 (PI_3V3) has a GND pad between it and the fault bus.

## All 28 pads

| TP | Net | Legend | Row | X, Y board (mm) | X, Y Gerber (mm) | Expected value / signal |
|---|---|---|---|---|---|---|
| TP1 | `PI_5V` | 1 PI5V | POWER | 72.166, 113.034 | 72.166, -113.034 | 5.0-5.1 V from the Pi supply or a bench supply. Keep it at or below 5.2 V: the U2 over-voltage trip is 5.28-5.47 V. |
| TP2 | `SYS_5V` | 2 SYS5V | POWER | 67.086, 113.034 | 67.086, -113.034 | Within about 60 mV of TP1 when U2 is on. 0 V after a U2 trip. Unpowered: about 1 kOhm to GND (R12 bleed). |
| TP3 | `VM_MOTOR` | 3 VM | MOTOR | 110.266, 105.414 | 110.266, -105.414 | TP2 minus the D5 Schottky drop: close to TP2 at rest, 0.3-0.4 V below it with the motors running. |
| TP4 | `PI_3V3` | 4 3V3P | STATUS | 110.266, 143.514 | 110.266, -143.514 | 3.3 V with the Pi running. About 0 V without the Pi (no phantom rail). |
| TP5 | `EXP_3V3` | 5 E3V3 | I2C | 145.826, 113.034 | 145.826, -113.034 | 3.3 V (U4, LT1763-3.3) whenever SYS_5V is up. |
| TP6 | `GND` | 6 GND | POWER | 69.626, 113.034 | 69.626, -113.034 | Ground for the row. |
| TP7 | `I2C_SDA` | 7 SDA | I2C | 130.586, 113.034 | 130.586, -113.034 | Pi I2C1 SDA (GPIO2). 3.3 V idle with the Pi running (Pi pull-up). Floats without the Pi. |
| TP8 | `I2C_SCL` | 8 SCL | I2C | 133.126, 113.034 | 133.126, -113.034 | Pi I2C1 SCL (GPIO3). 3.3 V idle with the Pi running (Pi pull-up). Floats without the Pi. |
| TP9 | `MOTOR_EN` | 9 MEN | STATUS | 120.426, 143.514 | 120.426, -143.514 | Pi GPIO26. 0 V (R45 22k pull-down) unless the host drives it. HIGH (3.3 V) while armed or during the arming handshake. |
| TP10 | `MOTOR_FAULT` | 10 FLT | STATUS | 115.346, 143.514 | 115.346, -143.514 | HIGH (about 3.3 V, R19 22k to SAFE_3V3) when healthy. LOW = fault from U11 (SYS_5V < 4.65 V), U13 (WDO, 22.5-27.5 ms), U15 or U30 (FAULT). **U1 (DRV8411) is not a source: its nFAULT is not connected.** With U12 blank and SW5 at RUN: LOW pulses of 22.5-27.5 ms every ~125-181 ms. |
| TP12 | `MOTOR_ISEN_A` | 12 ISA | MOTOR | 105.186, 105.414 | 105.186, -105.414 | Right motor shunt (R35). 0 V at rest. While driving: I x 0.82 Ohm, chopping at 180-230 mV (0.22-0.28 A). |
| TP13 | `MOTOR_ISEN_B` | 13 ISB | MOTOR | 102.646, 105.414 | 102.646, -105.414 | Left motor shunt (R36). 0 V at rest. While driving: I x 0.82 Ohm, chopping at 180-230 mV (0.22-0.28 A). |
| TP18 | `EXP_I2C_SDA` | 18 ESDA | I2C | 138.206, 113.034 | 138.206, -113.034 | Expansion side of U10. Idle about 3.0-3.1 V (R40 1.8k to SAFE_3V3 against R42 22k; higher with accessory pull-ups). Driven LOW: below 0.45 V. |
| TP19 | `EXP_I2C_SCL` | 19 ESCL | I2C | 140.746, 113.034 | 140.746, -113.034 | Expansion side of U10. Idle about 3.0-3.1 V (R41 1.8k to SAFE_3V3 against R43 22k; higher with accessory pull-ups). Driven LOW: below 0.45 V. |
| TP20 | `I2C_OVP_REF` | 20 OREF | I2C | 143.286, 113.034 | 143.286, -113.034 | 3.45 V +/- 0.2 V (U10 TPD2S703-Q1 in mode 1, R21 118k / R22 20k). |
| TP21 | `HOST_ARMED` | 21 ARM | STATUS | 117.886, 143.514 | 117.886, -143.514 | Buffered MOTOR_EN_SAFE, to Pi GPIO21. 0 V when disarmed or without the Pi (RN6 10k pull-down). HIGH (3.3 V) = motors armed. |
| TP24 | `GND` | 24 GND | I2C | 135.666, 113.034 | 135.666, -113.034 | Ground for the row. |
| TP25 | `GND` | 25 GND | MOTOR | 107.726, 105.414 | 107.726, -105.414 | Ground for the row. |
| TP26 | `GND` | 26 GND | STATUS | 112.806, 143.514 | 112.806, -143.514 | Ground for the row. |
| TP39 | `SAFE_3V3` | 39 3V3S | SWD | 89.946, 125.734 | 89.946, -125.734 | 3.234-3.366 V (U16, TPS70933). Unpowered: about 470 Ohm to GND (R25 bleed). Use it as the probe's VTref only. Never feed it. |
| TP40 | `GND` | 40 GND | SWD | 92.486, 125.734 | 92.486, -125.734 | Ground for the row. |
| TP41 | `SAFETY_NRST` | 41 RST | SWD | 95.026, 125.734 | 95.026, -125.734 | About 3.3 V when running (R30 47k pull-up, C29 1 nF). Pulses LOW with every fault-bus LOW (through R31 1k), and when the Pi programming path resets U12. |
| TP42 | `SAFETY_SWDIO` | 42 DIO | SWD | 100.106, 125.734 | 100.106, -125.734 | SWD data, U12 PA13 (pulled up inside U12). |
| TP43 | `SAFETY_SWCLK` | 43 CLK | SWD | 97.566, 125.734 | 97.566, -125.734 | SWD clock, U12 PA14-BOOT0 (pulled down inside U12). |
| TP44 | `MCU_WD_SERVICE` | 44 WDI | STATUS | 107.726, 143.514 | 107.726, -143.514 | U12 PA5 to U13 WDI. Programmed, healthy U12: a 1 ms HIGH pulse with a falling edge every 50 ms. Blank U12: no pulses (RN8 10k holds it LOW). Failed start-up checks: static, no falling edge. |
| TP45 | `SERVO_5V` | 45 S5V | SERVO | 110.266, 100.334 | 110.266, -100.334 | 0 V except during a servo burst (U15 on, 5 V, at most 500 ms). Unpowered: about 1 kOhm to GND (R26). |
| TP46 | `SERVO_POWER_EN` | 46 SVEN | STATUS | 122.966, 143.514 | 122.966, -143.514 | U27 output. 0 V except during a servo burst (RN10 10k pull-down). |
| TP47 | `GND` | 47 GND | SERVO | 112.806, 100.334 | 112.806, -100.334 | Ground for the row. |

## Using the pads

- **Power.** Feed a bench supply at TP1 (+) and TP6 (GND), or at PI1 pins 2/4 (+) and 6 (GND).
  - Never feed TP39 (SAFE_3V3). With 3.3 V only at TP39, SYS_5V stays low, U11 holds the fault bus low, and the fault
    bus holds U12 in reset. SWD cannot connect.
  - Never connect a bench supply while a powered Pi is fitted.
- **SWD recovery with an external probe.**
  - Use TP39 (VTref), TP40 (GND), TP41 (NRST), TP42 (SWDIO) and TP43 (SWCLK).
  - Set SW5 to ALL ON (PROG) first. That disables the U13 watchdog and clamps both permits, so WDI (TP44) needs no
    drive.
  - With SW5 at RUN, U13 resets a halted MCU unless WDI gets a falling edge about every 50 ms.
  - The normal route is the Pi (`hardware/docs/bringup.md`, section 4). These pads are the fallback.
- **Fixture.**
  - Bottom parts inside the probe field need pockets: C21 (under U12, 0.9 mm from the TP43 pad edge), RN9 (next to
    TP42) and R35/R36 (under U1).
  - Keep hold-downs clear of C4, PI1, J1, X1/X3 and SW1-SW4.
- **In service.** All 28 pads, and the PI1 and J1 solder joints, are bare metal on the underside.
  - Cover them with polyimide tape before the robot runs on floors. A static discharge into a bare pad bypasses the
    connector ESD arrays.
  - TP45 (SERVO_5V) sits next to TP47 (GND), and TP46 (SERVO_POWER_EN) sits next to TP9 (MOTOR_EN). A conductive
    bridge there shorts the servo supply or couples two enables.
