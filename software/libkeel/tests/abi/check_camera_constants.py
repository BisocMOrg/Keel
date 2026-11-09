#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Prove that libkeel-camera's constants and functions survive preprocessing.

The camera counterpart of check_constants.py (whose helpers it uses) for
camera.h, which the bindings read the same way as keel.h:

1. rejects any macro in camera.h other than its version numbers and its
   include guard;
2. preprocesses camera.h with `cc -E -P -DKEEL_API=` against stand-in system
   headers and evaluates every KEEL_CAMERA_* enumerator from the text alone;
3. compiles and runs a program that prints the compiler's value of each, and
   requires both views to agree;
4. compares the values with camera_constants.golden (frozen for 1.x);
5. compares the keel_camera_* functions declared with camera_symbols.txt;
6. parses the preprocessed text with pycparser when it is installed; with
   --require-parser a missing pycparser is a failure.

Usage: check_camera_constants.py [--cc CC] [--write-golden] [--require-parser]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# check_constants is imported from the source tree; leave no __pycache__ there.
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import check_constants  # noqa: E402  (the shared preprocessing helpers)

HEADER = os.path.join(check_constants.INCLUDE, "keel", "camera.h")
GOLDEN = os.path.join(HERE, "camera_constants.golden")
SYMBOLS = os.path.join(HERE, "camera_symbols.txt")
ALLOWED_MACROS = {"KEEL_CAMERA_VERSION_MAJOR", "KEEL_CAMERA_VERSION_MINOR", "KEEL_CAMERA_VERSION_PATCH",
                  "KEEL_CAMERA_H"}
# These follow the minor version, so camera_layout.c checks them instead of the golden file.
VERSION_DEPENDENT = {"KEEL_CAMERA_ABI_MINOR", "KEEL_CAMERA_ABI_VERSION"}


def fail(message):
    print("check_camera_constants: FAIL: " + message)
    sys.exit(1)


def check_macros():
    with open(HEADER, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            match = re.match(r"\s*#\s*define\s+(\w+)", line)
            if match and match.group(1) not in ALLOWED_MACROS:
                fail("camera.h:%d defines macro %s; constants must be enumerators" % (number, match.group(1)))


def compiler_values(cc, names, work):
    source = os.path.join(work, "print_camera_constants.c")
    binary = os.path.join(work, "print_camera_constants")
    with open(source, "w", encoding="utf-8") as handle:
        handle.write('#include <stdio.h>\n#include "keel/camera.h"\nint main(void) {\n')
        for name in names:
            handle.write('    printf("%%s=%%lld\\n", "%s", (long long)%s);\n' % (name, name))
        handle.write("    return 0;\n}\n")
    subprocess.run([cc, "-std=c11", "-I", check_constants.INCLUDE, source, "-o", binary], check=True)
    output = subprocess.run([binary], check=True, capture_output=True, text=True).stdout
    return dict((line.split("=")[0], int(line.split("=")[1])) for line in output.split())


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--write-golden", action="store_true",
                        help="rewrite camera_constants.golden from the header (new constants only)")
    parser.add_argument("--require-parser", action="store_true",
                        help="fail instead of skipping when pycparser is not installed")
    args = parser.parse_args()

    check_macros()
    with tempfile.TemporaryDirectory() as work:
        with open(os.path.join(work, "stdint.h"), "w", encoding="utf-8") as handle:
            handle.write(check_constants.FAKE_STDINT)
        with open(os.path.join(work, "stddef.h"), "w", encoding="utf-8") as handle:
            handle.write(check_constants.FAKE_STDDEF)
        text = check_constants.preprocess(args.cc, HEADER, work)
        known = {}
        # camera.h includes keel.h: its enumerators are evaluated too, then left out.
        constants = [(name, value) for name, value in check_constants.parse_enums(text, known)
                     if name.startswith("KEEL_CAMERA_")]
        names = [name for name, _ in constants]
        if len(set(names)) != len(names):
            fail("an enumerator name is declared twice")
        compiled = compiler_values(args.cc, names, work)
        for name, value in constants:
            if compiled.get(name) != value:
                fail("%s: preprocessed text gives %d, compiler gives %s" % (name, value, compiled.get(name)))
        declared = re.findall(r"\b(keel_camera_\w+)\s*\(", text)
        expected = check_constants.read_list(SYMBOLS)
        if declared != expected:
            fail("functions in camera.h differ from camera_symbols.txt:\n  header:  %s\n  symbols: %s"
                 % (" ".join(declared), " ".join(expected)))
        check_constants.check_with_pycparser(text, args.require_parser)

    golden = {}
    if os.path.exists(GOLDEN):
        for line in check_constants.read_list(GOLDEN):
            name, value = line.split("=")
            golden[name] = int(value)
    for name, value in golden.items():
        if name not in known:
            fail("released constant %s was removed" % name)
        if known[name] != value:
            fail("released constant %s changed from %d to %d" % (name, value, known[name]))
    frozen = [name for name in names if name not in VERSION_DEPENDENT]
    added = [name for name in frozen if name not in golden]
    if added and args.write_golden:
        with open(GOLDEN, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("# SPDX-License-Identifier: MIT\n# Copyright (c) 2026 BisocM\n#\n")
            handle.write("# Frozen values of every enumerator in camera.h (camera ABI 1.x).\n")
            handle.write("# Released values never change; check_camera_constants.py --write-golden\n")
            handle.write("# appends new names after a deliberate review.\n")
            for name in frozen:
                handle.write("%s=%d\n" % (name, known[name]))
        print("check_camera_constants: wrote %d constants to camera_constants.golden" % len(frozen))
    elif added:
        fail("new constants not in camera_constants.golden (review, then --write-golden): " + " ".join(added))
    print("check_camera_constants: OK: %d enumerators agree (preprocessed text, compiler, golden); "
          "%d functions match camera_symbols.txt" % (len(names), len(declared)))


if __name__ == "__main__":
    main()
