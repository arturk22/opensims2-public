#!/bin/sh
# TestPlacementCommon's slope and floor tests with the lot's geometry
# (round n20, docs/buy-build-mac.md §7.24): a chair on a sloped tile of
# N001_Lot59 does not fit (IsFlat, 0x12) and fits on a flat one; every
# object the lots keep, tested where it stands, fits as before but for the
# 6 oleander shrubs on N001_Lot59's slopes (the game's IsFlat refuses them
# too; inferred: their terrain was sculpted after them), and no staircase meets
# CeilingTooHighForObject (0x30).
# usage: placement_slope.sh <s2pkg executable>
s2pkg=$1
if [ -z "$OS2_DATA" ]; then echo "placement slope: ok (skipped: no OS2_DATA)"; exit 0; fi
lots="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001/Lots"
fail=0
out=$("$s2pkg" buy "$lots/N001_Lot59.package" --check "0x8C3FE510@23,7,0" --check "0x8C3FE510@23,6,0" 2>/dev/null)
echo "$out" | grep -q "at (23, 7) level 1 facing 0: does not fit: on a slope" || { echo "FAIL: a chair on a slope: $out"; fail=1; }
echo "$out" | grep -q "at (23, 6) level 1 facing 0: fits" || { echo "FAIL: a chair on a flat tile: $out"; fail=1; }
sweep=$("$s2pkg" placement-sweep "$lots/N001_Lot59.package" "$lots/N001_Lot11.package" 2>/dev/null)
n=$(echo "$sweep" | grep -c "refused 0x12")
[ "$n" = 6 ] || { echo "FAIL: $n objects refused on slopes (want the 6 oleanders): $sweep"; fail=1; }
echo "$sweep" | grep "refused" | grep -v "Shrub - Oleander" && { echo "FAIL: another object refused"; fail=1; }
[ $fail = 0 ] && echo "placement slope: ok"
exit $fail
