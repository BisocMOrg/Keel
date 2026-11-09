# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The outputs libkeel-camera writes itself (files, TCP, UDP), their state and their counters.

Camera.start_output() starts one and returns its id; Camera.output_stats() and Camera.stop_output() take the id.
"""

from ._generated import OutputState, OutputStats

__all__ = ["OutputState", "OutputStats"]
