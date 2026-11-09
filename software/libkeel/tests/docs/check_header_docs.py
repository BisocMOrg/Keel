#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Documentation gate for keel.h, port.h and camera.h (Doxygen is not available in CI).

Every enum, enumerator, struct typedef, struct field (including port.h hooks),
macro and function must carry a Doxygen comment: a standalone /** ... */ block
directly above it, or a trailing /**< ... */ on its own lines. Each function's
block must also contain:
- @brief;
- one @param[in], @param[out] or @param[in,out] per parameter, with matching
  names and no extras;
- @return or @retval when it returns a value;
- @since;
- "@par Thread safety" (keel.h and camera.h functions).
Every "@retval KEEL_X" must name a status code, and every non-OK retval must
start with the class tag that keel.h gives that code, e.g. [REJECTED].

Usage: check_header_docs.py [header ...]   (defaults to include/keel/keel.h, port.h and camera.h)
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INCLUDE = os.path.normpath(os.path.join(HERE, "..", "..", "include", "keel"))
DEFAULT_HEADERS = [os.path.join(INCLUDE, name) for name in ("keel.h", "port.h", "camera.h")]
FUNCTION = re.compile(r"^(KEEL_API\s+)?(const\s+)?\w+\s*\*?\s*(keel_\w+)\s*\(")
PARAM_TAG = re.compile(r"@param\[(in|out|in,out)\]\s+(\w+)")
RETVAL_TAG = re.compile(r"@retval\s+(\w+)\s*(\[[A-Z]+\])?")


class Header:
    """One header split into lines, with its Doxygen blocks located."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, encoding="utf-8") as handle:
            self.lines = handle.read().split("\n")
        # end line -> text of a standalone block (a line that starts with "/**").
        self.blocks = {}
        index = 0
        while index < len(self.lines):
            stripped = self.lines[index].strip()
            if stripped.startswith("/**") and not stripped.startswith("/**<"):
                start = index
                while "*/" not in self.lines[index]:
                    index += 1
                self.blocks[index] = "\n".join(self.lines[start:index + 1])
            index += 1
        self.all_docs = "\n".join(self.blocks.values())

    def doc_above(self, index):
        """Text of the standalone block that ends on the nearest non-blank line above."""
        above = index - 1
        while above >= 0 and not self.lines[above].strip():
            above -= 1
        return self.blocks.get(above)

    def documented(self, first, last):
        return self.doc_above(first) is not None or any(
            "/**<" in self.lines[i] for i in range(first, last + 1))


def status_classes(keel_h):
    """Map each status code to the class tag its keel.h documentation states."""
    classes = {}
    inside = False
    for index, line in enumerate(keel_h.lines):
        if line.startswith("enum keel_status {"):
            inside = True
        elif inside and line.startswith("};"):
            break
        elif inside and line.strip().startswith("KEEL_"):
            name = line.strip().split()[0]
            tag = re.search(r"\[([A-Z]+)\]", keel_h.doc_above(index) or "")
            classes[name] = tag.group(1) if tag else None
    return classes


def parameters(declaration):
    inner = declaration[declaration.index("(") + 1:declaration.rindex(")")]
    names = []
    for part in (piece.strip() for piece in inner.split(",")):
        if part and part != "void":
            names.append(re.search(r"(\w+)\s*(\[\d*\])?$", part).group(1))
    return names


def check_function(header, index, declaration, classes, problems):
    name = FUNCTION.match(declaration).group(3)
    doc = header.doc_above(index)
    where = "%s:%d %s" % (header.name, index + 1, name)
    if doc is None:
        problems.append(where + ": no Doxygen block")
        return
    required = ["@brief", "@since"]
    if header.name in ("keel.h", "camera.h"):
        required.append("@par Thread safety")
    for tag in required:
        if tag not in doc:
            problems.append("%s: missing %s" % (where, tag))
    documented = [match.group(2) for match in PARAM_TAG.finditer(doc)]
    expected = parameters(declaration)
    if documented != expected:
        problems.append("%s: @param %s, declaration has %s" % (where, documented, expected))
    returns_value = not re.sub(r"^KEEL_API\s+", "", declaration).startswith("void ")
    if returns_value and "@return" not in doc and "@retval" not in doc:
        problems.append(where + ": missing @return/@retval")
    for match in RETVAL_TAG.finditer(doc):
        code, tag = match.group(1), match.group(2)
        if code not in classes:
            problems.append("%s: @retval %s is not a status code" % (where, code))
        elif code != "KEEL_OK" and tag != "[%s]" % classes[code]:
            problems.append("%s: @retval %s tagged %s, class is [%s]"
                            % (where, code, tag, classes[code]))


def check_header(header, classes, counts, problems):
    index, scope = 0, None
    while index < len(header.lines):
        line = header.lines[index]
        stripped = line.strip()
        if re.match(r"^enum keel_\w+ \{", line):
            scope = "enum"
            counts["enums"] += 1
            if header.doc_above(index) is None:
                problems.append("%s:%d: undocumented %s" % (header.name, index + 1, stripped))
        elif re.match(r"^typedef struct keel_\w+ \{", line):
            scope = "struct"
            counts["structs"] += 1
            if header.doc_above(index) is None:
                problems.append("%s:%d: undocumented %s" % (header.name, index + 1, stripped))
        elif scope and line.startswith("}"):
            scope = None
        elif scope == "enum" and stripped.startswith("KEEL_"):
            counts["enumerators"] += 1
            if not header.documented(index, index):
                problems.append("%s:%d: undocumented enumerator %s"
                                % (header.name, index + 1, stripped.split()[0]))
        elif scope == "struct" and stripped and not stripped.startswith(("/*", "*")):
            first = index
            while not header.lines[index].rstrip().endswith(";") and \
                    not re.search(r";\s*/\*\*<.*\*/$", header.lines[index]):
                index += 1
            counts["fields"] += 1
            if not header.documented(first, index):
                problems.append("%s:%d: undocumented field %s" % (header.name, first + 1, stripped))
        elif scope is None and re.match(r"^typedef struct \w+ \w+;", line):
            counts["structs"] += 1
            if header.doc_above(index) is None:
                problems.append("%s:%d: undocumented %s" % (header.name, index + 1, stripped))
        elif scope is None and FUNCTION.match(line):
            first = index
            while not header.lines[index].rstrip().endswith(";"):
                index += 1
            declaration = " ".join(part.strip() for part in header.lines[first:index + 1])
            counts["functions"] += 1
            check_function(header, first, declaration, classes, problems)
        else:
            macro = re.match(r"^#\s*define\s+(KEEL_\w+)", line)
            # An include guard is a #define directly after the matching #ifndef.
            guard = macro and header.lines[index - 1].strip() == "#ifndef " + macro.group(1)
            if macro and not guard:
                counts["macros"] += 1
                if header.doc_above(index) is None and \
                        "@def " + macro.group(1) not in header.all_docs:
                    problems.append("%s:%d: undocumented macro %s"
                                    % (header.name, index + 1, macro.group(1)))
        index += 1


def main():
    paths = sys.argv[1:] or DEFAULT_HEADERS
    headers = [Header(path) for path in paths]
    keel_h = next((h for h in headers if h.name == "keel.h"), Header(DEFAULT_HEADERS[0]))
    classes = status_classes(keel_h)
    if not classes or None in classes.values():
        print("check_header_docs: FAIL: every keel_status enumerator needs a [CLASS] tag")
        return 1
    counts = dict(functions=0, enums=0, enumerators=0, structs=0, fields=0, macros=0)
    problems = []
    for header in headers:
        check_header(header, classes, counts, problems)
    summary = ", ".join("%d %s" % (value, key) for key, value in counts.items())
    print("check_header_docs: %s checked in %s" % (summary, ", ".join(h.name for h in headers)))
    print("check_header_docs: undocumented or incomplete declarations: %d" % len(problems))
    for problem in problems:
        print("  " + problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
