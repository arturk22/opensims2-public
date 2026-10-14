#!/bin/sh
# Lot A -> neighborhood -> lot B -> neighborhood -> lot A in one session with
# the game's data (OS2_DATA), headless, with the 256 open files an app opened
# from the Finder gets. It must exit with 0. The crashes it guards against
# (round 22):
# - the Sim parts' parallel read ran out of files and threw in its workers
#   (std::terminate) on the first lot;
# - the lot's particle effects outlived their scene's renderer and crashed on
#   exit (~LotEffects) or on the next lot;
# - the open mood popup's windows were read after their copy went
#   (UiElement::attr in LiveUi::screen).
# - per-scene state that outlived its scene (DEBITOS-TECNICOS.md 1.3/1.4):
#   with OS2_DEBUG_SCENE_STATE every unload must leave gScene empty, and with
#   OS2_PROBE_SCENE_CACHES each lot's vanity edit and filled wardrobe must
#   start from nothing (the mirror edits by Sim ID and the wardrobes filled
#   by family number of round 20 kept the last lot's: on N002_Lot10's second
#   visit its family's bin stayed unfilled).
# - the app's caches of the game's data (app/game_data_caches.hpp, round 24):
#   on quit they are freed after the scene and before the statics go (with
#   OS2_DEBUG_SCENE_STATE the quit lines show them full, then empty), and
#   with OS2_PROBE_APP_CACHES (every wardrobe and vanity group of every lot
#   Sim) the decoded pictures stay across a scene switch under the cap
#   (OS2_PICTURE_CAP_MB=120: N002_Lot10's ~97 MB) and are dropped at the
#   unload past it (with N002_Lot3's ~70 MB more).
# Run it with an AddressSanitizer build of the app too (cmake -B build-asan
# -DCMAKE_CXX_FLAGS=-fsanitize=address -DCMAKE_OBJCXX_FLAGS=-fsanitize=address
# -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address): any report fails it.
# usage: scene_cycle.sh <opensims2 executable> <scratch dir>
app=$1; out=$2
if [ -z "$OS2_DATA" ]; then echo "scene cycle: ok (skipped: no OS2_DATA)"; exit 0; fi
rm -rf "$out"; mkdir -p "$out/user"
leave="--click 139,690 --click 236,628 --click 528,398 --wait 2" # Options, Neighborhood, Yes (save)
(
    ulimit -S -n 256 # the soft limit only, as launchd sets it
    OS2_DEBUG_SCENE_STATE=1 OS2_PROBE_SCENE_CACHES=1 OS2_PROBE_APP_CACHES=1 OS2_PICTURE_CAP_MB=120 \
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 "$app" --data "$OS2_DATA" --user-dir "$out/user" --no-audio --hood N002 \
        --hood-lot-click 10 --click 595,592 --wait 30 --click 409,640 --wait 30 $leave \
        --click 807,104 --click 595,592 --wait 60 $leave \
        --click 870,427 --click 595,592 --wait 60 \
        --screenshot "$out/lot10.png" -ApplePersistenceIgnoreState YES > "$out/run.log" 2>&1
)
code=$?
fail=0
if [ $code -ne 0 ]; then echo "FAIL: exit code $code"; tail -5 "$out/run.log"; fail=1; fi
switches=$(grep -c "scene switch" "$out/run.log")
if [ "$switches" -lt 5 ]; then echo "FAIL: $switches scene switches (lot, hood, lot, hood, lot expected)"; fail=1; fi
if [ "$(grep -c 'entering lot 10\|hood-lot 10 ' "$out/run.log")" -lt 2 ]; then echo "FAIL: N002_Lot10 not entered twice"; fail=1; fi
if ! grep -q "N002_Lot3" "$out/run.log"; then echo "FAIL: N002_Lot3 not loaded"; fail=1; fi
after=$(grep -c "scene state after unload" "$out/run.log")
if [ "$after" -lt 5 ]; then echo "FAIL: $after scene state reports after an unload (5 expected)"; fail=1; fi
if grep "scene state after unload" "$out/run.log" | grep -q '=[1-9]'; then
    echo "FAIL: scene state not empty after an unload"; grep "scene state after unload" "$out/run.log" | grep '=[1-9]' | head -3; fail=1
fi
probes=$(grep -c "scene caches probe: .*mirrorEdits 0 -> 1, wardrobeFilled 0 -> 1" "$out/run.log")
if [ "$probes" -lt 3 ]; then
    echo "FAIL: $probes of 3 lots started with empty mirror edits and wardrobes filled"; grep "scene caches probe" "$out/run.log"; fail=1
fi
# The pictures: kept across a switch under the cap, dropped (all) past it.
kept=$(grep "app caches after unload" "$out/run.log" | grep -v "pictures dropped" | grep -c "imagesKB=[1-9]")
dropped=$(grep "app caches after unload" "$out/run.log" | grep "pictures dropped" | grep -c "imagesKB=0 ")
if [ "$kept" -lt 1 ]; then echo "FAIL: the app's pictures were not kept across a scene switch under the cap"; fail=1; fi
if [ "$dropped" -lt 1 ]; then echo "FAIL: the app's pictures were not dropped past the cap"; grep "app caches after unload" "$out/run.log" | head -5; fail=1; fi
over=$(grep "app caches after unload" "$out/run.log" | grep -v "pictures dropped" | sed -n 's/.*imagesKB=\([0-9]*\).*/\1/p' | awk '$1 > 120 * 1024' | wc -l)
if [ "$over" -gt 0 ]; then echo "FAIL: pictures past the cap kept"; fail=1; fi
reused=$(grep -c "app caches probe: .*pictures [1-9][0-9]* KB ->" "$out/run.log")
if [ "$reused" -lt 1 ]; then echo "FAIL: no lot started with the last scene's pictures"; grep "app caches probe" "$out/run.log" | cut -c1-120; fail=1; fi
# Quitting frees the app's caches after the scene (releaseAppState).
if ! grep "quit: app caches before release" "$out/run.log" | grep -q "casCatalog=1 .*simParts=[1-9]"; then
    echo "FAIL: no app caches to release on quit"; fail=1
fi
if ! grep -q "quit: app caches after release" "$out/run.log" || grep "quit: \(app caches\|scene state\) after release" "$out/run.log" | grep -q '=[1-9]'; then
    echo "FAIL: app caches or scene state not empty after the quit release"; grep "after release" "$out/run.log" | cut -c1-300; fail=1
fi
if grep -q "AddressSanitizer" "$out/run.log"; then echo "FAIL: AddressSanitizer report"; grep -A8 "ERROR: AddressSanitizer" "$out/run.log"; fail=1; fi
[ $fail = 0 ] && echo "scene cycle: ok"
rm -rf "$out/user"
exit $fail
