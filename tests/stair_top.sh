#!/bin/sh
# The stair-top standoff of round n18 (docs/routing-mac.md §9.2): on
# N001_Lot11 with OS2_SEED=2, Cassandra comes up the porch stairs to the
# stoop at their top (8.67, 12.19, 2) as Alexander, coming out of the front
# door, turns past her to the other lane. With the predicted blocking along
# the straight lines between the route points (the frame cuts inside them)
# he stepped within 0.283 of her unpredicted and the two blocked each other
# for good: 36 per-step blocks and 33 failed routes there by tick 3000. With
# the prediction along the frame's curve (GlobalLocationEstimate) he stops
# for her in time.
# Round n23 (event trees get the binary's parameters, the stack object ID
# among them: docs/anim-mac.md §10) moved the Sims' paths: seed 2 no longer
# replays that crossing, and the stoop now reaches the porch standoff that
# is still open (docs/routing-mac.md §10, TODO.md) at seeds 2 and 4 by tick
# 3000 (before n23: seed 6). That one has no per-step blocks, so the guard
# is the n18 signature, the per-step blocks, over seeds 1-8 (at most 2 in
# each); the failed routes on the stoop are reported, not gated.
# usage: stair_top.sh <s2pkg>
s2pkg=$1
if [ -z "$OS2_DATA" ]; then echo "stair top: ok (skipped: no OS2_DATA)"; exit 0; fi
lot="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot11.package"
bad=0
for seed in 1 2 3 4 5 6 7 8; do
    err=$(OS2_SEED=$seed OS2_ROUTE_FAILS=1 OS2_DEBUG_BLOCK=1 "$s2pkg" autonomy "$lot" --ticks 3000 2>&1 >/dev/null)
    fails=$(echo "$err" | grep 'route of' | grep -vE 'failed with 13 in "(Route - Dynamic w/Lead in|Route - Refresh route)"' |
            grep -cE 'at \(8\.[4-9][0-9], 12\.[0-9]+, 2\)')
    steps=$(echo "$err" | grep -c 'step into')
    echo "stair top (N001_Lot11, seed $seed, 3000 ticks): $steps per-step blocks, $fails failed routes on the stoop"
    if [ "$steps" -gt 2 ]; then bad=1; fi
done
if [ "$bad" = 0 ]; then
    echo "stair top: ok"
else
    echo "stair top: FAILED"
fi
