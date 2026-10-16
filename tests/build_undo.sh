#!/bin/sh
# Build mode's undo and redo give the lot back byte for byte (round n20,
# docs/buy-build-mac.md §7.24): s2pkg lot-save after a run of edits undone
# writes the same package as the lot saved untouched, and the run undone and
# redone the same as the run itself: on N001_Lot11 (a family's lot: the
# funds come back) a room, a floor, wallpaper, a fence, a foundation, a
# pool, a roof, a terrain stroke and a wall deleted; on N002_Lot23 a wall
# deleted with the door in it (sold, then back); on N001_Lot38 a modular
# flight sold and laid again.
# usage: build_undo.sh <s2pkg executable> <scratch dir>
s2pkg=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "build undo: ok (skipped: no OS2_DATA)"; exit 0; fi
rm -rf "$out"; mkdir -p "$out"
lots="$OS2_DATA/TSData/Res/UserData/Neighborhoods"
fail=0
same() { # <a> <b> <what>
    r=$("$s2pkg" compare "$1" "$2" | tail -1)
    echo "$r" | grep -q " 0 differ, 0 missing, 0 extra" || { echo "FAIL $3: $r"; fail=1; }
}
lot="$lots/N001/Lots/N001_Lot11.package"
set -- --room 2,2,6,6 --floor 2,2,5,5,carpete --wallpaper 2,2,6,2,b,listras --fence 2,9,6,9,picket \
    --foundation 17,3,19,6 --pool 13,3,15,6 --roof 2,2,6,6,5,2 --terrain raise,4,20,5,1 --delete-wall 2,2,2,6
undo=""; redo=""
for i in 1 2 3 4 5 6 7 8 9; do undo="$undo --build-undo 1"; redo="$redo --build-redo 1"; done
"$s2pkg" lot-save "$lot" "$out/base.package" > "$out/base.log" 2>&1
"$s2pkg" lot-save "$lot" "$out/direct.package" "$@" > "$out/direct.log" 2>&1 || { echo "FAIL the edits: $(grep FAILED "$out/direct.log")"; fail=1; }
"$s2pkg" lot-save "$lot" "$out/undone.package" "$@" $undo > "$out/undone.log" 2>&1 || { echo "FAIL undo: $(grep FAILED "$out/undone.log")"; fail=1; }
"$s2pkg" lot-save "$lot" "$out/redone.package" "$@" $undo $redo > "$out/redone.log" 2>&1 || { echo "FAIL redo: $(grep FAILED "$out/redone.log")"; fail=1; }
grep -q "^  --build-undo 1: done (funds §511632)" "$out/undone.log" || { echo "FAIL the funds after undo: $(tail -3 "$out/undone.log")"; fail=1; }
same "$out/base.package" "$out/undone.package" "Lot11 undone"
same "$out/direct.package" "$out/redone.package" "Lot11 redone"
lot="$lots/N002/Lots/N002_Lot23.package"
# (The deck's stairs by the room's corner go first: tests/door_cut_wall.sh.)
"$s2pkg" lot-save "$lot" "$out/door.package" --delete-stairs 289 --room 19,6,23,2 --door 0x2c77520c@23,4 > "$out/door.log" 2>&1
"$s2pkg" lot-save "$lot" "$out/door-undone.package" --delete-stairs 289 --room 19,6,23,2 --door 0x2c77520c@23,4 --delete-wall 23,2,23,6 \
    --build-undo 1 > "$out/door-undone.log" 2>&1
grep -q "1 attached objects sold" "$out/door-undone.log" || { echo "FAIL the door is not sold with its wall"; fail=1; }
same "$out/door.package" "$out/door-undone.package" "the wall and its door back"
lot="$lots/N001/Lots/N001_Lot38.package"
"$s2pkg" lot-save "$lot" "$out/stairs.package" > "$out/stairs.log" 2>&1
"$s2pkg" lot-save "$lot" "$out/stairs-undone.package" --delete-stairs 147 --stairs 0,18,16,2,0 --build-undo 1 \
    --build-undo 1 > "$out/stairs-undone.log" 2>&1 || { echo "FAIL stairs: $(grep FAILED "$out/stairs-undone.log")"; fail=1; }
same "$out/stairs.package" "$out/stairs-undone.package" "the flight back"
[ $fail = 0 ] && echo "build undo: ok"
exit $fail
