#!/bin/bash
# Headless check of the Sims' skin in Create-a-Sim: every age x gender x skin
# tone (--cas-age/--cas-gender/--cas-skin) is rendered offscreen, and any
# texture the app cannot find (a face skin falling back to white, as the
# teen girls' "tfface-s<n>" did: the game's tfface_s<n> material uses
# "afface-s<n>") fails the check. Screenshots go to OUT (default a temp dir).
#
# usage: tools/check_cas_skins.sh [OUT]
# env:   APP (build/opensims2.app/Contents/MacOS/opensims2), OS2_DATA as for the app
set -u
APP=${APP:-build/opensims2.app/Contents/MacOS/opensims2}
OUT=${1:-$(mktemp -d)}
mkdir -p "$OUT"
fail=0
for age in baby toddler child teen adult elder; do
  for g in f m; do
    for t in 1 2 3 4; do
      shot="$OUT/cas-$age-$g-s$t.png"
      log=$("$APP" --cas --cas-age "$age" --cas-gender "$g" --cas-skin "$t" --screenshot "$shot" 2>&1)
      missing=$(printf '%s\n' "$log" | grep -i "texture .*not found")
      if [ -n "$missing" ] || [ ! -s "$shot" ]; then
        echo "FAIL $age $g s$t: ${missing:-no screenshot}"
        fail=1
      fi
    done
  done
done
[ $fail = 0 ] && echo "cas skins: all ages, genders and tones found their textures ($OUT)"
exit $fail
