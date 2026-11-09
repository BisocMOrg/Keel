# Third-party notices

Keel is copyright (c) 2026 BisocM. Unless a file says otherwise, the software
and firmware are licensed under the MIT License ([`LICENSE`](LICENSE)) and the
hardware design files under the CERN Open Hardware Licence Version 2 -
Permissive, CERN-OHL-P-2.0 ([`LICENSE-HARDWARE`](LICENSE-HARDWARE)).

The files listed below come from third parties, or are adapted from
third-party work. They keep their own licences, and the project licences above
do not replace them. Notices embedded in the files are kept unchanged and take
precedence over the summaries here.

## Hardware

### Library inventory

The KiCad project in `hardware/kicad/` ships its own libraries. This is the
complete list of library files and their licences.

| Files | Count | Origin | Licence |
|---|---|---|---|
| `Keel.3dshapes/`: KiCad library models (listed below) | 32 | KiCad 10.0 3D library | CC-BY-SA-4.0 with the KiCad library exception (27 files); GPL-3.0-or-later with a design exception (5 files, licence text in `Keel.3dshapes/LICENSE-GPL-3.0.txt`) |
| `Keel.3dshapes/D_Wurth_WL-SMCW-0603_150060WS75000.step` | 1 | Wurth Elektronik | CC-BY-SA-4.0 with the same design exception |
| `Keel.3dshapes/APPROX_*.step`, `Keel.3dshapes/OPTIONAL_two_5pin_headers.wrl` | 4 | made for this project | CERN-OHL-P-2.0 |
| `Keel.pretty/`: footprints adapted from KiCad (listed below) | 31 | KiCad 10.0 footprint library | CC-BY-SA-4.0 with the KiCad library exception |
| `Keel.pretty/`: other footprints (listed below) | 11 | made for this project | CERN-OHL-P-2.0 |
| `Keel.kicad_sym` | 1 | made for this project | CERN-OHL-P-2.0 |

### KiCad library 3D models

- Location: `hardware/kicad/Keel.3dshapes/`
- Origin: the KiCad 10.0 3D model library (`kicad-packages3D`). Each file is
  byte-identical to the file distributed with KiCad 10.0. Four were renamed in
  this project, as shown in the table.
- Copyright: KiCad Libraries contributors. The holder and year are stated in
  the notice embedded in each file.
- Licence: 27 of the files carry the KiCad library notice: CC-BY-SA-4.0 with
  the KiCad library exception (quoted below). The five files marked GPL in the
  table carry an older notice instead: GPL-3.0-or-later, with a design
  exception that reads "if you create a design which uses this symbol, and
  embed this symbol or unaltered portions of this symbol into the design, this
  symbol does not by itself cause the resulting design to be covered by the GNU
  General Public License." Each file is distributed here with its own notice
  intact. The full GPL-3.0 text is in
  `hardware/kicad/Keel.3dshapes/LICENSE-GPL-3.0.txt`.
- Changes: none to file content.

| File in this project | KiCad library file | In-file notice |
|---|---|---|
| `CP_Elec_6.3x5.8.step` | `Capacitor_SMD.3dshapes/CP_Elec_6.3x5.8.step` | CC-BY-SA |
| `C_0201_0603Metric.step` | `Capacitor_SMD.3dshapes/C_0201_0603Metric.step` | CC-BY-SA |
| `C_0603_1608Metric.step` | `Capacitor_SMD.3dshapes/C_0603_1608Metric.step` | CC-BY-SA |
| `C_0805_2012Metric.step` | `Capacitor_SMD.3dshapes/C_0805_2012Metric.step` | CC-BY-SA |
| `C_1206_3216Metric.step` | `Capacitor_SMD.3dshapes/C_1206_3216Metric.step` | CC-BY-SA |
| `D_SMB.step` | `Diode_SMD.3dshapes/D_SMB.step` | GPL |
| `D_SMC.step` | `Diode_SMD.3dshapes/D_SMC.step` | GPL |
| `D_SOD-128.step` | `Diode_SMD.3dshapes/D_SOD-128.step` | CC-BY-SA |
| `HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3.4x5mm_Mask2.66x2.46mm.step` | `Package_SO.3dshapes/` (same name) | CC-BY-SA |
| `JST_SH_BM04B-SRSS-TB_1x04-1MP_P1.00mm_Vertical.step` | `Connector_JST.3dshapes/` (same name) | CC-BY-SA |
| `MSOP-10_3x3mm_P0.5mm.step` | `Package_SO.3dshapes/MSOP-10_3x3mm_P0.5mm.step` | CC-BY-SA |
| `Nexperia_SOT457_KiCad_TSOT23_6.step` | `Package_TO_SOT_SMD.3dshapes/TSOT-23-6.step` (renamed) | CC-BY-SA |
| `PinHeader_1x03_P2.54mm_Vertical.step` | `Connector_PinHeader_2.54mm.3dshapes/` (same name) | CC-BY-SA |
| `PinHeader_1x05_P2.54mm_Vertical.step` | `Connector_PinHeader_2.54mm.3dshapes/` (same name) | CC-BY-SA |
| `PinSocket_1x04_P2.54mm_Vertical.step` | `Connector_PinSocket_2.54mm.3dshapes/` (same name) | CC-BY-SA |
| `PinSocket_2x20_P2.54mm_Vertical.step` | `Connector_PinSocket_2.54mm.3dshapes/` (same name) | CC-BY-SA |
| `R_0603_1608Metric.step` | `Resistor_SMD.3dshapes/R_0603_1608Metric.step` | CC-BY-SA |
| `R_0805_2012Metric.step` | `Resistor_SMD.3dshapes/R_0805_2012Metric.step` | CC-BY-SA |
| `R_1206_3216Metric.step` | `Resistor_SMD.3dshapes/R_1206_3216Metric.step` | CC-BY-SA |
| `R_Array_Convex_4x0603.step` | `Resistor_SMD.3dshapes/R_Array_Convex_4x0603.step` | GPL |
| `SOIC-24W_7.5x15.4mm_P1.27mm.step` | `Package_SO.3dshapes/` (same name) | CC-BY-SA |
| `SOIC-8_3.9x4.9mm_P1.27mm.step` | `Package_SO.3dshapes/` (same name) | CC-BY-SA |
| `SOT-23.step` | `Package_TO_SOT_SMD.3dshapes/SOT-23.step` | CC-BY-SA |
| `SOT-23-5.step` | `Package_TO_SOT_SMD.3dshapes/SOT-23-5.step` | CC-BY-SA |
| `SOT-23-6.step` | `Package_TO_SOT_SMD.3dshapes/SOT-23-6.step` | CC-BY-SA |
| `SW_DIP_SPSTx04_Slide_KingTek_DSHP04TS_W7.62mm_P1.27mm.step` | `Button_Switch_SMD.3dshapes/` (same name) | CC-BY-SA |
| `SW_SPST_PTS645Sx43SMTR92.step` | `Button_Switch_SMD.3dshapes/` (same name) | CC-BY-SA |
| `TI_DDF0008A_KiCad_TSOT23_8.step` | `Package_TO_SOT_SMD.3dshapes/TSOT-23-8.step` (renamed) | CC-BY-SA |
| `TI_DQA0010_KiCad_USON10.step` | `Package_SON.3dshapes/USON-10_2.5x1.0mm_P0.5mm.step` (renamed) | GPL |
| `TI_DYA0002A_KiCad_SOD523.step` | `Diode_SMD.3dshapes/D_SOD-523.step` (renamed) | GPL |
| `TSSOP-20_4.4x6.5mm_P0.65mm.step` | `Package_SO.3dshapes/` (same name) | CC-BY-SA |
| `VSSOP-8_3x3mm_P0.65mm.step` | `Package_SO.3dshapes/` (same name) | CC-BY-SA |

### Wurth Elektronik LED 3D model

- File: `hardware/kicad/Keel.3dshapes/D_Wurth_WL-SMCW-0603_150060WS75000.step`
  (the body for D1-D4)
- Origin: Wurth Elektronik eiSos KiCad library package for the WL-SMCW series
  (revision 26b), file
  `3dmodels/LED_SMD_Wurth.3dshapes/D_Wurth_WL-SMCW-0603_150060WS75000.step`.
  The file is byte-identical to that one.
- Copyright: Wurth Elektronik eiSos GmbH & Co. KG.
- Licence: CC-BY-SA-4.0 with the same design exception as the KiCad library,
  as stated in the package's disclaimer. The model has no embedded notice, so
  this entry is its notice.
- Disclaimer (from the package, summarised): the data is provided free of
  charge and without warranty of accuracy, merchantability or fitness for a
  purpose. Wurth Elektronik is not liable for its use. Users must verify it in
  their own engineering environment.
- Changes: none.

### Footprints adapted from the KiCad footprint library

- Location: `hardware/kicad/Keel.pretty/`
- Origin: the KiCad 10.0 footprint library (`kicad-footprints`)
- Copyright: KiCad Libraries contributors
- Licence: CC-BY-SA-4.0 with the KiCad library exception
- Changes: 3D model paths point to `Keel.3dshapes/`, four footprints use a
  different 3D model or model placement, and the two pin-header footprints
  have their 3D model hidden. Silkscreen and fabrication-layer graphics,
  properties and three descriptions were edited. A few footprints
  have adjusted pad, drill, mask or attribute settings for this board.

These 29 footprints are adapted from KiCad library footprints of the same name:
`CP_Elec_6.3x5.8`, `C_0201_0603Metric`, `C_0603_1608Metric`,
`C_0805_2012Metric`, `C_1206_3216Metric`, `D_SMB`, `D_SMC`, `D_SOD-128`,
`JST_SH_BM04B-SRSS-TB_1x04-1MP_P1.00mm_Vertical`,
`JST_ZH_S2B-ZR-SM4A-TF_1x02-1MP_P1.50mm_Horizontal`, `LED_0603_1608Metric`,
`LED_Inolux_IN-P55TATRGB_PLCC6_5.0x5.5mm_P1.8mm`, `MSOP-10_3x3mm_P0.5mm`,
`PinHeader_1x03_P2.54mm_Vertical`, `PinHeader_1x05_P2.54mm_Vertical`,
`PinSocket_1x04_P2.54mm_Vertical`, `PinSocket_2x20_P2.54mm_Vertical`,
`R_0603_1608Metric`, `R_0805_2012Metric`, `R_1206_3216Metric`,
`SOIC-8_3.9x4.9mm_P1.27mm`, `SOT-23`, `SOT-23-5`, `SOT-23-6`,
`SW_DIP_SPSTx04_Slide_KingTek_DSHP04TS_W7.62mm_P1.27mm`,
`SW_SPST_PTS645Sx43SMTR92`, `TSSOP-20_4.4x6.5mm_P0.65mm`,
`TestPoint_Pad_D1.5mm` and `VSSOP-8_3x3mm_P0.65mm`.

The 30th, `YC164_4x0603`, is adapted from the KiCad footprint
`Resistor_SMD:R_Array_Convex_4x0603`.

The 31st, `IS31FL3218_SOP24`, is adapted from the KiCad footprint
`Package_SO:SOP-24_7.5x15.4mm_P1.27mm`. Its pads were resized per the
IS31FL3218 datasheet, and its description was edited.

The fiducials on the board use the standard KiCad footprint
`Fiducial:Fiducial_1mm_Mask2mm`. That footprint is stored inside the board
file and is not distributed as a separate library file.

### Project-made library files

These files were made for this project. They are part of the hardware design
and are licensed under CERN-OHL-P-2.0.

- Footprints in `Keel.pretty/`: `PinHeader_2x05_Row5.08_DNP`,
  `DRV8411A_PWP0016_C01_ExternalVias`,
  `MountingHole_2.1mm_NPTH`, `MountingHole_2.3mm_NPTH`,
  `MountingHole_2.7mm_NPTH`, `MountingHole_2.7mm_NPTH_PiHeaderClip`,
  `Nexperia_SOT457_SC74_1.5x2.9mm_P0.95mm`,
  `TI_DDF0008A_1.6x2.9mm_P0.65mm_CommonMask`,
  `TI_DQA0010A_TPD4E05U06_AB_Compatible_CommonMask`,
  `TI_DYA0002A_TPD1E05U06_SOD523` and `TPS25947_RPW0010A`. Their land
  patterns follow the part manufacturers' published package drawings. Several
  of them use the KiCad 3D models listed above.
- 3D body: `OPTIONAL_two_5pin_headers.wrl` (CERN-OHL-P-2.0).
- `APPROX_Inolux_IN_P55TATRGB_body.step`, `APPROX_JST_ZH_S2B_SM4A_body.step`
  and `APPROX_TPS25947_RPW0010A_body.step`: simple dimensional envelopes, not
  manufacturer CAD. Each file declares CERN-OHL-P-2.0 in its STEP header.
- The symbol library `Keel.kicad_sym`.

### KiCad library exception

This exception applies to the KiCad 3D models with the CC-BY-SA notice, the
KiCad-derived footprints and the Wurth Elektronik model above. It is quoted
from the notices in the KiCad library files:

> To the extent that the creation of electronic designs that use 'Licensed
> Material' can be considered to be 'Adapted Material', then the copyright
> holder waives article 3 of the license with respect to these designs and any
> generated files which use data provided as part of the 'Licensed Material'.
>
> You are free to use the library data in your own projects without the
> obligation to share your project files under this or any other license
> agreement. However, if you wish to redistribute these libraries, or parts
> thereof (including in modified form) as a collection then the exception
> above does not apply.

As a result, the Keel board design and the files generated from it, including
`hardware/3d/Keel_R3.step` and the renders, which contain geometry from these
models, are licensed under CERN-OHL-P-2.0. The library files themselves, as
distributed in this repository, keep their own licences: CC-BY-SA-4.0, or
GPL-3.0-or-later for the five files that carry that notice. If you share
them, or modified versions of them, keep their licence and notices and give
attribution.

CC-BY-SA-4.0 full text: <https://creativecommons.org/licenses/by-sa/4.0/legalcode>

GPL-3.0 full text: [`hardware/kicad/Keel.3dshapes/LICENSE-GPL-3.0.txt`](hardware/kicad/Keel.3dshapes/LICENSE-GPL-3.0.txt)

## Robot chassis

The files in `chassis/` were made for this project and are licensed under
CERN-OHL-P-2.0. The purchased parts in `chassis/Keel_R3_robot_assembly.3mf`
and in the images (Raspberry Pi 4, motors, wheels, caster, HC-SR04, camera,
cables, power bank, fasteners) are simplified models drawn for this project;
no third-party model file is included. The Keel board in them is derived from
`hardware/3d/Keel_R3.step`, so the KiCad library exception above applies to
it in the same way as to the STEP model and the renders.

## Firmware (STM32C011 safety controller)

### Arm CMSIS-Core headers

- Location: `firmware/safety-mcu/vendor/cmsis/`
- Files: `cmsis_compiler.h` (V5.3.0), `cmsis_gcc.h` (V5.4.2),
  `cmsis_version.h` (V5.0.5), `core_cm0plus.h` (V5.1.0), `mpu_armv7.h`
  (V5.1.2)
- Origin: Arm CMSIS 5 (`ARM-software/CMSIS_5`, `CMSIS/Core/Include`)
- Copyright: Arm Limited
- Licence: Apache-2.0. The full text is in `LICENSE.txt` in the same folder.
- Changes: none.

### STMicroelectronics CMSIS device files for STM32C0

- Location: `firmware/safety-mcu/vendor/st/`
- Files: `stm32c011xx.h`, `stm32c0xx.h` (CMSIS device version 1.4.1),
  `system_stm32c0xx.c`, `system_stm32c0xx.h`, `startup_stm32c011xx.s`
- Origin: STMicroelectronics `cmsis-device-c0`
- Copyright: 2022 STMicroelectronics
- Licence: Apache-2.0. The full text is in `LICENSE.txt` in the same folder.
- Changes: `startup_stm32c011xx.s` adds a dummy SRAM read before the first
  stack or data access after reset. This is the workaround from
  STMicroelectronics errata sheet ES0569, section 2.2.5. A comment in the file
  marks the change. The other files are unmodified.

The ST files refer to "the LICENSE file in the root directory of this software
component". For these files that is `firmware/safety-mcu/vendor/st/LICENSE.txt`,
not the repository's MIT `LICENSE`.

### Release image

`firmware/safety-mcu/release/safety_controller.hex` contains object code
compiled from the ST system and startup files above, as well as from the
project's own MIT sources. Keep the two licence files,
`firmware/safety-mcu/vendor/cmsis/LICENSE.txt` and
`firmware/safety-mcu/vendor/st/LICENSE.txt`, with any copy of the image.

### OpenOCD board file

`firmware/safety-mcu/programming/openocd_keel.cfg` was written for this project and is
licensed under MIT, as its SPDX line states. At run time it loads OpenOCD's
own `target/stm32c0x.cfg` from the user's OpenOCD installation. No OpenOCD
script is copied into this repository.

## Tools and dependencies that are not redistributed

These are fetched or installed by the user and are not included in this
repository. Each keeps its own licence.

- OpenOCD, Raspberry Pi fork, pinned commit
  `acff23ffd100479d6481a1155123774b79a990b9`, with its Jim Tcl submodule.
  `firmware/safety-mcu/programming/build_openocd.sh` downloads and builds them. They are
  licensed under GPL-2.0-or-later and BSD-2-Clause.
- Arm GNU Toolchain 14.3.Rel1, used to build the firmware.
- `pyelftools` 0.33 and the Unicorn CPU emulator 2.1.4, used by the firmware
  test harness. They are installed into a local virtual environment.
- KiCad 10, used to open and edit the hardware design.
- libcamera (LGPL-2.1-or-later) and libjpeg (libjpeg-turbo: IJG licence,
  BSD-3-Clause and zlib licences), which `libkeel-camera.so.1` links
  dynamically, and libcamera's Raspberry Pi IPA module, which libcamera loads.
  They come from the distribution's packages.
- FFmpeg's libavformat, libavcodec and libavutil (LGPL-2.1-or-later in their
  default configuration), an optional build feature of the camera library.
  The release packages are built without them.
- numpy (BSD-3-Clause), optional for the Python binding's frame arrays.
- Build and test tools fetched by pip and Maven: pycparser (BSD-3-Clause, for
  `software/bindings/tools/keelgen.py`), the Python test, lint and packaging
  tools listed in `software/bindings/python/pyproject.toml`, and JUnit 6
  (EPL-2.0), JaCoCo (EPL-2.0) and the Maven plugins listed in
  `software/bindings/java/pom.xml`.

## Trademarks

Raspberry Pi is a trademark of Raspberry Pi Ltd. STM32 is a trademark of
STMicroelectronics. Arm and Cortex are trademarks of Arm Limited or its
subsidiaries. Other product and company names, including the part
manufacturers named in the bill of materials, are trademarks of their
respective owners. They are used here only to identify the components.
Their use does not imply endorsement.
