#!/bin/sh
# A lot saved at night goes on once loaded ("à noite todo mundo fica parado
# sem fazer nada", fix-noite): N001_Lot11 is run from 17:02 to 22:02 (9000
# ticks) and saved (lot-save keeps the VM's tick count, vm/thread_state),
# then loaded in the app, headless, for three game hours (to 01:02). The VM
# must tick on from the saved count at once and the Sims pick things to do.
# Before, the app's scene clock started at 0 and the VM waited for it to
# reach the saved count (9000 ticks: over 6 minutes at speed 1) with every
# Sim standing still: the shots all said 9000 ticks.
# usage: saved_lot_night.sh <s2pkg> <opensims2 executable> <scratch dir>
s2pkg=$1; app=$2; out=$3
if [ -z "$OS2_DATA" ]; then echo "saved lot night: ok (skipped: no OS2_DATA)"; exit 0; fi
rm -rf "$out"; mkdir -p "$out/N001/Lots" "$out/user"
hood="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001"
ln -s "$hood/N001_Neighborhood.package" "$out/N001/N001_Neighborhood.package"
ln -s "$hood/Characters" "$out/N001/Characters"
lot="$out/N001/Lots/N001_Lot11.package"
"$s2pkg" lot-save "$hood/Lots/N001_Lot11.package" "$lot" --ticks 9000 > "$out/save.log" 2>&1 ||
    { echo "FAIL: lot-save: $(tail -n 2 "$out/save.log")"; exit 1; }
log="$out/app.log"
OS2_AUTONOMY_LOG=1 "$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio --lot "$lot" --run-vm 5400 --frames 3 \
    --screenshot "$out/night.png" -ApplePersistenceIgnoreState YES > "$log" 2>&1
fail=0
grep -q "fresh Mains, 22:02," "$log" || { echo "FAIL: the lot did not load at 22:02: $(grep 'VM:' "$log")"; fail=1; }
# The VM ticks at each shot (one per game hour of the run).
ticks=$(sed -n 's/.*wrote .*night-[123]\.png (.* s, \([0-9]*\) VM ticks).*/\1/p' "$log" | tr '\n' ' ')
set -- $ticks
[ $# -eq 3 ] || { echo "FAIL: shots: $ticks"; exit 1; }
[ "$1" -ge 10700 ] && [ $(($2 - $1)) -ge 1700 ] && [ $(($3 - $2)) -ge 1700 ] ||
    { echo "FAIL: the VM stood still after loading: ticks $1 $2 $3 at the three game hours (want 9000 + 1800 each)"; fail=1; }
# Autonomous picks after the load, per game hour.
picks=$(awk '/^autonomy: tick [0-9]+ Sim [0-9]+: "/ { h = int(($3 - 9000) / 1800); if (h >= 0 && h < 3) n[h]++ }
             END { printf "%d %d %d", n[0], n[1], n[2] }' "$log")
set -- $picks
[ $(($1 + $2 + $3)) -ge 3 ] || { echo "FAIL: nobody acted after the load: picks per hour $picks"; fail=1; }
[ $fail = 0 ] && echo "saved lot night: ok (VM ticks $ticks; autonomous picks per hour $picks)"
exit $fail
