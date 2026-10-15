#!/bin/sh
# The Families bin pages and picks a family while the Lot Info Dialog is open
# (playtest hood-slow: the arrows did nothing then). Headless, with the game's
# data (OS2_DATA): N001 copied to a scratch user folder, Caixão moved out of
# lot 11 so the bin holds three families (two pages); lot 11's dialog opened,
# the "Avançar" arrow pages to the third family, picking it closes the dialog,
# and a click on the empty lot 80 asks to move it in. The steps run as the
# window's events do (the cursor laid out after each one, a frame between):
# the frame after the lot click must draw the dialog (playtest hood-esc: the
# cursor's layout left the hover popup drawn, the dialog open but unseen and
# every click swallowed until Esc). A second run clicks only lot 11.
# usage: hood_family_bin.sh <opensims2 executable> <s2pkg executable> <scratch dir>
app=$1; s2pkg=$2; out=$3
if [ -z "$OS2_DATA" ]; then echo "hood family bin: ok (skipped: no OS2_DATA)"; exit 0; fi
rm -rf "$out"; mkdir -p "$out/user/Neighborhoods"
cp -Rc "$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001" "$out/user/Neighborhoods/N001" 2>/dev/null ||
    cp -R "$OS2_DATA/TSData/Res/UserData/Neighborhoods/N001" "$out/user/Neighborhoods/N001"
chmod -R u+w "$out/user"
OS2_DATA="$OS2_DATA" "$s2pkg" hood-move-out "$out/user/Neighborhoods/N001" 11 > "$out/moveout.log" 2>&1
fail=0
# Default camera: lot 11 at (736, 562), lot 80 at (1220, 129); the bin's Next
# arrow at (565, 645) and its first item at (425, 640).
"$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio --hood N001 --hood-panel families \
    --click 736,562 --click 565,645 --click 425,640 --click 1220,129 --screenshot "$out/bin.png" \
    -ApplePersistenceIgnoreState YES > "$out/bin.log" 2>&1
grep -q 'lot 11 "' "$out/bin.log" || { echo "FAIL: lot 11 not clicked"; fail=1; }
grep -q "family bin: page 2 of 2" "$out/bin.log" || { echo "FAIL: the bin's arrow did not page (dialog open)"; fail=1; }
grep -q "family bin: family [0-9]* selected" "$out/bin.log" || { echo "FAIL: no family picked from the bin"; fail=1; }
grep -q "dialog Accept at" "$out/bin.log" || { echo "FAIL: no move-in question on lot 80"; fail=1; }
grep -q "STALE" "$out/bin.log" && { echo "FAIL: a frame did not draw the UI's state"; grep "hood frame" "$out/bin.log"; fail=1; }
"$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio --hood N001 \
    --hover 700,500 --click 736,562 --screenshot "$out/dialog.png" -ApplePersistenceIgnoreState YES > "$out/dialog.log" 2>&1
grep -q "hood frame 2: lot dialog 11$" "$out/dialog.log" ||
    { echo "FAIL: the lot dialog opened by a click is not drawn"; grep "hood frame" "$out/dialog.log"; fail=1; }
[ $fail = 0 ] && echo "hood family bin: ok"
[ $fail = 0 ] || cat "$out/moveout.log"
rm -rf "$out/user"
exit $fail
