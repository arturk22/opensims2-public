#!/bin/sh
# An unoccupied lot opens in Buy mode with the Live button off (docs/
# game-flow-mac.md §7.5), an occupied one in Live; headless, with the game's
# data (OS2_DATA): N002_Lot23 has no family (entered by the Enter button of the
# neighborhood's dialog, then the Live button is clicked and does nothing),
# N002_Lot10 has one (opens in Live; its Live button works).
# usage: empty_lot.sh <opensims2 executable> <scratch dir>
app=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "empty lot: ok (skipped: no OS2_DATA)"; exit 0; fi
rm -rf "$out"; mkdir -p "$out/user"
run() { # <log> <args...>
    log=$1; shift
    "$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio "$@" -ApplePersistenceIgnoreState YES > "$log" 2>&1
}
fail=0
# From the neighborhood: the lot's dialog, Enter, then the Live button (63,550).
run "$out/vacant.log" --hood N002 --hood-lot-click 23 --click 605,468 --wait 20 --click 63,550 --wait 5 --screenshot "$out/vacant.png"
grep -q "N002_Lot23" "$out/vacant.log" || { echo "FAIL: N002_Lot23 not entered"; fail=1; }
grep -q "buy mode: entered" "$out/vacant.log" || { echo "FAIL: unoccupied lot did not open in Buy mode"; fail=1; }
grep -q "live button disabled" "$out/vacant.log" || { echo "FAIL: Live button worked on an unoccupied lot"; fail=1; }
grep -q "mode button 0 (live)" "$out/vacant.log" && { echo "FAIL: Live mode entered on an unoccupied lot"; fail=1; }
# Occupied: opens in Live.
run "$out/occupied.log" --hood N002 --hood-lot 10 --vm --ui live --click 63,550 --wait 5 --screenshot "$out/occupied.png"
grep -q "buy mode: entered" "$out/occupied.log" && { echo "FAIL: occupied lot opened in Buy mode"; fail=1; }
grep -q "mode button 0 (live)" "$out/occupied.log" || { echo "FAIL: Live button dead on an occupied lot"; fail=1; }
[ $fail = 0 ] && echo "empty lot: ok"
rm -rf "$out/user"
exit $fail
