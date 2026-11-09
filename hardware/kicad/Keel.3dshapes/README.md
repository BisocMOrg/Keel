# Keel 3D models

3D bodies for the Keel board and its project footprint library (`Keel.pretty`). Every model path is project-relative (`${KIPRJMOD}/Keel.3dshapes/...`). The models are for the 3D viewer and STEP export only: the footprints and the BOM define the assembly, and the models do not prove mechanical fit.

- `APPROX_*.step`: simple dimensional envelopes made by BisocM for the JST ZH motor connector, the Inolux RGB LED and the TPS25947 eFuse. They are not manufacturer CAD. Licence: CERN-OHL-P-2.0 (part of the hardware design), as stated in each file header.
- `OPTIONAL_two_5pin_headers.wrl`: a representative body made by BisocM that shows the unfitted 2x05 header positions. It is part of the hardware design (CERN-OHL-P-2.0).
- All other model files are KiCad library models and one Wurth Elektronik LED model. Their origin and licence (CC-BY-SA-4.0 with the KiCad library exception; five older KiCad models: GPL-3.0-or-later with a design exception) are listed in `THIRD_PARTY_NOTICES.md` at the repository root.
- `LICENSE-GPL-3.0.txt`: the full GNU General Public License version 3 text, for the five GPL-3.0-or-later KiCad models (`D_SMB.step`, `D_SMC.step`, `R_Array_Convex_4x0603.step`, `TI_DQA0010_KiCad_USON10.step`, `TI_DYA0002A_KiCad_SOD523.step`).
