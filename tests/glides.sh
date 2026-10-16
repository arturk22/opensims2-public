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
# Round n23 let one short glide through (a teen's 180-degree turnAndGo at
# tick 4603, 0.1 s, the feet's range 0.048); round n30 found its cause: a
# dialog paused the VM for 19 s while the actors' clock ran on, the start's
# clip played out under the dialog and, once it was answered, the VM walked
# the Sim along the rest of the start with the legs held in its last frame.
# The scene clock now stops with a dialog's pause (LotSimulation::speedScale):
# no episode at all again.
if grep -q "glides: 0 episodes" "$out/lot68.log"; then
    echo "glides: ok"
    exit 0
fi
echo "FAIL: $(grep 'glides:' "$out/lot68.log" | head -1)"
grep "glide: tick" "$out/lot68.log" | head -3
exit 1
