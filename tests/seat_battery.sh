#!/bin/sh
# The seat battery (round 21, the seat module): drawn places with the game's
# data (OS2_DATA), headless. Each case sends a Sim to sit, lie, stand at or
# swim in something, then (OS2_THEN_AT) to get up, change seats or climb out,
# and checks every frame (OS2_DEBUG_SEATS, app/seat_check.hpp; OS2_DEBUG_JUMPS):
#   - no drawn jump over a tile without a VM reason (a Snap, a seat in or out);
#   - in a seat at rest, the pelvis over the slot (sideways < 0.25, height in range);
#   - hands that reach a goal fully on end within 6 cm of it;
#   - at rest, no body point through a table's top or a chair the Sim is not in;
#   - no carry pose with empty hands;
#   - seated at rest, no foot behind the pelvis (legs folded back through the seat).
# usage: seat_battery.sh <opensims2 executable> <scratch dir> [case...]
# Prints one line per case and "seat battery: ok" when all pass. SEAT_SHOTS=1
# also saves frames of each case (close-ups of the Sim) for the eye.
app=$1; out=$2; shift 2
if [ -z "$OS2_DATA" ]; then echo "seat battery: ok (skipped: no OS2_DATA)"; exit 0; fi
mkdir -p "$out"
fail=0
only="$*"
# run <name> <then> <lot> <sim> <ticks> <app args...>
run() {
    name=$1; then=$2; lot=$3; sim=$4; ticks=$5; shift 5
    if [ -n "$only" ] && ! echo " $only " | grep -q " $name "; then return; fi
    frames=""
    [ -n "$SEAT_SHOTS" ] && frames="--frames 6 --track-sim --zoom 3"
    OS2_THEN_AT="$then" OS2_DEBUG_SEATS=${SEATS_SIM:-1} OS2_DEBUG_JUMPS=1 OS2_LOG_ANIMS=1 "$app" --data "$OS2_DATA" --vm --walls down \
        --lot "$lot" --sim-id "$sim" --run-vm "$ticks" $frames --screenshot "$out/$name.png" "$@" \
        -ApplePersistenceIgnoreState YES > "$out/$name.log" 2>&1
    seats=$(grep 'seats: .* problems' "$out/$name.log" | sed 's/opensims2: seats: //')
    jumps=$(grep 'jumps:' "$out/$name.log" | sed 's/opensims2: jumps: //; s/ drawn jumps without a VM reason.*//')
    printf "%-12s jumps %s; %s\n" "$name" "${jumps:-?}" "${seats:-no summary}"
    if [ "$jumps" != 0 ] || ! echo "$seats" | grep -q "seat 0 .*ik 0 .*table 0 .*chair 0 .*carry 0 .*legs 0 "; then
        grep "seat check:\|drawn jump" "$out/$name.log" | head -4 | sed 's/^/    /'
        fail=1
    fi
}
# Dining chair, normal: sit, then get up and walk off.
run dining   "1300:here:21,22,2" N001_Lot68 Angela 1700 --pie "Chair - Dining" --pie-select 0
# Breakfast: the bowl carried to the table, the leaveOut get-in with it, eating, putting it down.
run eat      "" N001_Lot68 Angela 3000 --pie Fridge --pie-select 3
# A child at the patio dining chair (N001_Lot11, Alexander): the seated
# c2o-chairDining clips are made facing 180 degrees (their header heading)
# and hold the legs by foot IK only; turned twice, the feet went behind the
# seat (round 21 childseat: kneeling in the chair, shins through its back).
# (Not the get-up yet: its right hand on the chair's r_childseatgrab_surface
# falls 0.2 short for 0.7 s, before and after this fix; TODO.)
run child    "" N001_Lot11 Alexander 1300 --pie "Chair - Dining" --pie-select 0
# Bar stool, then up.
run stool    "1300:here:21,22,2" N001_Lot68 Angela 1700 --pie Stool --pie-select 0
# Armchair, then change seats to the sofa.
run armchair "1100:Sofa:Sit" N001_Lot11 Alexander 2200 --pie "Chair - Living" --pie-select 0
# Sofa and loveseat.
run sofa     "" N001_Lot11 Alexander 1500 --pie Sofa --pie-select 0
run loveseat "" N002_Lot3 Circe 560 --pie Loveseat --pie-select 0
# Bed (relax), toilet, bathtub.
run bed      "" N001_Lot11 Alexander 1500 --pie Bed --pie-select 0
run toilet   "" N001_Lot68 Angela 2000 --pie Toilet --pie-select 0
run tub      "" N001_Lot68 Angela 2500 --pie Tub --pie-select 0
# Standing at the counter (a drink at the sink) and at the coffee maker.
run sink     "" N001_Lot11 Alexander 1200 --pie Counter --pie-select 0
run coffee   "" N001_Lot68 Angela 1500 --pie "Coffee Machine" --pie-select 0
# Chess at the dining chairs (N001_Lot11, Don by himself after the party):
# the pieces' reaches are the board's seated ones (its Reach Animations),
# not the standing "a2o-table-get" that stood him up through the chair.
SEATS_SIM=49 run chess "" N001_Lot11 Don 8960
# Pool: dive in, swim, climb out by the ladder.
run pool     "1500:here:9,14,1" N001_Lot68 Daniel 3000 --pie Pool --pie-select 0
[ $fail = 0 ] && echo "seat battery: ok"
exit $fail
