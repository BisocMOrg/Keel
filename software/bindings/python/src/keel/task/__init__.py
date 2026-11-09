# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Sequential student tasks over bounded robot actions.

Subclass KeelTask, implement run(), and call keel.run(task). The task's self.keel
handles automatic arming and finite motor actions while a separate ControlLoop
owns the board. Ordinary task code and camera calls stay on the calling thread.
Application behaviours such as approaching a coloured box belong in the task.
"""

from .keel_task import KeelTask, run
from .task_options import TaskOptions
from .task_robot import TaskError, TaskRobot

__all__ = ["KeelTask", "TaskError", "TaskOptions", "TaskRobot", "run"]
