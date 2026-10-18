#!/bin/sh
# The fireplace tool with the game's data (OS2_DATA), headless (round n48,
# docs/buy-build-mac.md §7.26): a saved fireplace sold with the sledgehammer
# and placed again where it was makes the saved chain piece by piece; a
# mantle put in front of a chimney goes into its chain; one in front of the
# cap at a new story's height makes the chimney's top again; undo gives the
# links back. The saved fireplaces against the tool's chimneys
# (fireplace-sweep).
# Build mode's BA30 in the app: the sledgehammer and the tool's click.
# usage: fireplace.sh <s2pkg executable> <opensims2 executable> <scratch dir>
s2pkg=$1; app=$2; out=$3
if [ -z "$OS2_DATA" ]; then echo "fireplace: ok (skipped: no OS2_DATA)"; exit 0; fi
mkdir -p "$out"
fail=0
lots="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001/Lots"
# N001_Lot68: the Loft fireplace 525 at (16, 11) on level 2 facing 2.
"$s2pkg" lot-save "$lots/N001_Lot68.package" "$out/lot68.package" --delete-fireplace 525 --fireplace 0,16,11,2,2 \
    --check-fireplaces 1 > "$out/lot68.log" 2>&1
grep -q -- "--delete-fireplace 525: a fireplace of 10 objects sold for §2525" "$out/lot68.log" ||
    { echo "FAIL sledgehammer: $(grep -- '--delete-fireplace' "$out/lot68.log")"; fail=1; }
grep -q -- "--fireplace 0,16,11,2,2: .* placed with 9 chimney pieces for §2500" "$out/lot68.log" ||
    { echo "FAIL place: $(grep -- '--fireplace' "$out/lot68.log" | cut -c1-200)"; fail=1; }
grep -q -- "--check-fireplaces 1: 2 fireplaces, 2 the tool's chimney" "$out/lot68.log" ||
    { echo "FAIL lot68 chain: $(grep -A2 DIFFERS "$out/lot68.log")"; fail=1; }
# N001_Lot59: the two Loft mantles of one chimney (levels 2 and 3), placed
# again one after the other; undo takes the second out of the chain.
"$s2pkg" lot-save "$lots/N001_Lot59.package" "$out/lot59.package" --delete-fireplace 668 --fireplace 0,28,14,0,2 \
    --fireplace 0,28,14,0,3 --check-fireplaces 1 --build-undo 1 --check-fireplaces 1 > "$out/lot59.log" 2>&1
grep -q -- "--fireplace 0,28,14,0,3: .* placed with 0 chimney pieces" "$out/lot59.log" ||
    { echo "FAIL in front of a chimney: $(grep -- '--fireplace 0,28,14,0,3' "$out/lot59.log" | cut -c1-200)"; fail=1; }
want="C(29.5,14.5,4)+5 C(29.5,14.5,4)+4 C(29.5,14.5,4)+3 C(29.5,14.5,4)+2 C(29.5,14.5,4)+1 F(29.5,14.5,4)+0 H(29.5,14.5,3)+0 M(28.5,14.5,3)+0 H(29.5,14.5,2)+0 M(28.5,14.5,2)+0 B(29.5,14.5,1)+0"
grep -q "chain $want" "$out/lot59.log" || { echo "FAIL lot59: the two mantles' chain is not the saved one"; fail=1; }
grep -q -- "--check-fireplaces 1: 3 fireplaces, 3 the tool's chimney" "$out/lot59.log" &&
    grep -q -- "--check-fireplaces 1: 2 fireplaces, 2 the tool's chimney" "$out/lot59.log" ||
    { echo "FAIL lot59 undo: $(grep -- '--check-fireplaces' "$out/lot59.log")"; fail=1; }
# N001_Lot82: a room on level 4 over the Moroccan chimney whose cap is there
# (elevation 0); the new mantle makes the top again on level 5.
"$s2pkg" lot-save "$lots/N001_Lot82.package" "$out/lot82.package" --build-funds 100000 --room 42,11,46,15,4 \
    --floor 42,11,45,14,0x2cc26a7e,4 --fireplace 4,45,13,0,4 --check-fireplaces 1 > "$out/lot82.log" 2>&1
want="C(46.5,13.5,5)+2 C(46.5,13.5,5)+1 F(46.5,13.5,5)+0 H(46.5,13.5,4)+0 M(45.5,13.5,4)+0 H(46.5,13.5,3)+0 M(45.5,13.5,3)+0 H(46.5,13.5,2)+0 M(45.5,13.5,2)+0 B(46.5,13.5,1)+0"
grep -q "same .* at (45.5,13.5,4) dir 0:" "$out/lot82.log" && grep -q "chain $want" "$out/lot82.log" ||
    { echo "FAIL lot82: $(grep -A2 '45.5,13.5,4' "$out/lot82.log" | cut -c1-300)"; fail=1; }
# Every saved fireplace of N001 and N002.
"$s2pkg" fireplace-sweep "$OS2_DATA"/TSData/Res/UserData/Neighborhoods/N00[12]/Lots/*.package > "$out/sweep.log" 2>&1
grep -q "fireplace-sweep: .* 25 fireplaces, 22 the tool's chimney (13 of them in a chain with other mantles, 10 of those" "$out/sweep.log" ||
    { echo "FAIL sweep: $(tail -1 "$out/sweep.log")"; fail=1; }
# The app: Build mode on N001_Lot68, the sledgehammer on 525, then the
# Loft fireplace held and clicked at (16, 11) on level 2 facing 2.
"$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio --lot N001_Lot68 --vm --mode build --delete-fireplace 525 \
    --build-level 2 --fireplace 0,16,11,2 --screenshot "$out/app.png" -ApplePersistenceIgnoreState YES > "$out/app.log" 2>&1
grep -q "build: --delete-fireplace 525: a fireplace of 10 objects sold for §2525" "$out/app.log" &&
    grep -q "build mode: .* placed with 9 chimney pieces for §2500; funds §2.275" "$out/app.log" ||
    { echo "FAIL app: $(grep 'build' "$out/app.log" | tail -4)"; fail=1; }
rm -rf "$out/user"
[ $fail = 0 ] && echo "fireplace: ok"
exit $fail
