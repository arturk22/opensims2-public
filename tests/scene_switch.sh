#!/bin/sh
# Two lots in one session with the game's data (OS2_DATA), headless: N002_Lot10
# (Silva), back to the neighborhood (Options, Neighborhood, Yes), then
# N002_Lot3 (Bicudo). The renderer is replaced with each scene; the fence
# pieces kept the first lot's model and instance ids and took over 336 of the
# second lot's object instances (the grandfather clock drawn as a brick
# chimney piece where a railing stands, the Sims and the railings gone).
# usage: scene_switch.sh <opensims2 executable> <scratch dir>
app=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "scene switch: ok (skipped: no OS2_DATA)"; exit 0; fi
rm -rf "$out"; mkdir -p "$out/user"
"$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio --hood N002 --hood-lot-click 10 --click 595,592 --wait 60 \
    --click 139,690 --click 236,628 --click 528,398 --wait 2 --click 807,104 --click 595,592 --wait 120 \
    --screenshot "$out/lot3.png" -ApplePersistenceIgnoreState YES > "$out/run.log" 2>&1
fail=0
switches=$(grep -c "scene switch" "$out/run.log")
if [ "$switches" -lt 3 ]; then echo "FAIL: $switches scene switches (lot, neighborhood, lot expected)"; fail=1; fi
if ! grep -q "N002_Lot3" "$out/run.log"; then echo "FAIL: N002_Lot3 not loaded"; fail=1; fi
if grep -q "not a fence's" "$out/run.log"; then
    echo "FAIL: fence pieces took over other instances"; grep "not a fence's" "$out/run.log" | head -3; fail=1
fi
[ $fail = 0 ] && echo "scene switch: ok"
rm -rf "$out/user"
exit $fail
