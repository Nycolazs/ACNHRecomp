#!/usr/bin/env bash
# Extracts every Maxwell shader container of Animal Crossing: New Horizons' RomFS (3.0.2), plus the shader blocks
# embedded in `main` (docs/SHADERS-ACNH.md). Run it from anywhere.
#   usage: tools/extract_shaders.sh [game dir]            (default: private/acnh)
#   env:   PY=<python with zstandard>   (default: private/venv/bin/python, else python3)
#          JOBS=<parallel extractions>  (default: half the cores, at least 1)
#          FULL=1                       also extract every Model/ container whole (61k files; the blocks only
#                                       models have are extracted anyway, by model_shaders.py)
# Output: <game dir>/shaders_extracted/<group>/..., one group per container.
set -u
cd "$(dirname "$0")/.." || exit 1
W=${1:-private/acnh}
PY=${PY:-}
if [ -z "$PY" ]; then if [ -x private/venv/bin/python ]; then PY=private/venv/bin/python; else PY=python3; fi; fi
JOBS=${JOBS:-$(( $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) / 2 ))}
[ "$JOBS" -ge 1 ] || JOBS=1
X=tools/maxwell/containers.py
R=$W/extract/romfs
O=$W/shaders_extracted
[ -d "$R" ] || { echo "no $R: import the game first (README, step 2)"; exit 1; }
mkdir -p "$O"
# One line per group: "<group> <container>". Group names come from the file name without the platform suffix.
grp() { local f=$1 p=$2 b; b=$(basename "$f"); b=${b%%.*}; echo "${p}_$b $f"; }
{
  for f in "$R"/Shader/*; do grp "$f" shader; done            # Park_UBER_All (bfsha), Park_Proc (agl BAHS), apkg
  for f in "$R"/Effect/*; do grp "$f" effect; done            # VFXB with GRSR/GRSC BNSH
  # System/: GSys and Agl resources (BAHS), the font and primitive renderer shaders (bare BNSH). The font shaders
  # and the primitive drawer hold the vertex shader of the final full-screen pass: without them nothing reaches
  # the window.
  find "$R"/System -type f | while read -r f; do grp "$f" "system_$(basename "$(dirname "$f")")"; done
  for f in "$R"/Swkbd/shader/*_Nx.arc.szs; do grp "$f" swkbd; done
  for f in "$R"/Layout/*; do grp "$f" layout; done            # 960 Ui2d archives, ~200 with bgsh
  if [ "${FULL:-0}" = 1 ]; then for f in "$R"/Model/*; do grp "$f" model; done; fi
} | xargs -P "$JOBS" -L 1 sh -c \
    '"$1" "$2" "$5" --extract "$3/$4" --unique --allow-external > "$3/$4.log" 2>&1 || echo "FAIL $4"' sh "$PY" "$X" "$O"
# Containers without shaders leave an empty folder (or none): drop them and their logs.
for d in "$O"/*/; do
  [ -n "$(ls -A "$d" 2>/dev/null | grep -v manifest.tsv)" ] || { rm -rf "$d"; rm -f "${d%/}.log"; }
done
for l in "$O"/*.log; do [ -d "${l%.log}" ] || rm -f "$l"; done
# The code blocks embedded in `main` that no container holds. After the others: it skips what they already have.
"$PY" tools/maxwell/exefs_shaders.py "$W/analysis/main.bin" --extract "$O/exefs" --allow-external > "$O/exefs.log" 2>&1 || echo "FAIL exefs"
# The variations only models carry (their own Park_UBER.bfsha): without them some materials draw depth only, black
# (character faces, the Nook Stop). Last: it keeps only the blocks no other group has. ~15 min for 60k archives.
"$PY" tools/maxwell/model_shaders.py "$R/Model" "$O" --jobs "$JOBS" --allow-external > "$O/model.log" 2>&1 || echo "FAIL model"
echo "groups: $(ls -d "$O"/*/ | wc -l)  blocks: $(find "$O" -name '*.code.bin' -o -name 'code_*.bin' | wc -l)"
