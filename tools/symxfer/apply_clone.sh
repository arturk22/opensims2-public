#!/bin/sh
# Put the current symxfer names into your own copy of the Ghidra project.
#   tools/symxfer/apply_clone.sh <dir> [map.txt]
# Makes <dir>/sims2.{gpr,rep} as an APFS clone of ~/sims2 (cheap, nothing
# shared is touched), regenerates the map with run.sh when none is given,
# and applies it with "reset" (names an older map gave and the new one drops
# go back to FUN_...).  Then use <dir> instead of ~ in analyzeHeadless:
#   analyzeHeadless <dir> sims2/mac -process sims2_bv.ppc -readOnly -noanalysis \
#     -scriptPath tools/symxfer -postScript DecompileList.java out.c <name|0xaddr>...
# One headless Ghidra at a time; GHIDRA_HEADLESS_MAXMEM defaults to 2G.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:?usage: apply_clone.sh <dir> [map.txt]}
MAP=$2
mkdir -p "$DIR"
[ -e "$DIR/sims2.rep" ] || { cp -c -R ~/sims2.rep "$DIR/sims2.rep"; cp -c ~/sims2.gpr "$DIR/sims2.gpr"; }
if [ -z "$MAP" ]; then
  MAP="$DIR/symxfer_map.txt"
  sh "$HERE/run.sh" "$MAP"
fi
H=${GHIDRA_HEADLESS:-$(ls /opt/homebrew/Cellar/ghidra/*/libexec/support/analyzeHeadless | tail -1)}
GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-2G} "$H" "$DIR" sims2/mac -process sims2_bv.ppc -noanalysis \
  -scriptPath "$HERE" -postScript ApplySymxferMap.java "$(cd "$(dirname "$MAP")" && pwd)/$(basename "$MAP")" reset \
  2>&1 | grep "symxfer:"
