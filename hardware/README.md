# Keel hardware

Keel is a robot baseboard for a Raspberry Pi 4. The Pi plugs in upside down on top of it. Keel drives two DC motors and
powers an ultrasonic sensor. It has six RGB underlights, four buttons with LEDs, I2C expansion and a servo header
(J5, not fitted).

A small safety microcontroller and hardware logic gates decide when the motors and the servo get power. The Pi asks
for power; it cannot force it on. A fault, a missed heartbeat or a stuck safety MCU removes it.

- Revision: **R3** (2026-11).
- Board: 106 x 150 mm, 6 layers, 1.6 mm.
- Design tool: KiCad 10.
- Copyright (c) 2026 BisocM. Licence: CERN-OHL-P-2.0 (see [Licence](#licence)).

Related folders:
- `hardware/kicad/`: the KiCad project.
- `hardware/fabrication/`, `hardware/assembly/`, `hardware/3d/`: the production outputs.
- `hardware/schematic/`: the schematic as a PDF, and sheet 1 as an image.
- `hardware/renders/`: rendered views of the board.
- `hardware/docs/`: bring-up, test pads and design notes.
- `firmware/`: the safety MCU firmware and the Pi flashing tool.
- `software/`: the host software for the Pi.
- `chassis/`: the 3D-printed robot chassis that uses the board as its base plate.

## Blocks

| Block | Main parts | What it does |
|---|---|---|
| Pi 4 interface | PI1 (2x20 socket, Samtec SSW-120-01-G-D); U29, U28, U8, U9 buffers | The Pi 4 mounts upside down on a booster header in PI1. The board takes its 5 V from Pi header pins 2 and 4. Pi signals reach the safety MCU through buffer U29. U28 returns READY and ARMED to the Pi. U8 and U9 buffer the ultrasonic, servo-signal and expansion-interrupt lines. |
| Power entry | D7 TVS, U2 eFuse (TPS259474L), U11 supervisor | U2 switches PI_5V to SYS_5V, with under- and over-voltage cut-off (over-voltage trip 5.28-5.47 V). U11 pulls the fault bus low if SYS_5V drops below 4.65 V. |
| Rails | U16 (TPS70933), U4 (LT1763-3.3), D5, C4, D6 | U16 makes SAFE_3V3 for the safety domain. U4 makes EXP_3V3 for expansion. The motor rail VM_MOTOR comes from SYS_5V through Schottky D5, with 47 uF (C4) and a TVS (D6). |
| Motor driver | U1 DRV8411 (dual H-bridge), R35/R36 0.82 Ohm shunts, J2/J3 | Drives the left (J2) and right (J3) motors from four Pi PWM lines. Current regulation limits each motor to 0.22-0.28 A. U1 sleeps unless the safety gate holds nSLEEP high. |
| Safety gate | U14, U27 (3-input AND gates), fault bus | Motor power: nSLEEP = Pi MOTOR_EN (GPIO26) AND MCU motor permit AND fault bus high. Servo power uses the same rule with the MCU servo permit. Pull-downs hold MOTOR_EN and both permits low by default. |
| Safety MCU | U12 STM32C011F4P6, U13 TPS3436 window watchdog, SW5 | U12 checks the Pi heartbeat, the arming handshake, the PWM inputs and the servo request, then grants the permits. U13 resets it and pulls the fault bus low if it stops kicking the watchdog. SW5 selects RUN or PROG. |
| Fault bus | MOTOR_FAULT (open drain, 22k pull-up) | Driven low by U11, U13, U15 and U30. A low bus turns off both gates at once and resets U12. U12 cannot drive it. |
| Servo and sensor power | U15, U30 (TPS2553-1 switches), J5, J1 | U15 feeds SERVO_5V (J5, 444-568 mA limit, short timed bursts only). U30 feeds SENSOR_5V (J1, 108-154 mA limit). Both latch off on overload and pull the fault bus low. |
| Lighting | U3 IS31FL3218 (I2C 0x54), RGB1-RGB6, D1-D4 | Six RGB LEDs on the underside, driven by U3 at up to 6.34 mA per channel. Four white button LEDs on Pi GPIOs. |
| Buttons | SW1-SW4 | Four push buttons (A, B, X, Y), active low on Pi GPIOs. |
| Expansion | U6, U7 (TCA9517A), U10 (TPD2S703-Q1), X1, X3, J4, JP1-JP5 | The Pi I2C bus reaches the LED driver through U7 and the expansion connectors through U6 and the over-voltage protector U10. X1/X3 (silk "I2C") are 4-pin JST-SH I2C sockets: pin 1 GND, pin 2 EXP_3V3, pin 3 SDA, pin 4 SCL. J4 (1x5) and JP1-JP5 (2x5 header positions, silk "EXP") are not fitted. |
| ESD | U17-U26 (TPD4E05U06), D8-D11 (TPD1E05U06), D6, D7 | Clamp arrays sit at the expansion connectors (X1, X3, J4, JP1-JP5), the sensor socket J1 and the servo header J5. D8-D11 protect the button lines. D7 and D6 are TVS diodes on the 5 V input and the motor rail. |
| Test pads | TP1-TP47 (28 pads) | Bare pads on the underside, in six labelled rows on a 2.54 mm grid. See `hardware/docs/test_points.md`. |

Programming: SW5 is a 4-pole DIP switch, marked "ON = PROG / OFF = RUN". At PROG, Pi GPIO0, GPIO1 and GPIO7 connect
to the MCU's SWD and reset pins, the watchdog is disabled and both permits are clamped low. The MCU is programmed from
the Pi with the tool in `firmware/`. RUN (all OFF) is the only operating state.

The rationale behind these choices is in `hardware/docs/design_notes.md`.

## Design note: U1 nFAULT is not connected

U1 (DRV8411) pin 8, nFAULT, is intentionally left unconnected. It has a no-connect flag in the schematic, and pad 8
has no track.

Why: the fault bus MOTOR_FAULT is an input of the safety gate U14, and U14 drives U1's own nSLEEP (MOTOR_EN_SAFE).
The DRV8411 pulls nFAULT LOW briefly each time it wakes from sleep. On the fault bus, that pulse would drop U1's own
nSLEEP: the driver would go back to sleep, release nFAULT, be woken again by U14 and repeat. It would never stay
awake, and the motors would never arm. The design rule is that a part's fault output never gates that part's own
enable.

The driver still protects itself: it retries after an over-current, shuts down when too hot and locks out on
under-voltage.

Consequences for fault reporting:
- U1's over-current, over-temperature and under-voltage faults do not reach the fault bus. They do not turn off the
  gates or reset the safety MCU, and the host is not told about them.
- TP10 (MOTOR_FAULT) stays HIGH while U1 regulates its current or retries after an over-current
  (`hardware/docs/bringup.md`, section 5.4).
- Every other fault source still drives the bus: U11 (SYS_5V under-voltage), U13 (watchdog), and U15 and U30
  (switch over-current).
- To report U1 faults, a later revision can route nFAULT to its own MCU input and ignore it during the wake time.

## Opening the project

1. Install KiCad 10.
2. Open `hardware/kicad/Keel.kicad_pro`.

The project brings its own libraries through its local tables:
- symbols: `Keel.kicad_sym`;
- footprints: `Keel.pretty`;
- 3D models: `Keel.3dshapes`, referenced as `${KIPRJMOD}/Keel.3dshapes/...`.

The six fiducials use KiCad's standard `Fiducial` library. All footprints are also stored in the board file, so the
board opens without any external library.

Files:
- `Keel.kicad_sch`: the root schematic. Ten sheets: power, motor, motor_safety, interfaces, user_io, lighting,
  expansion, esd, testpoints and ground_probes.
- `Keel.kicad_pcb`: the board, with zones filled.
- `Keel.kicad_dru`: the custom design rules. They run as part of DRC.

The fabrication notes are on the board's User.Drawings layer.

Checks on the project (KiCad 10.0.6): DRC 0 errors and 0 warnings, 0 unconnected items, schematic parity 0, ERC 0.

## Production files

| Folder | File | Use |
|---|---|---|
| `hardware/fabrication/` | `Keel_R3_Gerbers_Drill.zip` | The bare-board upload and the fabrication reference. Gerbers (6 copper, 2 mask, 2 silk, 2 paste, outline), PTH and NPTH Excellon with drill maps, the job file, `drill_report.txt` (hole counts per tool), the IPC-D-356 netlist `Keel.ipc` for the electrical test, and `Keel_R3_fabrication_drawing.pdf` (fabrication notes, stack-up, outline and NO TAB edges). |
| `hardware/assembly/` | `Keel_R3_BOM.csv` | The assembly BOM: 61 lines, 140 placements. Quantities are per board. Every line has a manufacturer part number; approved alternates are listed where they exist. |
| | `Keel_R3_CPL.csv` | Centroid file: 138 SMD parts, both sides, Gerber coordinates. |
| | `Keel_R3_assembly_drawing.pdf` | Assembly drawing, A3, scale 3:2. Sheet 1: top side. Sheet 2: bottom side, viewed from below. Reference designators, pin 1 and polarity marks, unfitted positions crossed out, assembly notes. |
| `hardware/schematic/` | `Keel_R3_schematic.pdf` | The schematic, 11 sheets. |
| | `Keel_R3_schematic_overview.png` | Schematic sheet 1 (host socket and block diagram) as an image. |
| `hardware/3d/` | `Keel_R3.step` | The assembled board, without DNP parts. |
| `hardware/renders/` | `Keel_R3_top.png`, `Keel_R3_bottom.png`, `Keel_R3_three_quarter.png` | Rendered views (KiCad 3D viewer; the bottom view is seen from below). |

Use the CPL file for placement. PI1 and J1 are through-hole and not in it; they are hand-soldered (see the assembly
notes).

BOM rules: the manufacturer part number governs. Schematic Values name the fitted part (the 100 nF parts read
`100n 100V X7R`); minimum ratings and other requirements are in each part's Selected_rating field. Substitute only a
packaging variant or an approved alternate listed on the line.

## Fabrication spec

| Item | Value |
|---|---|
| Layers | 6. Order: F.Cu, In1 (GND), In2, In3, In4 (GND), B.Cu. In1 and In4 are identical GND planes on purpose. |
| Material | FR-4, Tg150 (Shengyi S1000H/S1000HB or an equivalent standard Tg150). |
| Thickness | 1.6 mm standard build. The stack-up table models 1.524 mm including mask. Do not change it: the through-hole tails assume 1.6 mm. |
| Copper | 1 oz (35 um) finished on **all six layers**, inner layers included. The inner layers carry power. |
| Impedance control | None. |
| Finish | ENIG, standard gold thickness. |
| Solder mask / legend | Black mask, white legend, both sides. |
| Vias | 611 through vias, 0.6 mm pad / 0.3 mm drill. Tented both sides. Not plugged or filled. No via-in-pad. |
| Other holes | PTH 1.00 mm x58, 1.02 mm x4, 1.04 mm x40. NPTH 2.1 mm x4, 2.3 mm x2, 2.7 mm x6. No slots in the drill files. |
| Track / space | 0.15 / 0.15 mm minimum on all layers (the 6 mil class). 0.15 mm is also the pour clearance, so it occurs on every layer. |
| Other minimums | Annular ring 0.15 mm. Hole to hole 0.45 mm (12 via pairs are 0.452-0.481 mm apart). Copper to edge 0.5 mm. Mask web 0.10 mm (0.12 mm at C5). |
| Legend | 0.8 mm minimum text height, 0.15 mm minimum stroke. It may be clipped where it crosses tented via drills at Q1, U13 and J1. |
| Intentional features | Ganged mask windows on fine-pitch parts. 28 bare test pads on the bottom (copper and mask opening, no drill, no paste). Small ENIG logo openings on the bottom. |
| Outline | One closed contour plus 8 cut-outs. Smallest cut-out width 1.8 mm; motor windows have 1 mm corner radii. |
| Panel | Single boards. 5 mm process rails are fine. No V-score. No tabs on the edges marked NO TAB (button flanks, front arc, part of the east edge). No X-outs. |
| Quality | IPC Class 2. 100 % electrical test against the IPC-D-356 netlist `Keel.ipc` in the Gerber package. |
| Fab order number | Leave it off. The legend has no free space for it. |

## Assembly notes

- Both sides. Reflow the bottom first (14 parts), then the top (124 parts).
- Stencil 0.10-0.11 mm, both sides, for the 0.5 mm pitch ESD arrays and the 0201 C5. Apertures exactly as in the paste
  Gerbers. The U1 thermal-pad aperture overprints the mask by about 0.15 mm on purpose.
- PI1 and J1 are through-hole. Hand-solder them after reflow; no wave. Seat them flat. PI1 pins 2 and 4 carry the
  whole board current and need full fill.
- Do not fit: J4, J5 and JP1-JP5. Leave their holes open. U17-U21 sit inside the JP outlines and **are** fitted.
- U12: fit it blank. It is programmed from the Pi after assembly (`hardware/docs/bringup.md`).
- SW5: deliver with all four poles OFF (RUN). Keep its tape seal on.
- D1-D4: the body mark is the **anode** and goes to pad 2, away from the silk bracket. The CPL angle already includes
  the +180 deg.
- U17-U26: pin 1 is the end with the silk dot. The short bar at the other end is not a pin-1 mark.
- RGB1-RGB6 are on the bottom. Orient them by sheet 2 of `Keel_R3_assembly_drawing.pdf`.
- D6 must be the unidirectional SM15T6V8A, not the bidirectional version.
- X-ray U1, U2 and U17-U26 on the first boards. Ask for placement photos of D1-D4, U17-U26 and RGB1-RGB6.

Approved alternates (accept nothing else):

| Ref | Primary | Alternate |
|---|---|---|
| U1 | DRV8411PWPR | DRV8833PWPR only (C36/C37 are already fitted for it). Never DRV8411APWPR or clones. |
| U29 | SN74LVC541APWR | SN74LVC541APWRG4 or SN74LVC541APWRE4 |
| U8 | 74LVC2G07GV,125 | 74LVC2G07GV-Q100H |
| D12 | BAT54C,215 | BAT54CLT1G |
| R35, R36 | RL0805FR-070R82L | ERJ-6BQFR82V |
| U16 | TPS70933DBVT | TPS70933DBVR |
| Murata MLCCs | as in the BOM | packaging suffix only |

U13 (TPS3436CABFDDDFRQ1) has no alternate. Its timing code and pinout are fixed.

## Mechanical

The outline, cut-outs and holes are the chassis interface. A chassis that matches them fits.

Datum: the front-left corner of the outline's bounding box, seen from the top. X runs to the right, Y towards the
rear. KiCad board coordinates are these values plus (50.0, 40.0) mm.

- **Outline:** 106.0 x 150.0 mm bounding box, symmetric about the centre line X = 53.0. The front edge is a wide arc.
  The board is 106 mm wide across the motor windows, 96 mm wide along the Pi and 80 mm wide at the rear, where the four
  buttons sit on the side flanks. The full contour is in `Keel-Edge_Cuts.gm1` and on the fabrication drawing, both in
  `hardware/fabrication/Keel_R3_Gerbers_Drill.zip`.
- **Cut-outs (8):**
  - two motor windows, 10.0 x 10.0 mm with 1 mm corner radii, at X 23.0-33.0 and 73.0-83.0, Y 42.0-52.0. Each clears a
    motor's connector tab and the lower part of its JST-ZH socket;
  - four plate slots, 8.8 x 1.8 mm, at X 31.1-39.9 and 66.1-74.9. The front-plate pair is at Y 19.6-21.5, the
    camera-plate pair at Y 24.8-26.6. No top-side parts sit along them;
  - two vertical slots, 2.75 x 7.94 mm, at X 44.8-47.6 and 58.4-61.2, Y 16.5-24.5.
- **Holes (12, NPTH):**

  | Hole | Drill | X, Y (mm from the datum) | Use |
  |---|---|---|---|
  | H11, H12 | 2.7 mm (M2.5) | 35.50, 8.99 and 70.50, 8.99 | Front accessory mounting |
  | H9, H10 | 2.1 mm | 13.26, 38.00 and 92.74, 38.00 | Chassis mounting, beside the motor windows |
  | H7, H8 | 2.1 mm | 13.26, 55.99 and 92.74, 55.99 | Chassis mounting, beside the motor windows |
  | H5, H6 | 2.7 mm (M2.5) | 28.52, 58.00 and 77.48, 58.00 | Pi 4 standoffs |
  | H3, H4 | 2.7 mm (M2.5) | 28.52, 116.00 and 77.48, 116.00 | Pi 4 standoffs |
  | H1, H2 | 2.3 mm | 45.26, 121.01 and 60.75, 121.01 | Castor mounting. The underside has no parts in a 30 x 30 mm area around them (X 38-68, Y 111-141). |

  Every hole has an 8 mm diameter area with no outer-layer copper, for screw heads, nuts and washers.

- **Pi 4:** mounts upside down. Plug it into PI1 through a 2x20 booster header. The stack must be at least 18.2 mm;
  19.5 mm is nominal. Use four M2.5 standoffs at H3-H6 (a 49.0 x 58.0 mm pattern), at most 6.0 mm across corners.
  A fit check with a Pi 4 3D model at 19.5 mm found no collisions. The tightest gaps are between the Pi's PoE header and
  the H3 standoff (0.68 mm) and between its USB-C socket and the H6 standoff (0.72 mm).
- **Heights:** the tallest top part is C4 (5.8 mm). It sits under the Pi with 13.3 mm to spare. SW1-SW4 are 4.3 mm
  tall, about 1 mm from the board flanks.
- **Underside:** 14 SMD parts (including the six RGB LEDs), 28 bare test pads, and the PI1 and J1 solder tails, about
  1.0 mm proud. Leave clearance and insulate the underside (`hardware/docs/bringup.md`, section 7).

## Camera

The camera does not connect to Keel. It plugs into the Raspberry Pi 4's own camera connector, between the
micro-HDMI ports and the audio jack. Camera Module 2 and Camera Module 3 are supported. The chassis routes a 150 mm,
15-way, 1.0 mm pitch flat cable with its contacts on opposite faces at the two ends (`chassis/assembly.md`).
Connect and disconnect the cable only with the power off. The software is the camera library in `software/libkeel`
(`software/libkeel/docs/camera.md`).

## Verification

Verified on hardware on 2026-11-07, on an R3 board running the release firmware and libkeel 1.0.
- The motors arm and drive in both directions.
- The safety chain passes 17 of 17 tests: a stop drops ARMED within 9-37 us, heartbeat loss removes motor power
  through the safety MCU, and the re-arm hold-off and the emergency-stop latch work as specified.
- ARMED falls 51-71 ms after the host process is killed (design bound about 86 ms).
- Motor PWM timing over a 600 s soak under load: 99.9th-percentile lateness 216 us, no overruns.
- The RGB underlights and the button LEDs work (checked visually), and the buttons read correctly.
- libkeel installs from its Debian packages, and `keel-doctor` reports 13 checks passed, 1 skipped (the firmware's
  throttling flags are unavailable) and 0 failed on the robot.

Not yet characterised:
- ultrasonic ranging (no sensor was fitted during the tests);
- the servo path (J5 is not fitted);
- long-term thermal behaviour, ESD and EMC.

The full test plan and its coverage are in `hardware/docs/bringup.md`, section 6. Known limits and items for the next
revision are in `hardware/docs/design_notes.md`.

## Licence

Copyright (c) 2026 BisocM.

- Hardware: the KiCad project, the project-made libraries and the production files are licensed under the CERN Open
  Hardware Licence Version 2 - Permissive (CERN-OHL-P-2.0). The licence text is in
  [`LICENSE-HARDWARE`](../LICENSE-HARDWARE) at the repository root.
- Third-party files keep their own licences. The KiCad-derived footprints and 3D models keep CC-BY-SA-4.0 with the
  KiCad library exception (five older KiCad models: GPL-3.0-or-later with a design exception; see
  THIRD_PARTY_NOTICES.md), and the Wurth Elektronik LED model keeps CC-BY-SA-4.0 with the same exception. The GPL
  text is in [`kicad/Keel.3dshapes/LICENSE-GPL-3.0.txt`](kicad/Keel.3dshapes/LICENSE-GPL-3.0.txt). Each file and
  its source is listed in [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) at the repository root.
- Firmware and software: MIT ([`LICENSE`](../LICENSE) at the repository root).
