#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Keep the safety rules that live only in prose stated where readers look.

Some contracts cannot be tested by calling the library, only by keeping them
written down:

1. No libkeel function is async-signal-safe. keel.h must not offer
   keel_emergency_stop() to signal handlers, must carry the rule in its
   "Signal safety" paragraph, and every guide that maps Ctrl-C or SIGTERM to
   a stop (spec.md, safety.md, deploy.md, bindings.md) must state it.
2. The struct versioning rule names its two refinements in every place that
   states it (keel.h, spec.md, bindings.md): the KEEL_STRUCT_SIZE_MAX ceiling,
   and keel_open()'s report, the one output written on
   KEEL_E_INVALID_ARGUMENT.

Usage: check_doc_rules.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
problems = []


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as handle:
        return handle.read()


def flat(text):
    """Comment markers and line breaks removed, so phrases may wrap."""
    text = re.sub(r"^\s*\*\s?", "", text, flags=re.M)
    return re.sub(r"\s+", " ", text)


def doc_block(header, function):
    """The Doxygen block directly above a function's declaration."""
    match = re.search(r"/\*\*((?:(?!\*/).)*)\*/\s*KEEL_API [^;]*\b%s\(" % function, header, re.S)
    if not match:
        sys.exit("check_doc_rules: no documentation block for %s in keel.h" % function)
    return flat(match.group(1))


def expect(condition, message):
    if not condition:
        problems.append(message)


def main():
    header = read("include", "keel", "keel.h")
    stop = doc_block(header, "keel_emergency_stop")
    brief = re.search(r"@brief (.*?\.)", stop).group(1)
    expect("signal handler" not in brief.lower(),
           "keel.h offers keel_emergency_stop() to signal handlers: %s" % brief)
    expect("@par Signal safety" in stop and "async-signal-safe" in stop,
           "keel.h keel_emergency_stop() lacks its Signal safety paragraph")
    for document in ("spec.md", "safety.md", "deploy.md", "bindings.md"):
        text = flat(read("docs", document))
        expect("async-signal-safe" in text,
               "docs/%s maps a signal to a stop but never says libkeel is not async-signal-safe"
               % document)

    sources = {"include/keel/keel.h": flat(header), "docs/spec.md": flat(read("docs", "spec.md")),
               "docs/bindings.md": flat(read("docs", "bindings.md"))}
    for name, text in sources.items():
        expect("KEEL_STRUCT_SIZE_MAX" in text, "%s omits the KEEL_STRUCT_SIZE_MAX ceiling" % name)
        expect(re.search(r"keel_open\(?\)?`?'s report", text) is not None,
               "%s omits the keel_open report exception to the output rule" % name)

    for problem in problems:
        print("FAIL  " + problem)
    if problems:
        return 1
    print("check_doc_rules: OK: signal safety and the struct versioning refinements are stated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
