#!/usr/bin/env bash
# From your own dump to a native build, every stage in order (README, "Setup"). Each stage is skipped when its
# output is already there, so the script can be run again after a failure or an update of the repository.
#
#   usage: tools/setup_acnh.sh --keys <prod.keys> [--title-keys <title.keys>] --base <base.nsp> --update <update.nsp>
#                              [--dlc <add-on.nsp>]...   (add-on content: imported to private/acnh/aoc/<index>/)
#                              [--mod <name>=<zip>]...   (a RomFS mod, e.g. --mod ptbr=acnh_ptbr_beta4.zip: the pt-BR
#                                                         fan translation; private/acnh/mods/<name>/romfs/)
#                              [--out <folder>]          (the game's folder: import, generated code, build; default
#                                                         private/acnh. The launcher passes its install folder)
#          tools/setup_acnh.sh                       (later runs: the import is done, the rest is checked)
#   env:   JOBS=<compile jobs> (default: free RAM / 700 MB, at most the core count)   FORCE=<stage> to redo one stage
#
# Stages: tools (venv, Vulkan headers, .NET, Ryujinx source, hactool-free importer) -> import -> recompile ->
# shaders -> fonts -> build. Everything it writes stays in private/ (gitignored) and the --out folder. The lines
# starting with "== " name the stage that begins: the launcher (launcher/) follows them.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
P=$ROOT/private
T=$P/third_party
G=$P/acnh

KEYS="" TKEYS="" BASE="" UPD="" DLC=() MODS=() OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --keys) KEYS=$2; shift 2 ;;
    --title-keys) TKEYS=$2; shift 2 ;;
    --base) BASE=$2; shift 2 ;;
    --update) UPD=$2; shift 2 ;;
    --dlc) DLC+=("$2"); shift 2 ;;
    --mod) MODS+=("$2"); shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    *) echo "unknown argument $1"; exit 1 ;;
  esac
done
[ -n "$OUT" ] && { mkdir -p "$OUT"; G=$(cd "$OUT" && pwd); }
mkdir -p "$P" "$T" "$G/logs"

say() { printf '\n== %s\n' "$*"; }
need() { command -v "$1" >/dev/null || { echo "missing: $1 (see README, Requirements)"; exit 1; }; }
force() { [ "${FORCE:-}" = "$1" ]; }
need clang; need clang++; need curl; need tar; need python3

# Memory for compile jobs: what is free now (Linux) or half the RAM (macOS), less 1 GB for the rest of the system.
# A compiler job on the generated code peaks at about 600 MB (900 MB for the largest file), the link at 500 MB, so
# 4 GB machines run 3 jobs and 6 GB machines 5.
if [ "$(uname)" = Darwin ]; then
  NPROC=$(sysctl -n hw.ncpu); FREE_MB=$(( $(sysctl -n hw.memsize) / 2097152 ))
else
  NPROC=$(nproc); FREE_MB=$(( $(awk '/MemAvailable/ {print $2}' /proc/meminfo) / 1024 ))
  # Inside a memory-limited cgroup (a container, systemd-run -p MemoryMax=...), its limit counts.
  CG=/sys/fs/cgroup$(awk -F: '$1 == "0" {print $3}' /proc/self/cgroup 2>/dev/null)
  if [ -r "$CG/memory.max" ] && [ "$(cat "$CG/memory.max")" != max ]; then
    CG_MB=$(( ($(cat "$CG/memory.max") - $(cat "$CG/memory.current")) / 1048576 ))
    [ "$CG_MB" -ge "$FREE_MB" ] || FREE_MB=$CG_MB
  fi
fi
JOBS=${JOBS:-$(( (FREE_MB - 1000) / 700 ))}
[ "$JOBS" -ge 1 ] || JOBS=1
[ "$JOBS" -le "$NPROC" ] || JOBS=$NPROC

# ---- tools ----------------------------------------------------------------------------------------------------
say "tools"
if [ ! -x "$P/venv/bin/python" ]; then
  python3 -m venv "$P/venv" 2>/dev/null || {
    python3 -m venv --without-pip "$P/venv"
    curl -sL https://bootstrap.pypa.io/get-pip.py -o "$P/get-pip.py" && "$P/venv/bin/python" "$P/get-pip.py" -q
  }
fi
PY=$P/venv/bin/python
"$PY" -c "import capstone, lz4, zstandard, fontTools, cmake, ninja" 2>/dev/null ||
  "$PY" -m pip install -q capstone lz4 zstandard fonttools cmake ninja unicorn
CMAKE=$P/venv/bin/cmake NINJA=$P/venv/bin/ninja
VKH=$T/Vulkan-Headers-1.4.357
[ -d "$VKH" ] || curl -sL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.357.tar.gz | tar -xz -C "$T"
if [ ! -x "$T/dotnet/dotnet" ]; then
  mkdir -p "$T/dotnet"
  case "$(uname)-$(uname -m)" in
    Linux-x86_64) DN=linux-x64 ;; Linux-aarch64) DN=linux-arm64 ;; Darwin-arm64) DN=osx-arm64 ;; Darwin-x86_64) DN=osx-x64 ;;
  esac
  curl -sL "https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.401/dotnet-sdk-10.0.401-$DN.tar.gz" | tar -xz -C "$T/dotnet"
fi
export DOTNET_ROOT=$T/dotnet PATH=$T/dotnet:$PATH DOTNET_CLI_TELEMETRY_OPTOUT=1
if [ ! -f "$T/ryubing/Ryujinx.sln" ]; then
  mkdir -p "$T/ryubing"
  curl -sL https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz | tar -xz -C "$T/ryubing" --strip-components=1
fi
if [ ! -f "$T/imgui/imgui.cpp" ]; then  # the settings screen (Dear ImGui, MIT)
  mkdir -p "$T/imgui"
  curl -sL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz | tar -xz -C "$T/imgui" --strip-components=1
fi
HARNESS=$P/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll
if [ ! -f "$HARNESS" ] || force harness; then
  dotnet build tools/maxwell/spirv_harness/SpirvHarness.csproj -c Release -m:4 > "$G/logs/harness.log" 2>&1 ||
    { tail -20 "$G/logs/harness.log"; exit 1; }
fi
[ -f "$P/build/importer/build.ninja" ] ||
  CXX=clang++ "$CMAKE" -S tools/importer -B "$P/build/importer" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" >/dev/null
"$NINJA" -C "$P/build/importer" nximport >/dev/null

# ---- import ---------------------------------------------------------------------------------------------------
say "import (ExeFS + RomFS with the update applied, add-on content)"
# With --dlc the import always runs: files already imported are checked by size and skipped (seconds).
NEED_AOC=""
[ ${#DLC[@]} -gt 0 ] && NEED_AOC=1
if [ ! -f "$G/analysis/main.bin" ] || [ ! -d "$G/extract/romfs/Shader" ] || [ -n "$NEED_AOC" ] || force import; then
  [ -n "$KEYS" ] && [ -n "$BASE" ] && [ -n "$UPD" ] || { echo "first run: pass --keys, --base and --update"; exit 1; }
  # Some key dumps carry entries with odd lengths that the importer does not need: keep only well-formed ones.
  mkdir -p "$P/keys"
  "$PY" -I -c '
import re, sys
out = []
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r"\s*(\w+)\s*=\s*([0-9a-fA-F]+)\s*$", l)
    if m and len(m.group(2)) % 32 == 0:
        out.append(f"{m.group(1)} = {m.group(2)}\n")
open(sys.argv[2], "w").writelines(out)' "$KEYS" "$P/keys/prod.keys"
  ARGS=("$P/keys/prod.keys")
  [ -n "$TKEYS" ] && ARGS+=("$TKEYS")
  "$P/build/importer/nximport" --exefs --title 01006F8002326000 --out "$G" "${ARGS[@]}" "$BASE" "$UPD" ${DLC[@]+"${DLC[@]}"} \
    > "$G/logs/import.log" 2>&1 || { tail -5 "$G/logs/import.log"; exit 1; }
  tail -2 "$G/logs/import.log"
  cp "$G/analysis/main.npdm" "$G/exefs/" 2>/dev/null || true
fi
"$PY" -I -c '
import struct, sys
d = open(sys.argv[1], "rb").read()
print("main.bin", len(d), "bytes")' "$G/analysis/main.bin"

# ---- mods: a zip with romfs/ at its root (or one folder holding it), extracted to private/acnh/mods/<name>/ -------
for m in ${MODS[@]+"${MODS[@]}"}; do
  name=${m%%=*}; zip=${m#*=}
  [ "$name" != "$m" ] && [ -f "$zip" ] || { echo "--mod wants <name>=<zip>"; exit 1; }
  say "mod $name ($zip)"
  "$PY" -I - "$zip" "$G/mods/$name" <<'PYEOF'
import sys, zipfile, os, shutil
src, dst = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(src)
names = [i.filename for i in z.infolist()]
for n in names:
    if n.startswith('/') or '..' in n.split('/'): sys.exit(f'unsafe path in the zip: {n}')
prefix = ''
if not any(n.startswith('romfs/') for n in names):  # one wrapping folder
    tops = {n.split('/')[0] for n in names if '/romfs/' in n}
    if len(tops) != 1: sys.exit('no romfs/ folder in the zip')
    prefix = tops.pop() + '/'
shutil.rmtree(dst, ignore_errors=True)
for i in z.infolist():
    if i.is_dir() or not i.filename.startswith(prefix + 'romfs/'): continue
    out = os.path.join(dst, i.filename[len(prefix):])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with z.open(i) as a, open(out, 'wb') as b: shutil.copyfileobj(a, b)
print(f'{sum(1 for _ in os.scandir(dst))} entries in {dst}')
PYEOF
done

# ---- recompile ------------------------------------------------------------------------------------------------
say "recompile (main, sdk, subsdk0)"
# The update the dump was made with (main's build id): 3.0.3 moved most of the code by 0x10 (docs/ACNH.md, "Updates"),
# so its hooked instructions are listed apart.
MAIN_ID=$("$PY" -I -c 'import sys; print(open(sys.argv[1], "rb").read(0x50)[0x40:0x50].hex().upper())' "$G/exefs/main")
case "$MAIN_ID" in
  FCD2BB238ABE99E925B4E452DF1F41F1) HOOKS=games/acnh/hooks.txt; echo "main 3.0.2" ;;
  FF1D1C05670DB6021C85B624A710B963) HOOKS=games/acnh/hooks_303.txt; echo "main 3.0.3" ;;
  *) echo "main is neither 3.0.2 nor 3.0.3 (build id $MAIN_ID): import the 3.0.2 or 3.0.3 update"; exit 1 ;;
esac
recomp() {  # module base
  local m=$1 b=$2 H=()
  [ "$m" = main ] && H=(--hooks "$HOOKS")  # instructions that call the runtime first (games/acnh/game.cpp)
  # The native 60 FPS variant, kept once tools/setup_60fps.sh installed it.
  [ "$m" = main ] && [ -f "$G/speedfix_main.pchtxt" ] && H+=(--variant "$G/speedfix_main.pchtxt")
  if [ ! -f "$G/gen/$m/table.cpp" ] || [ "recomp/emit.py" -nt "$G/gen/$m/table.cpp" ] || force recomp ||
     { [ "$m" = main ] && [ "$HOOKS" -nt "$G/gen/$m/table.cpp" ]; }; then
    "$PY" -m recomp "$G/exefs/$m" "$G/gen/$m" --name "$m" --base "$b" --jobs "$JOBS" ${H[@]+"${H[@]}"} > "$G/logs/recomp_$m.log" 2>&1
  fi
  tail -1 "$G/logs/recomp_$m.log"
}
recomp sdk 0x4010000000
recomp subsdk0 0x4018000000
recomp main 0x4000000000

# ---- shaders --------------------------------------------------------------------------------------------------
say "shaders (Maxwell -> SPIR-V)"
if [ ! -f "$G/spirv/summary.txt" ] || force shaders; then
  JOBS=$NPROC tools/extract_shaders.sh "$G"
  VAL=(--no-val); command -v spirv-val >/dev/null && VAL=(--spirv-val "$(command -v spirv-val)")
  dotnet "$HARNESS" --batch "$G/shaders_extracted" --out "$G/spirv" --jobs "$NPROC" "${VAL[@]}" > "$G/logs/spirv.log" 2>&1
fi
head -1 "$G/spirv/summary.txt"

# ---- fonts ----------------------------------------------------------------------------------------------------
say "fonts (button icons)"
mkdir -p "$G/fonts"
# Stand-in for the Switch's button-icon font (type 5), drawn over a host font. The other shared fonts fall back
# to host fonts (runtime/src/hle_pl.cpp); ACNH ships its own fonts in its RomFS for almost all text.
if [ ! -f "$G/fonts/shared_5.ttf" ]; then
  for f in /System/Library/Fonts/Supplemental/Arial\ Unicode.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
           /usr/share/fonts/TTF/DejaVuSans.ttf /usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf; do
    [ -f "$f" ] && { "$PY" tools/fonts/make_button_font.py --base "$f" --out "$G/fonts/shared_5.ttf" >/dev/null; break; }
  done
fi
# ACNH draws its button prompts with its own ParkExt font (Font/ScalableFont.sarc.zs), not with type 5: a copy of
# that archive per controller style (Xbox, PlayStation) and face-button layout, chosen at boot by controls.glyphs
# (runtime/src/glyphs.cpp). About 13 MB each.
if [ ! -f "$G/fonts/glyphs_xbox/nintendo/romfs/Font/ScalableFont.sarc.zs" ] || force glyphs; then
  "$PY" tools/make_button_glyphs.py --romfs "$G/extract/romfs" --out "$G/fonts" > "$G/logs/glyphs.log"
fi

# ---- build ----------------------------------------------------------------------------------------------------
say "build (-j$JOBS; the first build compiles ~5 CPU-hours of code, about $(( 300 / JOBS + 10 )) minutes here)"
EXTRA=()
[ -n "$OUT" ] && EXTRA+=(-DNX_GEN_DIR="$G/gen")  # the default build reads private/acnh/gen
[ "$(uname)" = Darwin ] && EXTRA+=(-DCMAKE_PREFIX_PATH="$(brew --prefix 2>/dev/null || echo /opt/homebrew)")
CC=clang CXX=clang++ "$CMAKE" -S . -B "$G/build" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -DNX_GAME=acnh \
  -DCMAKE_BUILD_TYPE=Release -DNX_VULKAN_INCLUDE="$VKH/include" "${EXTRA[@]}" > "$G/logs/cmake.log"
"$NINJA" -C "$G/build" -j"$JOBS" acnh
if [ -n "$OUT" ]; then
  say "done: $G/build/acnh   (NX_DATA_ROOT=$G)"
else
  say "done: tools/acnh_linux.sh   (macOS: tools/acnh_mac.sh)"
fi
