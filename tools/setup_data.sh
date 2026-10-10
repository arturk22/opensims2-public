#!/bin/sh
# Rebuild data/ from the original CloneCD images.
# usage: tools/setup_data.sh <dir with Sims2_N.img>   (default: ../the-sims-2-2004)
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGES=${1:-"$ROOT/../the-sims-2-2004"}
DATA="$ROOT/data"

mkdir -p "$DATA/iso"
for img in "$IMAGES"/Sims2_*.img; do
    name=$(basename "$img" .img)
    n=${name#Sims2_}
    iso="$DATA/iso/$name.iso"
    [ -f "$iso" ] || python3 -I "$ROOT/tools/ccd2iso.py" "$img" "$iso"
    if [ ! -d "$DATA/disc$n" ]; then
        mp=$(hdiutil attach -readonly -nobrowse "$iso" | tail -1 | awk -F'\t' '{print $NF}')
        mkdir -p "$DATA/disc$n"
        cp -R "$mp"/ "$DATA/disc$n"/
        hdiutil detach -quiet "$mp"
    fi
done

[ -d "$DATA/game" ] || python3 -I "$ROOT/tools/extract_game.py" "$DATA" "$DATA/game"
echo "packages: $(find "$DATA/game" -name '*.package' | wc -l | tr -d ' ')"
