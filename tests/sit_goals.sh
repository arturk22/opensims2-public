#!/bin/sh
# Sitting goals (docs/seat-mac.md §A, ScanConeForSittingGoals): the TV's
# routing slot 0 weighs sitting 1000 over standing 1, so a Sim sent to it
# walks to the loveseat facing the TV and the post-route tree sits him in
# it (N001_Lot49: Darren, TV 110, loveseat part 458 or 459).
# usage: sit_goals.sh <s2pkg>
s2pkg=$1
if [ -z "$OS2_DATA" ]; then echo "sit goals: ok (skipped: no OS2_DATA)"; exit 0; fi
lot="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot49.package"
out=$("$s2pkg" route "$lot" 347 110 --slot 0 --ticks 900 2>&1 | tail -1)
echo "$out"
case "$out" in
    *"person data 62): 0,"*"in 458 slot"* | *"person data 62): 0,"*"in 459 slot"*) echo "sit goals: ok" ;;
    *) echo "sit goals: FAILED" ;;
esac
