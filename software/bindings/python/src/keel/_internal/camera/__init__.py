# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The private layer of keel.camera: loading libkeel-camera, and the structs and calls generated from camera.h.

Nothing here is API. This package imports none of its modules, and nothing in it loads libkeel.so: the camera library
is a library of its own (camera.h, SPEC section 26).
"""
