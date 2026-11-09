# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""ValueEnum: the base class of the open enumerations in the generated modules.

It imports nothing from keel, so every generated module can import it without an import cycle.
"""

from __future__ import annotations

import enum
from typing import Any, Final

SENTINEL: Final = "UNRECOGNIZED"
"""The name prefix of a pseudo-member; keelgen checks that no enumerator of keel.h starts with it."""


class ValueEnum(enum.IntEnum):
    """An open enumeration: an unknown value becomes an uncached pseudo-member instead of an error."""

    @classmethod
    def _missing_(cls, value: object) -> Any:
        if not isinstance(value, int) or isinstance(value, bool):
            return None
        member = int.__new__(cls, value)
        member._name_ = f"{SENTINEL}_{value}"
        member._value_ = value
        return member
