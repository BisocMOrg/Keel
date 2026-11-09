# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Sequential task actions through the real-time native simulator."""

import time

import pytest

import keel


def test_native_task_arms_moves_stops_between_actions_and_closes():
    robot_handles = []

    class Sequence(keel.KeelTask):
        def run(self):
            robot_handles.append(self.keel)
            self.keel.drive_for(20, 20, 30)
            snapshot = self.keel._loop.snapshot
            assert snapshot.state == keel.State.ARMED
            assert snapshot.command_left == snapshot.command_right == 0
            assert snapshot.output_left == snapshot.output_right == 0
            time.sleep(0.03)
            self.keel.turn_left_for(30)
            snapshot = self.keel._loop.snapshot
            assert snapshot.command_left == snapshot.command_right == 0
            assert snapshot.output_left == snapshot.output_right == 0

    keel.run(Sequence(), keel.TaskOptions(board_kwargs={"backend": keel.Backend.SIMULATOR}))
    assert robot_handles[0]._device.closed


@pytest.mark.camera
def test_native_task_camera_and_board_use_separate_owners():
    frames = []
    handles = []

    class CameraAndMotion(keel.KeelTask):
        def run(self):
            handles.append(self.keel)
            frames.append(self.keel.take_image())
            self.keel.drive_for(20, 20, 20)
            frames.append(self.keel.take_image())

    options = keel.TaskOptions(
        board_kwargs={"backend": keel.Backend.SIMULATOR}, camera_kwargs={"width": 320, "height": 240}
    )
    keel.run(CameraAndMotion(), options)
    assert len(frames) == 2
    assert all(frame.pixel_format == keel.camera.PixelFormat.BGR888 for frame in frames)
    assert all(len(frame.to_bytes()) == 320 * 240 * 3 for frame in frames)
    assert handles[0]._camera.closed
    assert handles[0]._device.closed
