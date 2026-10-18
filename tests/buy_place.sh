#!/bin/sh
# Buy mode's object tool with the game's data (OS2_DATA), headless, on the
# tutorial lot ("O Básico - Parte Um", where the playtest's click placed
# nothing, round 21): the footprint is the game's outline with the facing
# chevron; a placement charges the price and, for a "Keep Buying" object
# (the dining chair), holds another at once; a press-drag-release turns the
# object to face the drag before it is placed; Esc drops what is held.
# The hand (round 21 cursor): a placed chair picked up shows its outline at
# once (none until the mouse moved, playtest 38.png); a press-drag-release
# on it carries it to the release's tile; a press-release keeps it held.
# usage: buy_place.sh <opensims2 executable> <scratch dir>
app=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "buy place: ok (skipped: no OS2_DATA)"; exit 0; fi
mkdir -p "$out"
fail=0
lot="$OS2_DATA/TSData/Res/UserData/Neighborhoods/Tutorial/Lots/Tutorial_Lot14.package"
# The §615 dining chair (0x8C3FE510): placed at (14, 12) facing 2; then
# dragged from (16, 12) toward -y (facing 6) and from (17, 12) toward +x
# (facing 4); the next one dropped with Esc. (15, 12) and (15, 13) are the
# two sides of a door, whose parts' footprints take a chair as the game's
# objects test does.
"$app" --data "$OS2_DATA" --vm --ui live --lot "$lot" --mode buy --buy 0x8C3FE510 --place 14,12,2 \
    --buy-drag 16,12,16.5,9 --buy-drag 17,12,20,12.5 --buy-cancel \
    --grab 43 --buy-cancel --grab-drag 43,13,13 --grab-drag 47,16,12 --screenshot "$out/chairs.png" \
    -ApplePersistenceIgnoreState YES > "$out/chairs.log" 2>&1
log="$out/chairs.log"
grep -q "buy mode: footprint from objectPlacementIconGood/Bad" "$log" || { echo "FAIL: no placement outline models"; fail=1; }
grep -q "bought Cadeira de Jantar Carvalhosa as object [0-9]* at (14, 12) facing 2 for §615; funds §2.219" "$log" ||
    { echo "FAIL place: $(grep 'buy mode' "$log" | head -4)"; fail=1; }
n=$(grep -c "buy mode: holding Cadeira de Jantar Carvalhosa" "$log")
[ "$n" = 4 ] || { echo "FAIL keep buying: held $n times (want 4: picked, then after each of 3 placements)"; fail=1; }
grep -q "at (16, 12) facing 6 for §615; funds §1.604" "$log" || { echo "FAIL drag -y: $(grep 'at (16, 12)' "$log")"; fail=1; }
grep -q "at (17, 12) facing 4 for §615; funds §989" "$log" || { echo "FAIL drag +x: $(grep 'at (17, 12)' "$log")"; fail=1; }
grep -q "buy mode: dropped what was held" "$log" || { echo "FAIL: Esc dropped nothing"; fail=1; }
grep -q "picked up object 43 (.*); outline on (14, 12)" "$log" || { echo "FAIL: no outline on the grabbed chair"; fail=1; }
grep -q "buy mode: moved object 43" "$log" || { echo "FAIL: the grab-drag did not move the chair"; fail=1; }
grep -q "picked up object 47 (.*); outline on (16, 12)" "$log" && ! grep -q "moved object 47" "$log" ||
    { echo "FAIL: a press-release on a chair should keep it held"; fail=1; }
[ $fail = 0 ] && echo "buy place: ok"
exit $fail
