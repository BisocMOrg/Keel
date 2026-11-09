#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Tree hygiene for software/libkeel (and, given a root, software/bindings):
#   1. every file carries "SPDX-License-Identifier: MIT" and the
#      "Copyright (c) 2026 BisocM" line; a file named LICENSE is a verbatim
#      licence text, so it needs the copyright line only;
#   2. no file has CRLF line endings (the MCU policy copies are hashed);
#   3. no file name or content uses the product names of other parties.
# Those names are assembled at run time, so this script never contains them.
#
# Usage: check_tree.sh [root]   (default: the libkeel directory)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=${1:-$(cd "$here/../.." && pwd)}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT INT TERM
names="$(printf 'tri%s' 'lobot')|$(printf 'swift%s' 'bot')|$(printf 'pimo%s' 'roni')"
status=0

find "$root" -type d \( -name build -o -name .git \) -prune -o -type f -print | sort >"$out/files"

while IFS= read -r file; do
    if [ "$(basename "$file")" != LICENSE ]; then
        grep -q 'SPDX-License-Identifier: MIT' "$file" || echo "$file" >>"$out/no_spdx"
    fi
    grep -q 'Copyright (c) 2026 BisocM' "$file" || echo "$file" >>"$out/no_copyright"
    if grep -q "$(printf '\r')" "$file"; then
        echo "$file" >>"$out/crlf"
    fi
done <"$out/files"
tr '\n' '\0' <"$out/files" | xargs -0 grep -E -i -l -- "$names" >"$out/named" || true
grep -E -i "$names" "$out/files" >>"$out/named" || true

report() {
    if [ -s "$out/$1" ]; then
        echo "FAIL  $2:"
        sed 's/^/      /' "$out/$1"
        status=1
    else
        echo "PASS  $3"
    fi
}
count=$(wc -l <"$out/files")
report no_spdx "files without the SPDX line" "SPDX line in all $count files"
report no_copyright "files without the copyright line" "copyright line in all $count files"
report crlf "files with CRLF line endings" "no CRLF line endings"
report named "files naming other parties' products" "no other parties' product names"
exit "$status"
