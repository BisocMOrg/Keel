# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The private layer of the binding: the loaded library, how it is found and checked, and the generated structs.

Nothing here is API. This package imports none of its modules, so the generated modules of the public packages can
import keel._internal.value_enum without an import cycle.
"""
