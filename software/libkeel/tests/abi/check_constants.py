#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Prove that libkeel's constants and functions survive preprocessing unchanged.

Binding generators (ctypes tables, jextract, ClangSharp, pycparser) read the
output of `cc -E -P -DKEEL_API= keel.h`, which drops every macro. This gate:

1. rejects any macro in keel.h or port.h other than the version numbers,
   KEEL_API and the include guards, since a macro constant would vanish;
2. preprocesses both headers against stand-in <stdint.h>/<stddef.h> files and
   evaluates every enumerator from the preprocessed text alone;
3. compiles and runs a program that prints the compiler's value of every
   enumerator, and requires both views to agree;
4. compares the values with constants.golden: a name, once released, keeps its
   value for the whole 1.x series, and a new name must be added deliberately;
5. compares the functions declared in the preprocessed keel.h with
   symbols.txt;
6. parses the preprocessed keel.h with pycparser when it is installed; with
   --require-parser (CI) a missing pycparser is a failure, not a skip.

Usage: check_constants.py [--cc CC] [--write-golden] [--require-parser]
"""

import argparse
import ast
import operator
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
INCLUDE = os.path.join(ROOT, "include")
HEADERS = [os.path.join(INCLUDE, "keel", "keel.h"), os.path.join(INCLUDE, "keel", "port.h")]
GOLDEN = os.path.join(HERE, "constants.golden")
SYMBOLS = os.path.join(HERE, "symbols.txt")
ALLOWED_MACROS = {
    "KEEL_VERSION_MAJOR",
    "KEEL_VERSION_MINOR",
    "KEEL_VERSION_PATCH",
    "KEEL_API",
    "KEEL_KEEL_H",
    "KEEL_PORT_H",
}
# Stand-ins give a generator's view: plain typedefs, no compiler extensions.
FAKE_STDINT = """typedef signed char int8_t; typedef unsigned char uint8_t;
typedef short int16_t; typedef unsigned short uint16_t;
typedef int int32_t; typedef unsigned int uint32_t;
typedef long long int64_t; typedef unsigned long long uint64_t;
"""
# These follow the minor version, so layout.c checks them instead of the golden file.
VERSION_DEPENDENT = {"KEEL_ABI_MINOR", "KEEL_ABI_VERSION"}
FAKE_STDDEF = "typedef unsigned long size_t;\n#define NULL ((void *)0)\n"
TOKEN = re.compile(r"\s*(0[xX][0-9a-fA-F]+|\d+)[uUlL]*|\s*([A-Za-z_]\w*)|\s*(<<|>>|[()|&^~+\-*])")


def fail(message):
    print("check_constants: FAIL: " + message)
    sys.exit(1)


def check_macros():
    for path in HEADERS:
        with open(path, encoding="utf-8") as handle:
            for number, line in enumerate(handle, 1):
                match = re.match(r"\s*#\s*define\s+(\w+)", line)
                if match and match.group(1) not in ALLOWED_MACROS:
                    fail("%s:%d defines macro %s; constants must be enumerators"
                         % (os.path.basename(path), number, match.group(1)))


def preprocess(cc, header, fake_dir):
    command = [cc, "-E", "-P", "-DKEEL_API=", "-nostdinc", "-I", fake_dir, "-I", INCLUDE, header]
    text = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    if "__attribute__" in text or "#" in text:
        fail("preprocessed %s still contains compiler-specific text" % os.path.basename(header))
    return text


BINARY = {ast.BitOr: operator.or_, ast.BitAnd: operator.and_, ast.BitXor: operator.xor,
          ast.LShift: operator.lshift, ast.RShift: operator.rshift, ast.Add: operator.add,
          ast.Sub: operator.sub, ast.Mult: operator.mul}
UNARY = {ast.USub: operator.neg, ast.UAdd: operator.pos, ast.Invert: operator.invert}


def fold(node):
    """Fold an integer-only expression tree; any other node type is rejected."""
    if isinstance(node, ast.Expression):
        return fold(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, int):
        return node.value
    if isinstance(node, ast.BinOp) and type(node.op) in BINARY:
        return BINARY[type(node.op)](fold(node.left), fold(node.right))
    if isinstance(node, ast.UnaryOp) and type(node.op) in UNARY:
        return UNARY[type(node.op)](fold(node.operand))
    raise ValueError("unsupported expression element %s" % type(node).__name__)


def evaluate(expression, known):
    """Evaluate a C integer constant expression made of literals and known enumerators."""
    tokens, position = [], 0
    while position < len(expression):
        match = TOKEN.match(expression, position)
        if not match:
            if expression[position:].strip():
                raise ValueError("cannot parse %r" % expression)
            break
        number, name, symbol = match.groups()
        if number:
            tokens.append(str(int(number, 0)))
        elif name:
            if name not in known:
                raise ValueError("unknown name %s in %r" % (name, expression))
            tokens.append(str(known[name]))
        else:
            tokens.append(symbol)
        position = match.end()
    return fold(ast.parse(" ".join(tokens), mode="eval"))


def parse_enums(text, known):
    """Return [(name, value)] in declaration order for every enum in text."""
    found = []
    for body in re.findall(r"\benum\s+\w+\s*\{(.*?)\}\s*;", text, re.S):
        value = -1
        for item in (part.strip() for part in body.split(",")):
            if not item:
                continue
            name, _, expression = (piece.strip() for piece in item.partition("="))
            value = evaluate(expression, known) if expression else value + 1
            known[name] = value
            found.append((name, value))
    return found


def compiler_values(cc, names, work):
    source = os.path.join(work, "print_constants.c")
    binary = os.path.join(work, "print_constants")
    with open(source, "w", encoding="utf-8") as handle:
        handle.write('#include <stdio.h>\n#include "keel/keel.h"\n#include "keel/port.h"\n')
        handle.write("int main(void) {\n")
        for name in names:
            handle.write('    printf("%%s=%%lld\\n", "%s", (long long)%s);\n' % (name, name))
        handle.write("    return 0;\n}\n")
    subprocess.run([cc, "-std=c11", "-I", INCLUDE, source, "-o", binary], check=True)
    output = subprocess.run([binary], check=True, capture_output=True, text=True).stdout
    return dict((line.split("=")[0], int(line.split("=")[1])) for line in output.split())


def read_list(path):
    with open(path, encoding="utf-8") as handle:
        return [line.strip() for line in handle if line.strip() and not line.startswith("#")]


def check_functions(text):
    # Type names never precede "(" in keel.h, so every match is a declared function.
    declared = re.findall(r"\b(keel_\w+)\s*\(", text)
    expected = read_list(SYMBOLS)
    if declared != expected:
        fail("functions in keel.h differ from symbols.txt:\n  header:  %s\n  symbols: %s"
             % (" ".join(declared), " ".join(expected)))
    return len(declared)


def check_with_pycparser(text, required):
    try:
        import pycparser
    except ImportError:
        if required:
            fail("pycparser is not installed, so the C-parser check cannot run "
                 "(pip install pycparser, or apt-get install python3-pycparser)")
        print("check_constants: pycparser not installed; C-parser check skipped")
        return
    # The stand-in <stdint.h> is already inlined, so the text parses on its own.
    tree = pycparser.CParser().parse(text, "keel.h")
    functions = [node for node in tree.ext
                 if type(node).__name__ == "Decl" and type(node.type).__name__ == "FuncDecl"]
    print("check_constants: pycparser parsed keel.h: %d functions" % len(functions))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--write-golden", action="store_true",
                        help="rewrite constants.golden from the headers (new constants only)")
    parser.add_argument("--require-parser", action="store_true",
                        help="fail instead of skipping when pycparser is not installed")
    args = parser.parse_args()

    check_macros()
    with tempfile.TemporaryDirectory() as work:
        with open(os.path.join(work, "stdint.h"), "w", encoding="utf-8") as handle:
            handle.write(FAKE_STDINT)
        with open(os.path.join(work, "stddef.h"), "w", encoding="utf-8") as handle:
            handle.write(FAKE_STDDEF)
        texts = [preprocess(args.cc, header, work) for header in HEADERS]
        known = {}
        # port.h includes keel.h, so its text repeats keel.h's enums first.
        constants = parse_enums(texts[1], known)
        names = [name for name, _ in constants]
        if len(set(names)) != len(names):
            fail("an enumerator name is declared twice")
        compiled = compiler_values(args.cc, names, work)
        for name, value in constants:
            if compiled.get(name) != value:
                fail("%s: preprocessed text gives %d, compiler gives %s"
                     % (name, value, compiled.get(name)))
        function_count = check_functions(texts[0])
        check_with_pycparser(texts[0], args.require_parser)

    golden = {}
    if os.path.exists(GOLDEN):
        for line in read_list(GOLDEN):
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
            handle.write("# Frozen values of every enumerator in keel.h and port.h (ABI 1.x).\n")
            handle.write("# Released values never change; check_constants.py --write-golden\n")
            handle.write("# appends new names after a deliberate review.\n")
            for name in frozen:
                handle.write("%s=%d\n" % (name, known[name]))
        print("check_constants: wrote %d constants to constants.golden" % len(frozen))
    elif added:
        fail("new constants not in constants.golden (review, then --write-golden): "
             + " ".join(added))
    print("check_constants: OK: %d enumerators agree (preprocessed text, compiler, golden); "
          "%d functions match symbols.txt" % (len(names), function_count))


if __name__ == "__main__":
    main()
