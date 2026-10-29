#!/bin/bash
# Native 60 FPS for ACNH 3.0.2 and 3.0.3: the game's logic at 60 updates a second with every per-update amount rescaled, so it
# plays at its normal speed (Settings > Video > Frame rate > 60).
#
# The rescaling is tmouh's "ACNH 60 FPS + SpeedFix" (https://github.com/tmouh/acnh-60fps, CC BY-NC 4.0): 6,417
# code and data words for main (3.0.2 or 3.0.3, one patch each), 1,437 values in romfs/Pack/StaticParam.pack and three villager tables, each
# measured against the 30 FPS game. It is not part of this repository: this script downloads it (a pinned commit)
# into private/, builds the parameter file from your own dump, and recompiles main with the patch as a second body
# of every function it changes (recomp --variant): one build plays at 30 or at 60, picked in the settings.
#
# usage: tools/setup_60fps.sh            (needs the game set up by tools/setup_acnh.py first)
cd "$(dirname "$0")/.." || exit 1
set -e
P=private
G=$P/acnh
PY=$P/venv/bin/python
WIN=
[ -x "$P/venv/Scripts/python.exe" ] && PY=$P/venv/Scripts/python.exe WIN=1  # Windows (Git Bash)
REPO_URL=https://github.com/tmouh/acnh-60fps
COMMIT=e5e03e64739334c9fbd36f19658b98e805f8ba8d
SRC=$P/third_party/acnh-60fps
MOD=$G/mods/speedfix60
say() { printf '\n== %s\n' "$*"; }

[ -f "$G/exefs/main" ] && [ -d "$G/extract/romfs" ] || { echo "run tools/setup_acnh.py first"; exit 1; }
id=$("$PY" -c "print(open('$G/exefs/main','rb').read(0x50)[0x40:0x50].hex().upper())")
case "$id" in  # tmouh's patch for this build of main, and this build's hooks (games/acnh/versions.txt)
  FCD2BB238ABE99E925B4E452DF1F41F1) PCH=speedfix_main_302.pchtxt HOOKS=games/acnh/hooks.txt ;;
  FF1D1C05670DB6021C85B624A710B963) PCH=speedfix_main.pchtxt HOOKS=games/acnh/hooks-3.0.3.txt ;;
  *) echo "main is not 3.0.2 or 3.0.3 (build id $id)"; exit 1 ;;
esac

say "download the SpeedFix ($REPO_URL @ ${COMMIT:0:7})"
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$P/third_party"
  git clone -q "$REPO_URL" "$SRC"
fi
git -C "$SRC" fetch -q origin "$COMMIT" 2>/dev/null || true
git -C "$SRC" checkout -q "$COMMIT"
EXEFS="$SRC/ryujinx/mods/contents/01006F8002326000/ACNH 60 FPS + SpeedFix"

say "parameter files (from your dump) -> $MOD/romfs"
rm -rf "$MOD"
mkdir -p "$MOD/romfs/Pack" "$MOD/romfs/Bcsv"
"$PY" -I "$SRC/tools/apply_ips.py" "$SRC/pack/StaticParam.pack.ips" "$G/extract/romfs/Pack/StaticParam.pack" \
  "$MOD/romfs/Pack/StaticParam.pack"
cp "$EXEFS"/romfs/Bcsv/*.bcsv "$MOD/romfs/Bcsv/"
cp "$EXEFS/exefs/$PCH" "$G/speedfix_main.pchtxt"

say "recompile main with the 60 FPS variant (~8 min)"
if [ -n "$WIN" ]; then  # Windows: setup_acnh.py passes the variant itself and builds in Visual Studio's environment
  FORCE=recomp "$PY" tools/setup_acnh.py
  echo
  echo "Done. Settings > Video > Frame rate > 60 (or video.frame_rate = 60 in $G/config.ini), then restart the game."
  exit 0
fi
"$PY" -m recomp "$G/exefs/main" "$G/gen/main" --name main --base 0x4000000000 --jobs 2 \
  --hooks "$HOOKS" --variant "$G/speedfix_main.pchtxt" > "$G/logs/recomp_main.log" 2>&1 ||
  { tail -20 "$G/logs/recomp_main.log"; exit 1; }
tail -3 "$G/logs/recomp_main.log"

say "build (only the changed files)"
until mkdir $P/build.lock 2>/dev/null; do echo "waiting for another build"; sleep 10; done
trap 'rmdir $P/build.lock 2>/dev/null' EXIT
ninja -C "$G/build" acnh

echo
echo "Done. Settings > Video > Frame rate > 60 (or video.frame_rate = 60 in $G/config.ini), then restart the game."
