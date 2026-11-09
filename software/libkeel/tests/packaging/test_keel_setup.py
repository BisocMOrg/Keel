#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Tests of scripts/keel-setup without root and without a Raspberry Pi.

The config.txt edit, the group plan and the backed-up atomic write are tested
as functions; main() runs end to end in --dry-run against a stand-in device
tree and a temporary config.txt.

Usage: test_keel_setup.py
"""

import contextlib
import grp
import importlib.machinery
import importlib.util
import io
import json
import os
import pwd
import stat
import sys
import tempfile
import unittest

# keel-setup is loaded from the source tree; leave no __pycache__ there.
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.normpath(os.path.join(HERE, "..", "..", "scripts", "keel-setup"))


def load_keel_setup():
    loader = importlib.machinery.SourceFileLoader("keel_setup", SCRIPT)
    spec = importlib.util.spec_from_loader("keel_setup", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


setup = load_keel_setup()
BLOCK = "\n".join(setup.managed_block(False))
FAST_BLOCK = "\n".join(setup.managed_block(True))
PI_OS_CONFIG = """# For more options and information see config.txt docs
dtparam=audio=on
dtparam=spi=on   # enabled by the imager
#dtparam=spi=on
camera_auto_detect=1

[cm4]
otg_mode=1

[all]
"""


class PlannedConfig(unittest.TestCase):
    def test_empty_file_gets_the_block(self):
        self.assertEqual(setup.planned_config("", False), BLOCK + "\n")

    def test_block_appended_and_spi_turned_off_elsewhere(self):
        planned = setup.planned_config(PI_OS_CONFIG, False)
        self.assertTrue(planned.endswith("\n\n" + BLOCK + "\n"))
        self.assertIn("\ndtparam=spi=off   # enabled by the imager\n", planned)
        self.assertIn("\n#dtparam=spi=on\n", planned)  # comments stay as they are
        self.assertNotIn("dtparam=i2c_arm_baudrate", planned)

    def test_idempotent(self):
        once = setup.planned_config(PI_OS_CONFIG, False)
        self.assertEqual(setup.planned_config(once, False), once)
        fast = setup.planned_config(PI_OS_CONFIG, True)
        self.assertEqual(setup.planned_config(fast, True), fast)

    def test_block_replaced_in_place(self):
        with_block = setup.planned_config(PI_OS_CONFIG, False) + "\n[pi4]\narm_boost=1\n"
        fast = setup.planned_config(with_block, True)
        self.assertIn(FAST_BLOCK + "\n\n[pi4]\narm_boost=1\n", fast)
        self.assertEqual(fast.count(setup.BLOCK_BEGIN), 1)
        self.assertEqual(setup.planned_config(fast, False), with_block)

    def test_unterminated_block_is_refused(self):
        broken = PI_OS_CONFIG + setup.BLOCK_BEGIN + "\ndtparam=spi=off\n"
        with self.assertRaises(setup.SetupError):
            setup.planned_config(broken, False)


class GroupPlan(unittest.TestCase):
    def test_plan(self):
        groups = {"gpio": ["pi"], "i2c": [], "video": []}
        self.assertEqual(setup.group_plan("pi", groups.get), (True, ["keel", "i2c", "video"]))
        groups["keel"] = ["pi"]
        groups["i2c"] = ["pi"]
        self.assertEqual(setup.group_plan("pi", groups.get), (False, ["video"]))
        groups["video"] = ["pi"]
        self.assertEqual(setup.group_plan("pi", groups.get), (False, []))
        self.assertEqual(setup.group_plan("robot", {"keel": []}.get), (False, ["keel"]))

    def test_ubuntu_joins_keel_and_video(self):
        """Ubuntu has no gpio or i2c group: the nodes belong to keel, and the camera's to video."""
        groups = {"keel": [], "video": []}
        self.assertEqual(setup.group_plan("robot", groups.get), (False, ["keel", "video"]))


class WriteConfig(unittest.TestCase):
    def test_backup_and_atomic_replace(self):
        with tempfile.TemporaryDirectory() as work:
            path = os.path.join(work, "config.txt")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(PI_OS_CONFIG)
            os.chmod(path, 0o640)
            updated = setup.planned_config(PI_OS_CONFIG, False)
            backup = setup.write_config(path, PI_OS_CONFIG, updated)
            with open(path, encoding="utf-8") as handle:
                self.assertEqual(handle.read(), updated)
            with open(backup, encoding="utf-8") as handle:
                self.assertEqual(handle.read(), PI_OS_CONFIG)
            self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o640)
            self.assertEqual(sorted(os.listdir(work)), sorted(["config.txt", os.path.basename(backup)]))


class Main(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.compatible = os.path.join(self.work.name, "compatible")
        with open(self.compatible, "wb") as handle:
            handle.write(b"raspberrypi,4-model-b\0brcm,bcm2711\0")
        self.config = os.path.join(self.work.name, "config.txt")
        with open(self.config, "w", encoding="utf-8") as handle:
            handle.write(PI_OS_CONFIG)
        self.addCleanup(setattr, setup, "DEVICE_TREE_COMPATIBLE", setup.DEVICE_TREE_COMPATIBLE)
        setup.DEVICE_TREE_COMPATIBLE = self.compatible

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = setup.main(list(args))
        return status, out.getvalue(), err.getvalue()

    def test_dry_run_changes_nothing(self):
        user = pwd.getpwuid(os.getuid()).pw_name
        status, out, _ = self.run_main("--config", self.config, "--user", user, "--dry-run")
        self.assertEqual(status, 0)
        self.assertIn("+dtparam=i2c_arm=on", out)
        self.assertIn("would update", out)
        with open(self.config, encoding="utf-8") as handle:
            self.assertEqual(handle.read(), PI_OS_CONFIG)
        status, out, _ = self.run_main("--config", self.config, "--dry-run", "--json", "--i2c-fast")
        summary = json.loads(out)
        self.assertEqual(status, 0)
        self.assertTrue(summary["config_changed"] and summary["reboot_required"] and summary["dry_run"])
        self.assertIsNone(summary["backup"])

    def test_refusals(self):
        status, _, err = self.run_main("--config", self.config, "--user", "no such user", "--dry-run")
        self.assertEqual(status, 1)
        self.assertIn("invalid user name", err)
        with open(self.compatible, "wb") as handle:
            handle.write(b"raspberrypi,5-model-b\0brcm,bcm2712\0")
        status, _, err = self.run_main("--config", self.config, "--dry-run")
        self.assertEqual(status, 1)
        self.assertIn("not a Raspberry Pi 4", err)
        if os.geteuid() != 0:
            status, _, err = self.run_main("--config", self.config)
            self.assertEqual(status, 1)
            self.assertIn("as root", err)

    def test_groups_only(self):
        user = pwd.getpwuid(os.getuid()).pw_name
        status, out, _ = self.run_main("--no-boot-config", "--user", user, "--dry-run", "--json")
        summary = json.loads(out)
        self.assertEqual(status, 0)
        self.assertIsNone(summary["config"])
        self.assertLessEqual(set(summary["groups_added"]), {"keel", "gpio", "i2c", "video"})
        for name in set(summary["groups_added"]) - {"keel"}:
            grp.getgrnam(name)  # distribution groups are joined only where they exist


if __name__ == "__main__":
    unittest.main(verbosity=2)
