#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Keep the status and fault-cause tables of the documentation equal to keel.h.

The tables are rendered from keel.h itself: the value of every enumerator of
keel_status and keel_fault_cause, and the class or status tag and text of its
Doxygen comment. A document carries each table between marker lines:

  <!-- status-table:begin -->  ...  <!-- status-table:end -->
  <!-- cause-table:begin -->   ...  <!-- cause-table:end -->

Without --write the script fails when a document's table differs from the
rendering; with --write it replaces the tables.

Usage: check_doc_tables.py [--write] [document ...]   (default: docs/safety.md)
"""

import argparse
import os
import re
import sys

# check_header_docs is imported from the source tree; leave no __pycache__ there.
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import check_header_docs  # noqa: E402  (locates the Doxygen block above each declaration)

KEEL_H = os.path.join(ROOT, "include", "keel", "keel.h")
DEFAULT_DOCUMENTS = [os.path.join(ROOT, "docs", "safety.md")]


def doc_text(block):
    """One Markdown line from a Doxygen block: markup becomes code spans."""
    lines = [re.sub(r"^\s*/?\*+/?\s?", "", line) for line in block.split("\n")]
    text = " ".join(line for line in lines if line.strip())
    text = text.replace("*/", "").strip()
    text = re.sub(r"<tt>(.*?)</tt>", r"`\1`", text)
    text = re.sub(r"@[cp] (\w+)", r"`\1`", text)
    text = re.sub(r"\b(\w+)::(\w+)", r"`\1.\2`", text)  # struct member reference
    text = re.sub(r"::(\w+)", r"`\1`", text)  # enumerator or other global reference
    text = re.sub(r"(?<!`)\b(keel_\w+\(\))", r"`\1`", text)
    return re.sub(r"\s+", " ", text).replace("|", "\\|")


def enumerators(header, enum):
    """[(name, value, text)] of one enum, value as written in the header."""
    found, inside = [], False
    for index, line in enumerate(header.lines):
        if line.startswith("enum %s {" % enum):
            inside = True
        elif inside and line.startswith("};"):
            return found
        elif inside and line.strip().startswith("KEEL_"):
            match = re.match(r"\s*(KEEL_\w+)\s*=\s*(-?\d+)", line)
            found.append((match.group(1), int(match.group(2)), doc_text(header.doc_above(index))))
    raise SystemExit("check_doc_tables: enum %s not found in keel.h" % enum)


def status_table(header):
    rows = ["| Status | Value | Class | Meaning |", "|---|---|---|---|"]
    for name, value, text in enumerators(header, "keel_status"):
        tag = re.match(r"\[([A-Z]+)\]\s*(.*)", text)
        rows.append("| `%s` | %d | %s | %s |" % (name, value, tag.group(1), tag.group(2)))
    return rows


def cause_table(header):
    rows = ["| # | Cause | Status | Sticky | Meaning and `detail` |", "|---|---|---|---|---|"]
    for name, value, text in enumerators(header, "keel_fault_cause"):
        tag = re.match(r"\[(E_\w+)(, sticky)?\]\s*(.*)", text)
        if tag:
            status = "`KEEL_%s`" % tag.group(1)
            sticky = "yes" if tag.group(2) else "no"
            meaning = tag.group(3)
        else:  # KEEL_CAUSE_NONE has no status
            status, sticky, meaning = "-", "-", text
        rows.append("| %d | `%s` | %s | %s | %s |"
                    % (value, name[len("KEEL_CAUSE_"):], status, sticky, meaning))
    return rows


def replace_block(text, marker, rows, path):
    """Return (new_text, changed) with the marker block's content set to rows."""
    begin, end = "<!-- %s:begin -->" % marker, "<!-- %s:end -->" % marker
    pattern = re.compile(r"(^%s\n)(.*?)(^%s$)" % (re.escape(begin), re.escape(end)), re.M | re.S)
    match = pattern.search(text)
    if not match:
        raise SystemExit("check_doc_tables: %s has no %s markers" % (path, marker))
    body = "\n".join(rows) + "\n"
    return text[:match.start(2)] + body + text[match.end(2):], match.group(2) != body


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--write", action="store_true", help="rewrite the tables in place")
    parser.add_argument("documents", nargs="*", default=DEFAULT_DOCUMENTS)
    args = parser.parse_args()

    header = check_header_docs.Header(KEEL_H)
    tables = {"status-table": status_table(header), "cause-table": cause_table(header)}
    status = 0
    for path in args.documents:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        stale = []
        for marker, rows in tables.items():
            text, changed = replace_block(text, marker, rows, path)
            if changed:
                stale.append(marker)
        if args.write and stale:
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
            print("check_doc_tables: rewrote %s in %s" % (", ".join(stale), path))
        elif stale:
            print("check_doc_tables: %s differs from keel.h in %s (run with --write)"
                  % (", ".join(stale), path))
            status = 1
        else:
            print("check_doc_tables: %s matches keel.h" % path)
    return status


if __name__ == "__main__":
    sys.exit(main())
