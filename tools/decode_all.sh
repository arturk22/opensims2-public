#!/bin/sh
# Runs `s2pkg decode-all` on every .package of the game and sums the results per type.
# Usage: tools/decode_all.sh [build-dir] [data-dir]   (errors go to decode_errors.txt)
set -u
BUILD=${1:-build}
DATA=${2:-data/game}
ERRORS=decode_errors.txt
: > "$ERRORS"
find "$DATA" -name '*.package' -print0 |
    xargs -0 -n1 "$BUILD/s2pkg" decode-all 2>>"$ERRORS" |
    awk '{ ok[$1] += $2; bad[$1] += $4 } END { for (t in ok) printf "%-10s %8d ok %6d failed\n", t, ok[t], bad[t] }' |
    sort
echo "errors: $(wc -l < "$ERRORS" | tr -d ' ') (see $ERRORS)"
