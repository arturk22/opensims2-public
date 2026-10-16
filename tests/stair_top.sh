#!/bin/sh
# The stair-top and porch-stoop standoffs of N001_Lot11 (docs/routing-mac.md
# §9.2, §9.3): the stoop at the top of the porch stairs (8, 12, level 2) is a
# single tile between the deck stair's top and the front door.
#  - Round n18: Cassandra, coming up, and Alexander, coming out of the door
#    and turning past her, stepped into each other unpredicted (the
#    prediction along the straight lines between the route points) and
#    blocked each other for good: 36 per-step blocks by tick 3000 (seed 2).
#    Gate: at most 2 per-step blocks into a Sim on the stoop, seeds 1-8.
#  - Rounds n18b/n23: the two stood on each other's portal end and failed
#    their routes for the rest of the game (seed 4, then seeds 2 and 4: 28
#    and 60 failed routes by tick 3000). Round n30 read the binary's move
#    out of the way (a 0.3-tile step), its try counter, CT - Route Blocked
#    and the asker told when the asked Sim's route ends, and they pass.
#    Gate: at most 2 failed routes on the stoop at seeds 2 and 4 (not
#    counted: the dynamic social route's designed 13; the asked Sim's
#    "Interaction - Move Out Of Way", "Common Entry - Okay to Stand Here?",
#    "Command - Reposition" and the failure feedback's own walk, which
#    find no place on the stoop by design: its room is the outdoors of a
#    level whose only floor there is the stoop).
#  - What the binary does not undo (§9.3): a Sim that stops on the stoop
#    (a social with one standing there, a step out of another's way) can
#    still hold it for a while, nobody having a place to step aside to; at
#    seeds 6 (3000 ticks) and 10 and 21 of 0-27 this happens. Reported per
#    seed, not gated.
# usage: stair_top.sh <s2pkg>
s2pkg=$1
if [ -z "$OS2_DATA" ]; then echo "stair top: ok (skipped: no OS2_DATA)"; exit 0; fi
lot="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot11.package"
stoop='at \(8\.[4-9][0-9], 12\.[0-9]+, 2\)'
bad=0
for seed in 1 2 3 4 5 6 7 8; do
    err=$(OS2_SEED=$seed OS2_ROUTE_FAILS=1 OS2_DEBUG_BLOCK=1 "$s2pkg" autonomy "$lot" --ticks 3000 2>&1 >/dev/null)
    fails=$(echo "$err" | grep 'route of' | grep -E "$stoop" |
            grep -vE 'failed with 13 in "(Route - Dynamic w/Lead in|Route - Refresh route)"' |
            grep -vcE '"(Interaction - Move Out Of Way|Common Entry - Okay to Stand Here\?|Command - Reposition|CT - Route Failure Feedback)"')
    steps=$(echo "$err" | grep 'step into' | grep '(person)' | grep -cE "$stoop")
    echo "stair top (N001_Lot11, seed $seed, 3000 ticks): $steps per-step blocks into a Sim on the stoop, $fails failed routes there"
    if [ "$steps" -gt 2 ]; then bad=1; fi
    if { [ "$seed" = 2 ] || [ "$seed" = 4 ]; } && [ "$fails" -gt 2 ]; then bad=1; fi
done
if [ "$bad" = 0 ]; then
    echo "stair top: ok"
else
    echo "stair top: FAILED"
fi
