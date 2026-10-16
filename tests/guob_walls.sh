#!/bin/sh
# The objects' ground and wall shadow planes (GUOB) as nTSSG::cShadow turns
# them on (round n40, docs/lighting-mac.md 12), headless on N001_Lot11's
# living room piano (object 184, its back against the wall along y = 13):
# with the walls up (mode 2) its southwallshadow is on; cut away (mode 1,
# the lot's saved mode) it is off (the base game shows the side planes with
# the walls up only); dragged a tile from the wall in Buy mode, off; with
# the wall behind it deleted in Build mode, off; OS2_GUOB_AS_SAVED=1 keeps
# the lot's saved planes (all on) and works nothing out.
# usage: guob_walls.sh <opensims2 executable> <scratch dir>
app=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "guob walls: ok (skipped: no OS2_DATA)"; exit 0; fi
mkdir -p "$out"
fail=0
run() { # <name> <args...>
    name=$1; shift
    OS2_DEBUG_GUOB=1 "$app" --data "$OS2_DATA" --lot N001_Lot11 --vm --ui live --run-vm 20 --key pageup "$@" \
        --frames 1 --screenshot "$out/$name.png" -ApplePersistenceIgnoreState YES > "$out/$name.log" 2>&1
}
piano() { grep "guob: object 184 (pianoUprightSaloon)" "$out/$1.log" | tail -1; }
run up --key walls-2
piano up | grep -q "level 1 dir 2 mode 2 groundshadow+ southwallshadow+ eastwallshadow- westwallshadow-" ||
    { echo "FAIL walls up: $(piano up)"; fail=1; }
grep -q "guob: object 118 (fridgeModerateStainless) .*mode 2 groundshadow+ southwallshadow+" "$out/up.log" ||
    { echo "FAIL walls up, the fridge: $(grep 'object 118 ' "$out/up.log")"; fail=1; }
grep -q "guob: object 1184 (windowCraftsman) .*mode 2 wallshadow+" "$out/up.log" ||
    { echo "FAIL walls up, a window's wallshadow: $(grep 'object 1184 ' "$out/up.log")"; fail=1; }
run cutaway
piano cutaway | grep -q "mode 1 groundshadow+ southwallshadow- " || { echo "FAIL cutaway: $(piano cutaway)"; fail=1; }
run down --key walls-0
grep -q "guob: object 1184 (windowCraftsman) .*mode 0 wallshadow-" "$out/down.log" ||
    { echo "FAIL walls down, a window's wallshadow: $(grep 'object 1184 ' "$out/down.log")"; fail=1; }
run moved --mode buy --key walls-2 --grab-drag 184,18,15
grep -q "buy mode: moved object 184" "$out/moved.log" || { echo "FAIL: the piano was not moved"; fail=1; }
piano moved | grep -q "mode 2 groundshadow+ southwallshadow- " || { echo "FAIL moved from the wall: $(piano moved)"; fail=1; }
run nowall --mode build --key walls-2 --delete-wall 18,13,20,13
grep -q "delete-wall 18,13,20,13: 2 wall sections deleted" "$out/nowall.log" || { echo "FAIL: the wall was not deleted"; fail=1; }
piano nowall | grep -q "mode 2 groundshadow+ southwallshadow- " || { echo "FAIL the wall deleted: $(piano nowall)"; fail=1; }
OS2_GUOB_AS_SAVED=1 run saved --key walls-2
! grep -q "guob:" "$out/saved.log" || { echo "FAIL: OS2_GUOB_AS_SAVED still works planes out"; fail=1; }
[ $fail = 0 ] && echo "guob walls: ok"
exit $fail
