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
if grep -q "AddressSanitizer" "$out/run.log"; then echo "FAIL: AddressSanitizer report"; grep -A8 "ERROR: AddressSanitizer" "$out/run.log"; fail=1; fi
[ $fail = 0 ] && echo "scene cycle: ok"
rm -rf "$out/user"
exit $fail
