#!/bin/bash
# Copies the game's data to the phone connected by adb: to the app's folder on the memory card when there is one
# (the runtime looks there, platform_android.cpp), else on the phone's own storage. The RomFS goes as one packed
# file (tools/pack_romfs.py) sent in parts of 1 GiB and appended on the phone, so an interrupted copy resumes where
# it stopped (adb exec-in streams stalled after a few GB) and the Mac needs only 1 GiB free for it.
#
# usage: tools/push_android.sh [--skip-romfs]
cd "$(dirname "$0")/.." || exit 1
set -e
GAME=acnh
PKG=io.github.acnhrecomp
SRC=private/$GAME
PART=1073741824
card=$(adb shell 'for d in /storage/*; do case $d in */emulated|*/self) ;; *) echo $d; break;; esac; done' | tr -d '\r')
D=${card:-/sdcard}/Android/data/$PKG/files
echo "== data folder on the phone: $D"
adb shell mkdir -p "$D"

# What the runtime reads (not shaders_extracted: an intermediate of the setup). A folder already there with as many
# files is skipped: thousands of small files take minutes each time.
for d in analysis spirv fonts exefs mods aoc; do
  [ -e "$SRC/$d" ] || continue
  local_n=$(find -L "$SRC/$d" -type f | wc -l | tr -d ' ')
  phone_n=$(adb shell "find $D/$d -type f 2>/dev/null | wc -l" | tr -d '\r ')
  if [ "$local_n" = "$phone_n" ]; then echo "== $d: already there ($local_n files)"; continue; fi
  echo "== $d ($local_n files)"
  adb push "$SRC/$d" "$D/" | tail -1
done
for f in config.ini language.txt users.txt; do [ -f "$SRC/$f" ] && adb push "$SRC/$f" "$D/" | tail -1; done

[ "$1" = --skip-romfs ] && exit 0
total=$(python3 tools/pack_romfs.py "$SRC/extract/romfs" --size)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
while :; do
  have=$(adb shell "stat -c %s $D/romfs.pak 2>/dev/null || echo 0" | tr -d '\r')
  [ "$have" -ge "$total" ] && break
  to=$((have + PART))
  echo "== romfs.pak: $((have >> 20)) of $((total >> 20)) MiB"
  python3 tools/pack_romfs.py "$SRC/extract/romfs" "$tmp/part" --from "$have" --to "$to" 2>/dev/null
  adb push "$tmp/part" "$D/romfs.part" | tail -1
  adb shell "cat $D/romfs.part >> $D/romfs.pak && rm $D/romfs.part"
done
[ "$have" -eq "$total" ] || { echo "romfs.pak is $have bytes, expected $total"; exit 1; }
echo "== romfs.pak complete ($((total >> 20)) MiB)"
# The loose RomFS of an older copy is no longer read: it only took space.
adb shell "rm -rf $D/extract" && echo "== removed the loose extract/romfs"
