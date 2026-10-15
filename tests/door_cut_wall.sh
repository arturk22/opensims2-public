#!/bin/sh
# Doors and windows on walls (issue 002), headless with the game's data
# (OS2_DATA), on N002_Lot23's flat corner (a 4x4 room at 22,2-26,6):
# - the object tool turns what needs a wall to it (TryPlace ->
#   AutoRotateObjectToMatchWalls, docs/buy-build-mac.md §7.6.1): a door,
#   window, arch and fence gate given without a direction are placed, and the
#   saved lot has the door facing its wall; with no wall the game's rule
#   stays ("needs a wall");
# - in the app, Build mode, walls cut away: the mouse over the yard by the
#   cut front wall holds the door there (shown, not hidden with its wall) and
#   the press places it; walls up, it is there.
# usage: door_cut_wall.sh <s2pkg> <opensims2 executable> <scratch dir>
s2pkg=$1; app=$2; out=$3
if [ -z "$OS2_DATA" ]; then echo "door cut wall: ok (skipped: no OS2_DATA)"; exit 0; fi
rm -rf "$out"; mkdir -p "$out/user"
fail=0
lot="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N002/Lots/N002_Lot23.package"
door=0x2c77520c; window=0xec6a6fb8; arch=0xcc0c1bfb; gate=0x2d124597
"$s2pkg" lot-save "$lot" "$out/doors.package" --room 22,2,26,6 --door $door@26,4 --door $window@24,5 \
    --door $arch@23,2 --fence 22,10,26,10,picket --door $gate@24,9 > "$out/doors.log" 2>&1
log="$out/doors.log"
grep -q "@26,4: Porta Simples Produtos Lenhosos bought as object 151" "$log" || { echo "FAIL door: $(grep '@26,4' "$log")"; fail=1; }
grep -q "@24,5: .* bought as object" "$log" || { echo "FAIL window: $(grep '@24,5' "$log")"; fail=1; }
grep -q "@23,2: .* bought as object" "$log" || { echo "FAIL arch: $(grep '@23,2' "$log")"; fail=1; }
grep -q "@24,9: .* bought as object" "$log" || { echo "FAIL gate: $(grep '@24,9' "$log")"; fail=1; }
"$s2pkg" lot-state "$out/doors.package" --id 151 > "$out/state.log" 2>&1
grep -q "XMTO 151 at (26.50, 4.50, 1) elevation 0 dir 4" "$out/state.log" ||
    { echo "FAIL saved door: $(grep 'XMTO 151' "$out/state.log")"; fail=1; }
"$s2pkg" lot-save "$lot" "$out/none.package" --door $door@4,4 > "$out/none.log" 2>&1
grep -q "@4,4: FAILED: .*needs a wall" "$out/none.log" || { echo "FAIL no wall: $(grep '@4,4' "$out/none.log")"; fail=1; }
# A diagonal wall (4,6)-(6,4): the door and the window turn into their
# diagonal twins (SwapDiagonalObject, OBJD diagonal selector), facing it.
"$s2pkg" lot-save "$lot" "$out/diag.package" --wall 4,6,6,4 --door $door@5,4 --door $window@4,5 > "$out/diag.log" 2>&1
grep -q "turned to direction 4 (Door - 1 Tile - Diagonal - Value 0,0)" "$out/diag.log" &&
    grep -q "@5,4: .* bought as object .*Door - 1 Tile - Diagonal - Value 0,0 at (5,4,1)" "$out/diag.log" ||
    { echo "FAIL diagonal door: $(grep -i 'door' "$out/diag.log")"; fail=1; }
grep -q "@4,5: .* bought as object .*Window - Privacy - Diagonal" "$out/diag.log" ||
    { echo "FAIL diagonal window: $(grep '@4,5' "$out/diag.log")"; fail=1; }
# The diagonal windows N002_Lot3 was saved with fit where they stand (the
# diagonal wall placement flags 0x40/0x80 read on the right diagonal).
lot3="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N002/Lots/N002_Lot3.package"
"$s2pkg" buy "$lot3" --move 1120@27,36,6,2 --move 1407@35,31,2,3 --move 1423@28,25,4,3 > "$out/lot3.log" 2>&1 ||
    { echo "FAIL N002_Lot3 diagonal windows: $(grep move "$out/lot3.log")"; fail=1; }
# The app: the mouse at (555, 434) is over the yard at the cut x = 26 wall.
run() { # <name> <walls> <steps...>
    name=$1; walls=$2; shift 2
    rm -rf "$out/user"; mkdir -p "$out/user"
    "$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio --hood N002 --hood-lot 23 --vm --mode build \
        --walls "$walls" --pivot 24,4 --zoom 1 --room 22,2,26,6 --door $door "$@" -ApplePersistenceIgnoreState YES \
        > "$out/$name.log" 2>&1
}
run hold cutaway --hover 555,434 --screenshot "$out/hold.png"
grep -q "build mouse over at lot (26\.[0-9]*, 4\.[0-9]*)" "$out/hold.log" || { echo "FAIL hover: $(grep 'build mouse' "$out/hold.log")"; fail=1; }
run place cutaway --drag 555,434,555,434 --screenshot "$out/place.png"
grep -q "build mode: bought Porta Simples Produtos Lenhosos" "$out/place.log" ||
    { echo "FAIL click on the cut wall: $(grep 'build drag' "$out/place.log")"; fail=1; }
run up up --drag 555,434,555,434 --screenshot "$out/up.png"
grep -q "build mode: bought Porta Simples Produtos Lenhosos" "$out/up.log" || { echo "FAIL walls up"; fail=1; }
# The app's Build mode, the same diagonal wall: the door bought is the diagonal one.
rm -rf "$out/user"; mkdir -p "$out/user"
"$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio --hood N002 --hood-lot 23 --vm --mode build --walls up \
    --wall 4,6,6,4 --door $door@5,4 --screenshot "$out/diag.png" -ApplePersistenceIgnoreState YES > "$out/diag-app.log" 2>&1
grep -q "build mode: bought Porta Simples Produtos Lenhosos as object [0-9]* (Door - 1 Tile - Diagonal - Value)" "$out/diag-app.log" ||
    { echo "FAIL app diagonal door: $(grep 'build mode' "$out/diag-app.log")"; fail=1; }
[ $fail = 0 ] && echo "door cut wall: ok"
rm -rf "$out/user"
exit $fail
