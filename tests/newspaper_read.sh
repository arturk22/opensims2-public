#!/bin/sh
# Reading the paper seated, with the game's data (OS2_DATA), headless
# (round 21, playtest: "o jornal aparece aberto e fechado ao mesmo tempo").
# Vidcund (N002_Lot4) takes the paper to the patio chair and reads it. While
# he plays the read animations the snapped models stand in for the paper he
# holds: the newspaper object (in his hand slot while he is himself in the
# chair's slot) must be hidden, the read start must show the rolled paper in
# the right hand from its first tick (its second copy sent to Oblivion at the
# same tick must not take it away), then the open one; and the paper's own
# animations (o2a-newspaper-read-*) must play on the snapped model.
# usage: newspaper_read.sh <opensims2 executable> <scratch dir>
app=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "newspaper read: ok (skipped: no OS2_DATA)"; exit 0; fi
mkdir -p "$out"
log="$out/read.log"
OS2_DEBUG_SNAPS=1 OS2_DEBUG_VM=1 "$app" --data "$OS2_DATA" --vm --walls down --lot N002_Lot4 --sim-id Vidcund --motives 60 \
    --pie Newspaper --pie-select 2 --run-vm 1250 --frames 60 --frames-from 15 --screenshot "$out/read.png" \
    -ApplePersistenceIgnoreState YES > "$log" 2>&1
fail=0
# The paper held while seated: its holder is the Sim, its outermost container the chair.
grep -q "snaps: object [0-9]* (newspaperrolled) hidden, held by 152 in [0-9]*" "$log" ||
    { echo "FAIL: the held paper was not hidden"; fail=1; }
grep "snaps: object [0-9]* (newspaperrolled) hidden, held by 152 in 152$" "$log" >/dev/null &&
    { echo "FAIL: not seated when the paper was hidden (the case needs the chair)"; fail=1; }
# The read start's first two seconds: the rolled paper in the right hand.
grep -q "sim 152 (10 snaps, [01]\.[0-9]* s in) shown \[ newspaper:newspaperRolled \]" "$log" ||
    { echo "FAIL: no rolled paper in the hand at the read start"; grep "sim 152 (10 snaps, [01]\." "$log" | head -2; fail=1; }
grep -q "sim 152 (10 snaps, [0-9.]* s in) shown \[ newspaperRead:newspaperReading \]" "$log" ||
    { echo "FAIL: no open paper"; fail=1; }
grep -q "sim 152 .* shown \[ [^ ]*newspaper[^ ]* [^ ]*newspaper[^ ]* \]" "$log" &&
    { echo "FAIL: two papers shown at once"; grep -m1 "sim 152 .* shown \[ [^ ]*newspaper[^ ]* [^ ]*newspaper[^ ]* \]" "$log"; fail=1; }
grep -q "snapped newspaperRead:newspaperReading plays o2a-newspaper-read" "$log" ||
    { echo "FAIL: the open paper's own animation did not play"; fail=1; }
[ $fail = 0 ] && echo "newspaper read: ok"
exit $fail
