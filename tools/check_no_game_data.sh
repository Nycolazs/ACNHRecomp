#!/usr/bin/env bash
# Refuses to let game data, keys or recompiled code into a commit. Run before every push
# (or install as a hook: ln -s ../../tools/check_no_game_data.sh .git/hooks/pre-commit).
# Checks the staged files: no private/ or refs/ paths, no dump/key/shader/binary extensions, no binary blobs (except
# README screenshots in docs/screenshots/),
# no generated recompiler output (function bodies named after guest addresses), no key material.
cd "$(git rev-parse --show-toplevel)" || exit 1
bad=0
while IFS= read -r f; do
  [ -f "$f" ] || continue
  case "$f" in
    private/*|refs/*) echo "BLOCKED (local-only folder): $f"; bad=1; continue ;;
  esac
  lf=$(printf '%s' "$f" | tr '[:upper:]' '[:lower:]')  # (macOS ships bash 3.2: no ${f,,})
  case "$lf" in
    *.nsp|*.xci|*.nca|*.nso|*.npdm|*.keys|*.tik|*.cert|*.spv|*.bin|*.bfsha|*.bntx|*.szs|*.zs|*.sarc|*.bfres|*.ttf|*.otf)
      echo "BLOCKED (game/derived file type): $f"; bad=1; continue ;;
  esac
  # README screenshots (pictures of the running game, no game files) are the only binaries allowed.
  case "$lf" in
    docs/screenshots/*.png|docs/screenshots/*.jpg) continue ;;
    android/res/mipmap-*/*.png|android/icon_512.png) continue ;;  # the launcher icon, drawn by tools/make_android_icon.py
  esac
  if [ "$(head -c 8000 "$f" | tr -d '\000' | wc -c)" -ne "$(head -c 8000 "$f" | wc -c)" ]; then
    echo "BLOCKED (binary content): $f"; bad=1; continue
  fi
  if grep -qE '^void (main|sdk|subsdk[0-9])_[0-9A-F]{8}\(nx::CPU' "$f"; then
    echo "BLOCKED (recompiled game code): $f"; bad=1; continue
  fi
  if grep -qiE '^(master_key|header_key|titlekek|key_area_key|aes_kek_generation)[_0-9a-z]* *= *[0-9a-f]{32}' "$f"; then
    echo "BLOCKED (key material): $f"; bad=1; continue
  fi
done < <(git diff --cached --name-only --diff-filter=ACMR)
[ $bad = 0 ] && echo "ok: no game data staged" || { echo "commit refused"; exit 1; }
