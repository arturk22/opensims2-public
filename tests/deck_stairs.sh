#!/bin/sh
# The visitors on N003_Lot192's level-2 deck leave (docs/routing-mac.md
# §9.4). Juliette and Tybalt are saved on the deck, whose only ways down are
# the 6-step modular stairs; their steps end mid-tile (y 27.5) and the lower
# ends of the stairs' two lanes (the binary's slot portals, round n03) are
# at y 27.71, on the floor half of the "End" piece's tile. The VM closed the
# whole tile under the steps (World::addStair's "no floor"), which the binary
# does not (the pieces block by their footprints, which end at 27.625), so
# the lanes had no way off them: "Leave - By Ped Portal" failed with 2 and
# the two started "Leave" ~115 times in 12 hours, never leaving. Now they
# leave by 00:10 with one "Leave" each.
# usage: deck_stairs.sh <s2pkg>
s2pkg=$1
if [ -z "$OS2_DATA" ]; then echo "deck stairs: ok (skipped: no OS2_DATA)"; exit 0; fi
lot="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N003/Lots/N003_Lot192.package"
out=$(S2PKG_VISITORS=1 "$s2pkg" autonomy "$lot" --ticks 7000 2>&1)
bad=0
for sim in Juliette Tybalt; do
    leaves=$(echo "$out" | grep -c "$sim#[0-9]* *running \"Leave\"")
    gone=$(echo "$out" | grep "$sim#[0-9]* *is gone" | head -1)
    echo "deck stairs: $sim started \"Leave\" $leaves times; ${gone:-never gone}"
    if [ -z "$gone" ] || [ "$leaves" -gt 2 ]; then bad=1; fi
done
if [ "$bad" = 0 ]; then
    echo "deck stairs: ok"
else
    echo "deck stairs: FAILED"
fi
