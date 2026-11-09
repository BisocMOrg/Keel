"""Execute startup/peripheral regressions against compiled Cortex-M0+ images.

This is a bounded peripheral model, not a silicon or external-watchdog model.
No programming or hardware access occurs. Requires build/safety_controller.elf
from tools/build_firmware.py (Arm GNU Toolchain 14.3.Rel1), plus pyelftools and Unicorn.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import unittest
import zlib

sys.dont_write_bytecode = True

from elftools.elf.elffile import ELFFile
import test_firmware_boot as model

MCU = Path(__file__).resolve().parents[2]  # firmware/safety-mcu
FIRMWARE_C = MCU / "src" / "firmware.c"
BUILD = MCU / "build"
observations = []


def make_early_start_mutant():
    """Build the image with a deliberately broken CHSELR write before every ADC start."""
    target = BUILD / "startup_mutant"
    target.mkdir(exist_ok=True)
    source = FIRMWARE_C.read_text()
    start = " ADC1->ISR=ADC_ISR_EOC|ADC_ISR_EOS|ADC_ISR_OVR;ADC1->CR|=ADC_CR_ADSTART;"
    assert source.count(start) == 1
    source = source.replace(start, " ADC1->CHSELR=(1u<<10); /* deliberately broken */\n" + start)
    mutant_source = target / "firmware.c"
    mutant_source.write_text(source)
    record = json.loads((BUILD / "firmware_build.json").read_text())
    command = record["command"][:]
    command[command.index(str(FIRMWARE_C))] = str(mutant_source)
    command[-1] = str(target / "safety_controller.elf")
    command = ["-Wl,-Map=" + str(target / "safety_controller.map") if arg.startswith("-Wl,-Map=")
               else arg for arg in command]
    subprocess.run(command, check=True, capture_output=True)
    elf_path = target / "safety_controller.elf"
    objcopy = str(Path(command[0]).with_name("arm-none-eabi-objcopy" + (".exe" if os.name == "nt" else "")))
    binary = target / "safety_controller.bin"
    subprocess.run([objcopy, "-O", "binary", str(elf_path), str(binary)], check=True)
    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {symbol.name: symbol["st_value"] for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
    crc = zlib.crc32(binary.read_bytes()[:symbols["__image_end"] - 0x08000000])
    crc_path = target / "image_crc.bin"
    crc_path.write_bytes(crc.to_bytes(4, "little"))
    subprocess.run([objcopy, "--update-section", ".safety_crc=" + str(crc_path), str(elf_path)], check=True)
    return model.Image(elf_path)


def device(image):
    blank = model.seal(model.NEVER, 0, 0)
    return model.Mcu(image, model.PWRRSTF, blank, blank)


class StartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.release = model.Image(BUILD / "safety_controller.elf")
        cls.early_start = make_early_start_mutant()

    def test_initial_reload_and_runtime_lease(self):
        for lsi_hz in (29500, 32000, 34000):
            with self.subTest(lsi_hz=lsi_hz):
                m = device(self.release)
                m.lsi_hz = lsi_hz
                m.start()
                self.assertEqual(m.var("startup_diagnostic"), 5)
                self.assertEqual((m.iwdg_pr, m.iwdg_rlr), (0, 2047))
                self.assertEqual(len(m.iwdg_reloads), 1)
                self.assertGreaterEqual(m.iwdg_reloads[0][0], m.iwdg_ready_at)
                self.assertEqual(m.iwdg_reloads[0][1], 8192)
                w = model.World()
                for _ in range(200):
                    m.step(w)
                self.assertTrue(m.odr & model.PA4)
                self.assertFalse(m.odr & (model.PA11 | model.PA7))
                self.assertEqual(m.var("firmware_diagnostic") & 0x7FF, 0)
                self.assertEqual(m.var("measured_safe_mv"), 3298)
                self.assertEqual(m.adc_channel_writes, 1)
                self.assertEqual(m.adc_ignored_starts, 0)
                self.assertGreater(len(m.iwdg_reloads), 1)
                self.assertTrue(all(ticks == 8192 for _, ticks in m.iwdg_reloads))
                observations.append({"case": "release", "lsi_hz": lsi_hz,
                                     "sync_model_ms": m.iwdg_ready_at,
                                     "initial_reload_lsi_ticks": m.iwdg_reloads[0][1],
                                     "timeout_ms": 8192 * 1000 / lsi_hz,
                                     "adc_channel_writes": m.adc_channel_writes,
                                     "ignored_adc_starts": m.adc_ignored_starts,
                                     "first_foreground_wdi_fall_ms": w.kicks[0] if w.kicks else None})

    def test_stuck_iwdg_status_cannot_reload_or_enable(self):
        m = device(self.release)
        m.iwdg_stuck = True
        m.start()
        self.assertEqual(m.var("startup_diagnostic"), 0x80000002)
        m.step(model.World())
        self.assertFalse(m.odr & (model.PA4 | model.PA7 | model.PA11 | model.PA12))
        self.assertEqual(m.iwdg_reloads, [])

    def test_missing_ccrdy_fails_bounded_and_closed(self):
        m = device(self.release)
        m.adc_never_ready = True
        m.start()
        self.assertEqual(m.var("startup_diagnostic"), 0x80000005)
        self.assertEqual(m.adc_ignored_starts, 0)
        m.step(model.World())
        self.assertEqual(m.var("firmware_diagnostic") & 0x7FF, 0x281)
        self.assertFalse(m.odr & (model.PA4 | model.PA7 | model.PA11 | model.PA12))
        self.assertEqual(len(m.iwdg_reloads), 1)  # startup only; no feed on failed ADC

    def test_per_sample_channel_write_starts_before_ccrdy(self):
        m = device(self.early_start)
        m.start()
        self.assertEqual(m.var("startup_diagnostic"), 5)
        with self.assertRaisesRegex(AssertionError, "did not return to WFI"):
            m.step(model.World())
        self.assertGreater(m.adc_ignored_starts, 0)
        self.assertFalse(m.odr & (model.PA4 | model.PA7 | model.PA11 | model.PA12))
        observations.append({"case": "per_sample_CHSELR_mutant", "ignored_adc_starts": m.adc_ignored_starts,
                             "note": "No automatic SysTick during this blocked-loop model; bounded production EOC timeout remains."})

    def test_invalid_options_and_crc_expose_exact_stage(self):
        m = device(self.release)
        m.optr &= ~model.NRST_MODE
        m.start()
        self.assertEqual(m.var("startup_diagnostic"), 0x80000003)
        m = device(self.release)
        m.w32(m.img.syms["expected_image_crc"], 0)
        m.start()
        self.assertEqual(m.var("startup_diagnostic"), 0x80000004)


if __name__ == "__main__":
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(StartupTests))
    record = {"status": "PASS" if result.wasSuccessful() else "FAIL", "test_methods": result.testsRun, "skipped": len(result.skipped),
              "hex_sha256": hashlib.sha256((BUILD / "safety_controller.hex").read_bytes()).hexdigest(),
              "model": "Compiled ARM startup, prescaled IWDG sync and conditional initial reload; ADC CCRDY rejects early ADSTART",
              "limits": "Discrete model, not silicon; external watchdog and startup elapsed wall time need physical verification",
              "observations": observations}
    (BUILD / "startup_results.json").write_text(json.dumps(record, indent=2) + "\n")
    raise SystemExit(0 if result.wasSuccessful() else 1)
