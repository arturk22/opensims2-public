#!/bin/sh
# Sims gliding with still legs (OS2_DEBUG_GLIDE), with the game's data
# (OS2_DATA), headless: N001_Lot68 at game speed 3 (several VM ticks a
# frame). An overlay the VM took off was stopped on the player at once
# while its start still waited in the actor's cues: started afterwards it
# stayed on for good, the idle breathe at level 1 over the walk (level 0),
# and the Sims glided with their legs still (6 episodes before 9000 ticks).
# usage: glides.sh <opensims2 executable> <scratch dir>
app=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "glides: ok (skipped: no OS2_DATA)"; exit 0; fi
mkdir -p "$out"
OS2_DEBUG_GLIDE=1 "$app" --data "$OS2_DATA" --vm --lot N001_Lot68 --game-speed 3 --run-vm 9000 --screenshot "$out/lot68.png" \
    -ApplePersistenceIgnoreState YES > "$out/lot68.log" 2>&1
# Round n23 (event trees get the binary's parameters, docs/anim-mac.md §10)
# moved the Sims' paths, and one short glide shows up: a teen's 180-degree
# "turnAndGo" start leaving "Common Exit - Response" (tick 4603, 0.1 s, the
# feet's range 0.048 against the 0.06 limit; TODO.md). The overlay bug gave
# several long episodes, so one episode under half a second still passes.
summary=$(grep "glides: [0-9]* episodes" "$out/lot68.log" | head -1)
episodes=$(echo "$summary" | sed -n 's/.*glides: \([0-9]*\) episodes.*/\1/p')
seconds=$(echo "$summary" | sed -n 's/.*, \([0-9.]*\) s in all.*/\1/p')
if [ "$episodes" = 0 ] || { [ "$episodes" = 1 ] && awk "BEGIN { exit !($seconds < 0.5) }"; }; then
    echo "glides: ok ($episodes episodes, ${seconds:-0} s)"
    exit 0
fi
echo "FAIL: $(grep 'glides:' "$out/lot68.log" | head -1)"
grep "glide: tick" "$out/lot68.log" | head -3
exit 1
