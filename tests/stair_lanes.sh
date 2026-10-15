#!/bin/sh
# Stairs as the binary's slot portals (docs/routing-mac.md §8.6): each stair
# has a lane up and a lane down, so two Sims crossing on the straight stair
# of N001_Lot11 both get through. Mortimer goes up from the bottom landing
# to the cabinet 169 upstairs while Cassandra comes down from the top
# landing to the piano 184; with the old single portal (OS2_STAIR_ONE_PORTAL)
# he was blocked by her on the shared landing point and failed with 14.
# usage: stair_lanes.sh <s2pkg>
s2pkg=$1
if [ -z "$OS2_DATA" ]; then echo "stair lanes: ok (skipped: no OS2_DATA)"; exit 0; fi
lot="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot11.package"
out=$(S2PKG_ROUTE_AT=15.5,12.6,2 S2PKG_ROUTE_ALSO=Cassandra,184,15.5,18.7,3 \
      "$s2pkg" route "$lot" Mortimer 169 --ticks 3000 2>&1)
echo "$out" | grep -E "arrived|failed|person data 62"
if echo "$out" | grep -q "arrived" && echo "$out" | grep -q "person data 62): 0,"; then
    echo "stair lanes: ok"
else
    echo "stair lanes: FAILED"
fi
