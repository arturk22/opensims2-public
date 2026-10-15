#!/bin/sh
# The front door, with the game's data (OS2_DATA), headless (round 22,
# "Ninguém atende a porta" and "Porta da frente do N001_Lot11 fica aberta").
# 1. N001_Lot68: Daniel invites Catarina Langerak by phone; she walks up from
#    the street, rings at the front door on the first try (the walk out of
#    her turn-and-go start passed the mailbox and failed the knock with
#    "blocked by object"), waits at the door (person data 34 "Greet Status"
#    0), Daniel greets her ("Cumprimentar", queued as the player would: the
#    person's greet entry has no motive ads, so nobody greets by himself) and
#    she comes into the house.
# 2. N001_Lot11: every door leaf the VM opens is drawn closed again.
# usage: front_door.sh <s2pkg> <opensims2 executable> <scratch dir>
s2pkg=$1; app=$2; out=$3
if [ -z "$OS2_DATA" ]; then echo "front door: ok (skipped: no OS2_DATA)"; exit 0; fi
mkdir -p "$out"
lot="$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot68.package"
fail=0
first="$out/invite.log"
S2PKG_SHOW_CREATED=1 S2PKG_WATCH=Kaylynn OS2_ROUTE_FAILS=1 "$s2pkg" pie "$lot" Daniel Phone --select 0 --ticks 1800 > "$first" 2>&1
id=$(grep -m1 'object [0-9]* "N001_User00072 - Kaylynn" made' "$first" | sed 's/.*object \([0-9]*\) .*/\1/')
[ -n "$id" ] || { echo "FAIL: the invited visitor did not come"; exit 1; }
grep -q "route of $id failed with [0-9]* in \"CT - Knock on Door\" #1" "$first" &&
    { echo "FAIL: the visitor could not walk up to the door"; grep "route of $id" "$first" | head -3; fail=1; }
grep -q "\"N001_User00072 - Kaylynn\" at .*, 2) .* greet 0$" "$first" ||
    { echo "FAIL: the visitor never waited at the door"; fail=1; }
log="$out/greet.log"
S2PKG_SHOW_CREATED=1 S2PKG_WATCH=Kaylynn S2PKG_PIE_ALSO="116,$id,Cumprimentar,1800" "$s2pkg" pie "$lot" Daniel Phone --select 0 --ticks 6500 > "$log" 2>/dev/null
grep -q "also queued \"Cumprimentar" "$log" || { echo "FAIL: no Cumprimentar for the visitor"; fail=1; }
grep -q "^  tick [0-9]*: $id \"N001_User00072 - Kaylynn\" at .* greet [1-9]" "$log" ||
    { echo "FAIL: the visitor was not greeted"; fail=1; }
grep "^  tick [0-9]*: $id \"N001_User00072 - Kaylynn\" at " "$log" | grep -qv " room 0 " ||
    { echo "FAIL: the greeted visitor never came in"; fail=1; }
# The doors of N001_Lot11 as drawn (OS2_DEBUG_DOORS=2): each opening closes.
doors="$out/doors.log"
OS2_DEBUG_DOORS=2 OS2_DEBUG_VM=1 "$app" --data "$OS2_DATA" --lot N001_Lot11 --run-vm 3000 --frames 2 --screenshot "$out/doors.png" \
    -ApplePersistenceIgnoreState YES > "$doors" 2>&1
# An opening in the run's last 60 ticks (a Sim passing a door as it ends)
# has had no time to close: left out of the count.
opens=$(awk '/^tick [0-9]+ / { t = $2 + 0; if (t > last) last = t } /starts o-door-open/ { n++; at[n] = t } END { c = 0; for (i = 1; i <= n; i++) if (at[i] < last - 60) c++; print c }' "$doors")
closes=$(grep -c "starts o-door-close" "$doors")
[ "$opens" -gt 0 ] || { echo "FAIL: no door opened"; fail=1; }
[ "$closes" -ge "$opens" ] || { echo "FAIL: $opens openings, $closes closings"; fail=1; }
open=$(awk '/^door [0-9]+ at/ { r[$2] = $6; l[$2] = $8 } END { for (d in r) if (r[d] + 0 > 5 || r[d] + 0 < -5 || l[d] + 0 > 5 || l[d] + 0 < -5) print d }' "$doors")
[ -z "$open" ] || { echo "FAIL: drawn open at the end: $open"; fail=1; }
[ $fail = 0 ] && echo "front door: ok ($opens doors opened and closed)"
exit $fail
