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
[ $fail = 0 ] && echo "drawn places: ok"
exit $fail
