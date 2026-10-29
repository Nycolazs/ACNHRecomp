#!/bin/bash
# The macOS release package: the sources the setup needs at its root (tools/release_sources.sh: nothing of the game)
# with the launcher already built beside them, SDL3 (zlib licence) beside it so it opens without Homebrew's, and a
# "Start ACNHRecomp.command" to double-click. The launcher then checks the computer and sets the game up from the
# player's own files (README, "Installation").
# usage: tools/package_release_mac.sh <version>      -> private/release/ACNHRecomp-<version>-macOS-arm64.zip
cd "$(dirname "$0")/.." || exit 1
set -e
VER=${1:?usage: tools/package_release_mac.sh <version>}
[ "$(uname)" = Darwin ] || { echo "macOS only"; exit 1; }
git diff --quiet HEAD -- || { echo "uncommitted changes: commit first (the package is made from HEAD)"; exit 1; }
BUILD=private/release/launcher-build
OUT=private/release/ACNHRecomp-$VER-macOS-arm64
rm -rf "${OUT:?}" "$OUT.zip"
mkdir -p "$OUT"

echo "== launcher"
[ -f "$BUILD/CMakeCache.txt" ] ||
  cmake -S . -B "$BUILD" -G Ninja -DNX_GAME=acnh -DNX_GEN_DIR="$PWD/$BUILD/no-gen" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$(brew --prefix)" >/dev/null
cmake --build "$BUILD" --target acnhrecomp_launcher

echo "== sources (tools/release_sources.sh)"
R=$OUT
tools/release_sources.sh "$R" macos
cp "$BUILD/acnhrecomp_launcher" "$R/ACNHRecomp Launcher"
SDL=$(otool -L "$R/ACNHRecomp Launcher" | awk '/libSDL3/ {print $1}')
cp "$SDL" "$R/$(basename "$SDL")"
chmod u+w "$R/$(basename "$SDL")"
install_name_tool -change "$SDL" "@executable_path/$(basename "$SDL")" "$R/ACNHRecomp Launcher"
install_name_tool -id "@executable_path/$(basename "$SDL")" "$R/$(basename "$SDL")"
codesign --force -s - "$R/$(basename "$SDL")"
codesign --force -s - "$R/ACNHRecomp Launcher"
cat > "$R/Start ACNHRecomp.command" <<'EOF'
#!/bin/bash
# Double-click: opens the launcher (the first time macOS may ask: right-click > Open).
cd "$(dirname "$0")" || exit 1
xattr -dr com.apple.quarantine . 2>/dev/null
exec "./ACNHRecomp Launcher"
EOF
chmod +x "$R/Start ACNHRecomp.command"
ditto -c -k --sequesterRsrc "$OUT" "$OUT.zip"
rm -rf "${OUT:?}"
ls -la "$OUT.zip"
