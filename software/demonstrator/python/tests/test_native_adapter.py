# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Opt-in native simulator integration for the session-backed demonstrator."""
import importlib.util
import os
from pathlib import Path
import sys
import time
import unittest

PATH = Path(__file__).resolve().parents[1] / 'keel_demo.py'
SPEC = importlib.util.spec_from_file_location('keel_demo_native_test', PATH)
demo = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = demo
SPEC.loader.exec_module(demo)


@unittest.skipUnless(os.environ.get('KEEL_DEMO_NATIVE_TEST') == '1', 'native simulator test is opt-in')
class NativeSessionAdapter(unittest.TestCase):
    """Run only with simulator libraries; never select hardware mode."""

    def test_explicit_arm_original_lease_and_peripherals(self):
        """A live browser heartbeat cannot renew an old drive command."""
        app = demo.Controller('simulator')
        sequence = 0

        def command(kind, **values):
            nonlocal sequence
            sequence += 1
            return app.command(dict(type=kind, clientId='native-test', seq=sequence, **values))

        def until(predicate, timeout=4):
            deadline = time.monotonic() + timeout
            while not predicate():
                if time.monotonic() > deadline:
                    self.fail('native adapter state timed out')
                time.sleep(.01)

        try:
            self.assertIn('SIMULATED', app.state()['robot']['capabilities'])
            command('claim')
            command('arm')
            until(lambda: app.state()['robot']['state'] == 'ARMED')
            command('drive', left=200, right=200)
            until(lambda: app.state()['robot']['left'] == 200)
            for _ in range(5):
                time.sleep(.08)
                command('heartbeat')
            until(lambda: app.state()['robot']['state'] == 'DISARMED')
            self.assertEqual(0, app.state()['robot']['left'])
            self.assertIsNone(app.loop_error)
            command('rgb', index=-1, color=0x123456)
            self.assertEqual([0x123456] * 6, app.state()['robot']['rgb'])
            command('buttonLeds', mask=5)
            until(lambda: app.state()['robot']['buttonLeds'] == 5)
            app.board.session.sim_set(app.board.keel.SimItem.RANGE_MM, 500)
            command('range')
            self.assertEqual(500, app.state()['robot']['distanceMm'])
        finally:
            app.close()
