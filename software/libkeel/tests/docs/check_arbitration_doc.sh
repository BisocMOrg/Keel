#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Keeps the documents' arbitration table identical to
# tests/integration/arbitration_table.h, the table the arbitration test
# replays. Each document carries the output of arbitration_render.c between
# the lines
#   <!-- arbitration-table:begin -->
#   <!-- arbitration-table:end -->
# This script renders the table and fails if any document's block differs;
# with --write it replaces each block with fresh output instead.
#
# Usage: check_arbitration_doc.sh [--write] [document ...]   (default: docs/spec.md)
# Environment: CC (gcc).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
CC=${CC:-gcc}
write=0
if [ "${1:-}" = "--write" ]; then
    write=1
    shift
fi
[ "$#" -eq 0 ] && set -- "$root/docs/spec.md"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT INT TERM
"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -I"$root/include" \
    -I"$root/tests/integration" "$here/arbitration_render.c" -o "$out/render"
"$out/render" >"$out/table.md"

status=0
for document in "$@"; do
    # Split the document into the text before, inside and after the markers.
    awk -v before="$out/before" -v inside="$out/inside" -v after="$out/after" '
        BEGIN { part = before }
        /^<!-- arbitration-table:begin -->$/ { print > before; part = inside; next }
        /^<!-- arbitration-table:end -->$/ { part = after; print > after; next }
        { print > part }
    ' "$document"
    if [ ! -s "$out/after" ]; then
        echo "check_arbitration_doc: $document has no arbitration-table markers"
        status=1
    elif [ "$write" -eq 1 ]; then
        cat "$out/before" "$out/table.md" "$out/after" >"$document"
        echo "check_arbitration_doc: wrote the table into $document"
    elif cmp -s "$out/inside" "$out/table.md"; then
        echo "check_arbitration_doc: $document matches arbitration_table.h"
    else
        echo "check_arbitration_doc: $document differs from arbitration_table.h (run with --write)"
        diff "$out/inside" "$out/table.md" | head -20 || true
        status=1
    fi
    rm -f "$out/before" "$out/inside" "$out/after"
done
exit "$status"
