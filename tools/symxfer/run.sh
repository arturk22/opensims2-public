#!/bin/sh
# Regenerate the symbol map for the Mac Bon Voyage 1.10 ppc slice and
# (optionally) import it into the Ghidra project ~/sims2 as /mac/sims2_bv.ppc.
#   tools/symxfer/run.sh [out_map] [--ghidra]
# Inputs live outside the repo (copyrighted binaries, third-party symbols):
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REFS=${REFS:-"$HERE/../../../references"}
BIN="$REFS/bv_mac/sims2_bv.ppc"          # lipo -thin ppc of "The Sims 2.app"
NM="$REFS/bv_mac/ppc_syms.txt"            # nm -n: "addr name"
SYM="$REFS/sims2_mac_decomp/baserom.sym"  # nm of the other ppc build
OUT=${1:-map.txt}
python3 "$HERE/symxfer.py" "$BIN" "$SYM" "$NM" --manual "$HERE/manual.txt" -o "$OUT"
grep -E "^#|9cTSObject|9cTSPerson|11cAnimatable|12cLocomotable|7cObject|7cPerson|Param|14cEdithLanguage|cTSTreeSim|TreeStack|cTSBehavior|cTSRoutingObject|cTSInventory|cTSRelationship" "$OUT" \
  | grep -v "^[0-9a-f]* __ZN9__gnu_cxx\|__ZNSt\|__ZSt" > "$HERE/map_simantics.txt"
if [ "$2" = "--ghidra" ]; then
  H=${GHIDRA_HEADLESS:-$(ls /opt/homebrew/Cellar/ghidra/*/libexec/support/analyzeHeadless | tail -1)}
  "$H" ~ sims2/mac -import "$BIN" -overwrite -noanalysis -scriptPath "$HERE" \
    -postScript ApplySymxferMap.java "$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
fi
