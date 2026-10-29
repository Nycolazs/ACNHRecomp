#!/bin/bash
# The Linux release package: the sources the setup needs at its root (tools/release_sources.sh: nothing of the game)
# with the launcher already built beside them, SDL3 (zlib licence) beside it so it opens without the system's, and
# "Start ACNHRecomp.sh" to run. The launcher then checks the computer and sets the game up from the player's own files
# (README, "Installation"); building the game still needs the packages of README "Computer".
# usage: tools/package_release_linux.sh <version> [<SDL3 prefix>]
#   <SDL3 prefix>: an SDL3 install (cmake --install of SDL's SDL3-<x.y.z>.tar.gz); default: the system's
#                                                    -> private/release/ACNHRecomp-<version>-linux-x64.tar.gz
# needs: a C++20 compiler, cmake, ninja, patchelf
cd "$(dirname "$0")/.." || exit 1
set -e
VER=${1:?usage: tools/package_release_linux.sh <version> [<SDL3 prefix>]}
SDL=${2:-}
[ "$(uname)" = Linux ] || { echo "Linux only"; exit 1; }
git diff --quiet HEAD -- || { echo "uncommitted changes: commit first (the package is made from HEAD)"; exit 1; }
BUILD=private/release/launcher-linux
OUT=private/release/ACNHRecomp-$VER-linux-x64
rm -rf "${OUT:?}" "$OUT.tar.gz"
mkdir -p "$OUT"

echo "== launcher"
[ -f "$BUILD/CMakeCache.txt" ] ||
  cmake -S . -B "$BUILD" -G Ninja -DNX_GAME=acnh -DNX_GEN_DIR="$PWD/$BUILD/no-gen" -DCMAKE_BUILD_TYPE=Release \
        ${SDL:+"-DCMAKE_PREFIX_PATH=$SDL"} >/dev/null
cmake --build "$BUILD" --target acnhrecomp_launcher

echo "== sources (tools/release_sources.sh)"
R=$OUT
tools/release_sources.sh "$R" linux
cp "$BUILD/acnhrecomp_launcher" "$R/ACNHRecomp Launcher"
strip "$R/ACNHRecomp Launcher"
# libSDL3.so.0 (its soname) beside the launcher, found there through $ORIGIN.
SDLSO=$(ldd "$R/ACNHRecomp Launcher" | awk '/libSDL3/ {print $3}')
[ -f "$SDLSO" ] || { echo "libSDL3 not found (ldd)"; exit 1; }
cp -L "$SDLSO" "$R/$(basename "$SDLSO")"
patchelf --set-rpath '$ORIGIN' "$R/ACNHRecomp Launcher"
cat > "$R/Start ACNHRecomp.sh" <<'EOF'
#!/bin/bash
# Run it (or open it from the file manager): opens the launcher.
cd "$(dirname "$0")" || exit 1
exec "./ACNHRecomp Launcher" "$@"
EOF
chmod +x "$R/Start ACNHRecomp.sh"
# A tarball keeps the launcher and the scripts executable.
tar -czf "$OUT.tar.gz" -C "$OUT" .
rm -rf "${OUT:?}"
ls -la "$OUT.tar.gz"
