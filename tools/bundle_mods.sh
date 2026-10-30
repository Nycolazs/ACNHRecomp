#!/bin/bash
# The mods every package carries (runtime/src/mod_fetch.cpp): the pt-BR fan translation and the SpeedFix's parameter
# files, from this repository's "mods" release (made from the game's files: never committed). Each zip is checked
# against its SHA-256 below, extracted into <dest>/mods/<name>/, and <dest>/mods/bundle.txt lists them: the bundle's
# version (the zips' hashes), then every file. The zips are kept in private/mods_cache/.
#
# usage: tools/bundle_mods.sh <dest>      (a package's folder: beside the player, the app's Resources, the APK's assets)
# To change a mod: upload the new zip to the "mods" release (gh release upload mods <zip> --clobber) and put its
# SHA-256 here.
cd "$(dirname "$0")/.." || exit 1
set -e
DEST=${1:?usage: tools/bundle_mods.sh <dest>}
URL=https://github.com/Nycolazs/ACNHRecomp/releases/download/mods
MODS="ptbr:ACNHRecomp-ptbr-mod.zip:5171d2503bba635e089834e746294bb24feb93f129ce316309305360ed2f20d3
speedfix60:ACNHRecomp-speedfix60-mod.zip:524bcfc32e7b42af32bf86706ac294283d93cd86a46aa7bc7dbe077acaa16761"
CACHE=private/mods_cache
mkdir -p "$CACHE" "$DEST/mods"
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
VERSION=
for m in $MODS; do
  IFS=: read -r name zip sum <<<"$m"
  if [ ! -f "$CACHE/$zip" ] || [ "$(sha "$CACHE/$zip")" != "$sum" ]; then
    curl -fsSL --retry 3 -o "$CACHE/$zip.part" "$URL/$zip"
    mv "$CACHE/$zip.part" "$CACHE/$zip"
  fi
  [ "$(sha "$CACHE/$zip")" = "$sum" ] || { echo "$zip: not the expected file (SHA-256 $(sha "$CACHE/$zip"))"; exit 1; }
  rm -rf "${DEST:?}/mods/$name"
  unzip -q -o "$CACHE/$zip" -d "$DEST/mods"
  [ -d "$DEST/mods/$name/romfs" ] || { echo "$zip: no $name/romfs inside"; exit 1; }
  VERSION="$VERSION${sum:0:8}"
done
find "$DEST/mods" -name .DS_Store -delete
{
  echo "$VERSION"
  (cd "$DEST/mods" && find . -type f ! -name bundle.txt | sed 's|^\./||' | LC_ALL=C sort)
} > "$DEST/mods/bundle.txt"
echo "mods: $(($(wc -l < "$DEST/mods/bundle.txt") - 1)) files in $DEST/mods (bundle $VERSION)"
