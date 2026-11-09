# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""stop_on_signals(): turn the stop signals into an emergency stop of every target, then unwind.

The default signals are those libkeel's own tools stop on: SIGINT (Ctrl-C), SIGTERM (systemd, kill), SIGHUP (the
terminal or ssh session went away) and SIGQUIT. Without it, SIGTERM, SIGHUP and SIGQUIT end the process with no
cleanup, and the motors stop only when the safety MCU's heartbeat window closes, about 86 ms after the last
heartbeat. Python runs signal handlers on the main thread between
bytecodes, never inside a foreign call, so a handler may call emergency_stop(), which takes no binding lock. Never
call libkeel from a native signal handler (a ctypes callback installed with the C library's signal or sigaction):
that runs in real signal context, where no libkeel function is safe.
"""

from __future__ import annotations

import logging
import signal
from collections.abc import Callable, Iterator, Sequence
from contextlib import contextmanager
from types import FrameType
from typing import TYPE_CHECKING, Any, NoReturn

from .device import Device
from .errors import HandleError, KeelError
from .loop import ControlLoop

if TYPE_CHECKING:
    from .task import RobotSession, TaskRobot

__all__ = ["stop_on_signals"]

_log = logging.getLogger("keel")

# SIGHUP and SIGQUIT do not exist on Windows, where the package must still import.
_DEFAULT_SIGNALS: tuple[signal.Signals, ...] = tuple(
    getattr(signal, name) for name in ("SIGINT", "SIGTERM", "SIGHUP", "SIGQUIT") if hasattr(signal, name)
)


@contextmanager
def stop_on_signals(
    *targets: Device | ControlLoop | RobotSession | TaskRobot, signals: Sequence[signal.Signals] = _DEFAULT_SIGNALS
) -> Iterator[None]:
    """Install handlers that emergency-stop every target, and restore the previous handlers on exit.

    The default signals are SIGINT, SIGTERM, SIGHUP and SIGQUIT; pass signals to choose others. Enter it on the main
    thread (signal.signal raises ValueError elsewhere). Each handler calls emergency_stop() on every target: a Device,
    a ControlLoop's device, a TaskRobot, or an interactive RobotSession. Each call is inside its own try: a failure is
    logged and the handler goes on to the next
    target, and a HandleError means the target was already closed (its close stopped it). The handler then raises
    KeyboardInterrupt for SIGINT, or SystemExit(128 + signal number) for any other signal, so the program unwinds
    through its with blocks and closes every device. By then every target is already latched FAULTED with cause
    USER_STOP. If a handler cannot be installed (an invalid signal, or SIGKILL), the handlers installed before it
    are restored and the error is raised.

    Example::

        with keel.open() as device, keel.stop_on_signals(device):
            ...
    """
    devices = [target.device if isinstance(target, ControlLoop) else target for target in targets]
    from .task import RobotSession, TaskRobot

    for device in devices:
        if not isinstance(device, (Device, RobotSession, TaskRobot)):
            raise TypeError(
                f"stop_on_signals targets must be Device or ControlLoop, TaskRobot or RobotSession, "
                f"not {type(device).__name__}"
            )

    def handler(signum: int, frame: FrameType | None) -> NoReturn:
        for device in devices:
            try:
                device.emergency_stop()
            except HandleError:
                _log.debug("signal %d: already stopped by close", signum)
            except KeelError as error:
                _log.error("signal %d: the emergency stop could not be confirmed: %s", signum, error)
        if signum == signal.SIGINT:
            raise KeyboardInterrupt
        raise SystemExit(128 + signum)

    # Each previous handler is recorded before its signal is changed, so the finally block restores exactly the
    # handlers that were replaced, also when installing a later one fails.
    previous: list[tuple[int, Callable[[int, FrameType | None], Any] | int | None]] = []
    try:
        for number in signals:
            previous.append((number, signal.getsignal(number)))
            try:
                signal.signal(number, handler)
            except BaseException:
                previous.pop()
                raise
        yield
    finally:
        for number, old in reversed(previous):
            signal.signal(number, old)
