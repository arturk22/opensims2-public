#!/bin/sh
# Drawn places with the game's data (OS2_DATA), headless: Sims whose drawn
# place jumps without a VM reason (OS2_DEBUG_JUMPS), seats drawn far from
# their objects (OS2_DEBUG_POSES), a plate at the dining table's far end.
# usage: drawn_places.sh <opensims2 executable> <scratch dir>
app=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "drawn places: ok (skipped: no OS2_DATA)"; exit 0; fi
mkdir -p "$out"
fail=0
run() { # name, then the app's arguments
    name=$1; shift
    OS2_DEBUG_JUMPS=1 OS2_DEBUG_POSES=1 OS2_DEBUG_SLOT=1 "$app" --data "$OS2_DATA" --vm --walls down --screenshot "$out/$name.png" "$@" \
        -ApplePersistenceIgnoreState YES > "$out/$name.log" 2>&1
    if ! grep -q "jumps: 0 drawn jumps without a VM reason (0 back and forth), 0 Sims" "$out/$name.log"; then
        echo "FAIL $name: $(grep 'jumps:' "$out/$name.log")"; grep "drawn jump\|repeated" "$out/$name.log" | head -5; fail=1
    fi
    if grep -q "far from its seat" "$out/$name.log"; then
        echo "FAIL $name: a seat drawn far from its object"; grep "far from" "$out/$name.log" | head -3; fail=1
    fi
}
# The straight stairs' end (the climber flashed 1.5-3.4 tiles off at the top and the bottom).
run stairs --lot N001_Lot68 --sim-id Daniel --vm-go-here 14,16,3 --run-vm 900
# Breakfast at the dining table: the plate at slot 1 of the table's "1,0" part, in front of Angela at (17.5, 25.5).
run plate --lot N001_Lot68 --sim-id Angela --pie Fridge --pie-select 5 --run-vm 2300
plate=$(grep "slot: object .* in 639 slot 1 " "$out/plate.log" | tail -1)
y=$(echo "$plate" | sed 's/.*-> ([^ ]* \([^ ]*\) .*/\1/')
case "$y" in 25.[2-7]*) ;; *) echo "FAIL plate: drawn at y '$y' (${plate:-no plate in the table slot})"; fail=1 ;; esac
# The child reading in N001_Lot11 (the book's own carry animation sank him to the floor, back and forth).
run book --lot N001_Lot11 --run-vm 4600
# An empty lot (N002_Lot23): its PlumbBobs are out of the world (saved at
# (0, 0) in no slot) and their spin must not give them an actor, which was
# drawn floating past the lot's corner (live QA, round 20).
OS2_DEBUG_VM=1 "$app" --data "$OS2_DATA" --vm --ui live --lot N002_Lot23 --screenshot "$out/emptylot.png" \
    -ApplePersistenceIgnoreState YES > "$out/emptylot.log" 2>&1
n=$(grep -c "(PlumbBob_objt) out of the world: not animated" "$out/emptylot.log")
if [ "$n" != 2 ] || grep -q "object [0-9]* plays o-plumbob-spin" "$out/emptylot.log"; then
    echo "FAIL emptylot: $n out-of-world PlumbBobs left unanimated (want 2)"; fail=1
fi
# The selected Sim's PlumbBob in N001_Lot11 spins on: its saved Animate
# Object request has loops 0xFFFFFFFF (temp 1 = -1), played looping; it
# held its last frame after one turn.
OS2_DEBUG_VM=1 "$app" --data "$OS2_DATA" --vm --lot N001_Lot11 --run-vm 30 --screenshot "$out/plumbob.png" \
    -ApplePersistenceIgnoreState YES > "$out/plumbob.log" 2>&1
if ! grep -q "starts o-plumbob-spin (overlay [0-9]*, placed, looping)" "$out/plumbob.log"; then
    echo "FAIL plumbob: $(grep 'o-plumbob-spin' "$out/plumbob.log" || echo 'no spin started')"; fail=1
fi
# The floor buttons stop at the ground: N002_Lot31's pool makes a level -1,
# which Page Down showed with nothing but the terrain (live QA, round 20).
"$app" --data "$OS2_DATA" --vm --ui live --lot N002_Lot31 --key pagedown --screenshot "$out/floordown.png" \
    -ApplePersistenceIgnoreState YES > "$out/floordown.log" 2>&1
if ! grep -q "key pagedown: level shown [01]$" "$out/floordown.log"; then
    echo "FAIL floordown: $(grep 'key pagedown' "$out/floordown.log" || echo 'no level shown')"; fail=1
fi
[ $fail = 0 ] && echo "drawn places: ok"
exit $fail
