#!/usr/bin/env python3
"""Offline parser and option-policy tests. Never opens GPIO or OpenOCD."""
from pathlib import Path
import hashlib
import unittest
import tkinter
import sys

sys.dont_write_bytecode = True

MCU = Path(__file__).resolve().parents[2]  # firmware/safety-mcu
PROGRAMMING = MCU / "programming"
sys.path.insert(0, str(PROGRAMMING))

from check_hex import inspect_hex


def record(kind, address=0, payload=b""):
    data = bytes((len(payload), address >> 8, address & 255, kind)) + payload
    return ":" + (data + bytes((-sum(data) & 255,))).hex().upper()


EXT = record(4, payload=bytes.fromhex("0800"))
DATA = record(0, payload=bytes.fromhex("0018002001010008"))
EOF = record(1)


class HexTests(unittest.TestCase):
    def test_release_image_pinned(self):
        current = (MCU / "release" / "safety_controller.hex").read_bytes()
        self.assertEqual(hashlib.sha256(current).hexdigest(),
                         "1608d866ce248fc12e9eead1dd6d9f6626d3cf7842c615226223ef04c19ffe5d")
        self.assertEqual(inspect_hex(current.decode("ascii")), (4360, 0x08000000, 0x08001108))

    def test_minimum_valid(self):
        self.assertEqual(inspect_hex("\n".join((EXT, DATA, EOF))), (8, 0x08000000, 0x08000008))

    def test_rejections(self):
        cases = {
            "missing EOF": [EXT, DATA],
            "extra after EOF": [EXT, DATA, EOF, DATA],
            "duplicate EOF": [EXT, DATA, EOF, EOF],
            "truncated header": [":00", EOF],
            "count mismatch": [EXT, ":090000000018002001010008BE", EOF],
            "checksum": [EXT, DATA[:-2] + "00", EOF],
            "odd hex": [EXT, DATA + "0", EOF],
            "nonhex": [EXT, DATA[:-1] + "Z", EOF],
            "embedded whitespace": [EXT, DATA[:5] + " " + DATA[5:], EOF],
            "EOF with payload": [EXT, DATA, record(1, payload=b"x")],
            "EOF address": [EXT, DATA, record(1, address=1)],
            "bad extended length": [record(4, payload=b"x"), DATA, EOF],
            "bad extended address": [record(4, address=1, payload=b"\x08\x00"), DATA, EOF],
            "overlap": [EXT, DATA, DATA, EOF],
            "empty data": [EXT, DATA, record(0), EOF],
            "out of flash": [EXT, DATA, record(0, 0x4000, b"x"), EOF],
            "cross flash end": [EXT, DATA, record(0, 0x3FFF, b"xx"), EOF],
            "OTP": [record(4, payload=bytes.fromhex("1fff")), record(0, 0x7000, b"12345678"), EOF],
            "option bytes": [record(4, payload=bytes.fromhex("1fff")), record(0, 0x7800, b"12345678"), EOF],
            "address wrap": [EXT, DATA, record(0, 0xFFFF, b"xx"), EOF],
            "unknown type": [EXT, DATA, record(6), EOF],
            "bad entry": [EXT, DATA, record(5, payload=bytes.fromhex("20000001")), EOF],
            "non Thumb entry": [EXT, DATA, record(5, payload=bytes.fromhex("08000100")), EOF],
            "duplicate entry": [EXT, DATA, record(5, payload=bytes.fromhex("08000101")),
                                record(5, payload=bytes.fromhex("08000101")), EOF],
            "no data": [EOF],
            "missing vectors": [EXT, record(0, 8, b"12345678"), EOF],
        }
        for name, records in cases.items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                inspect_hex("\n".join(records))


def interpreter(initial):
    tcl = tkinter.Tcl()
    for command in ("adapter", "transport", "gdb_port", "telnet_port", "tcl_port", "reset_config", "source"):
        tcl.eval(f"proc {command} args {{}}")
    tcl.eval("proc find {path} {return $path}")
    tcl.eval((PROGRAMMING / "openocd_keel.cfg").read_text())
    tcl.setvar("sim_optr", initial)
    tcl.eval("""
        set writes {}; set logs {}; set bad_reload 0; set reads 0; set reset_ran 0
        proc echo {msg} {global logs; lappend logs $msg}
        proc sleep args {}
        proc flash args {}
        proc verify_image args {}
        proc mmw args {}
        proc halt args {}
        proc resume args {}
        proc reg args {return {pc 0x08000101}}
        proc reset args {global reset_ran; set reset_ran 1}
        proc keel_halt_under_reset args {}
        proc keel_interlock args {return ""}
        proc keel_release_debug args {}
        proc keel_rd32 {address} {
            global sim_optr reads KEEL_FLASH_OPTR
            if {$address == $KEEL_FLASH_OPTR} {incr reads; return $sim_optr}
            return 0
        }
        proc stm32l4x {command args} {
            global writes sim_optr bad_reload
            if {$command eq "option_write"} {
                lappend writes $args
                set want [lindex $args 2]; set mask [lindex $args 3]
                set sim_optr [expr {($sim_optr & (0xFFFFFFFF ^ $mask)) | ($want & $mask)}]
            }
            if {$command eq "option_load"} {set sim_optr [expr {$sim_optr ^ $bad_reload}]}
        }
    """)
    return tcl


class OptionPolicyTests(unittest.TestCase):
    def test_changes_only_allowed_bits_and_preserves_rdp(self):
        before = 0x806000AA
        tcl = interpreter(before)
        tcl.eval("keel_fix_optr")
        self.assertEqual(int(tcl.eval("llength $writes")), 1)
        self.assertEqual(int(tcl.eval("lindex $writes 0 2"), 0), 0x1F400000)
        self.assertEqual(int(tcl.eval("lindex $writes 0 3"), 0), 0x1F400000)
        after = int(tcl.getvar("sim_optr"))
        self.assertEqual(after & 0x1F4000FF, 0x1F4000AA)
        self.assertEqual((after ^ before) & (0xFFFFFFFF ^ 0x1F400000), 0)
        self.assertGreaterEqual(int(tcl.getvar("reads")), 2)
        self.assertIn("OPTR OK", tcl.eval("join $logs"))

    def test_correct_options_need_no_write(self):
        tcl = interpreter(0x1F4000AA)
        tcl.eval("keel_fix_optr")
        self.assertEqual(int(tcl.eval("llength $writes")), 0)

    def test_protected_device_refused(self):
        tcl = interpreter(0x1F4000BB)
        tcl.eval("keel_fix_optr")
        self.assertEqual(int(tcl.eval("llength $writes")), 0)
        self.assertIn("OPTR FAIL", tcl.eval("join $logs"))

    def test_reload_corruption_is_rejected(self):
        for corrupt in (0x00400000, 0x00000001, 0x00010000):
            with self.subTest(corrupt=corrupt):
                tcl = interpreter(0x000000AA)
                tcl.setvar("bad_reload", corrupt)
                tcl.eval("keel_fix_optr")
                self.assertIn("OPTR FAIL", tcl.eval("join $logs"))
                self.assertNotIn("OPTR OK", tcl.eval("join $logs"))

    def test_program_requires_fresh_final_optr(self):
        for optr, expected in ((0x1F4000AA, "FINAL OPTR OK"),
                               (0x1F0000AA, "FINAL OPTR FAIL"),
                               (0x1F4000BB, "FINAL OPTR FAIL")):
            with self.subTest(optr=optr):
                tcl = interpreter(optr)
                tcl.eval("keel_program {mock.hex}")
                self.assertEqual(int(tcl.getvar("reset_ran")), 1)
                self.assertEqual(int(tcl.getvar("reads")), 1)
                self.assertIn(expected, tcl.eval("join $logs"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
