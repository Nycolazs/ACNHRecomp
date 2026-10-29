#!/bin/bash
# The Linux release package: the launcher and the player (acnh, built with NX_GAME_FROM_FOLDER: no game code), with
# SDL3 (zlib), libvpx (BSD), libvorbis and libogg (BSD) beside them, found through $ORIGIN, and "Start ACNHRecomp.sh".
# The player plays the game folder ACNHRecomp-Importer prepared (game/linux-x64/): the launcher only asks for that
# folder (README, "Installation"). The Vulkan loader and driver are the system's. Built on the oldest system it should
# run on (CI: Ubuntu 22.04, glibc 2.35).
# usage: tools/package_release_linux.sh <version> [<SDL3 prefix> [<Vulkan-Headers include>]]
#   <SDL3 prefix>: an SDL3 install (cmake --install of SDL's SDL3-<x.y.z>.tar.gz); default: the system's
#   <Vulkan-Headers include>: default: the system's
#                                                    -> private/release/ACNHRecomp-<version>-linux-x64.tar.gz
# needs: clang, cmake, ninja, patchelf, libvulkan-dev, libvpx-dev, libvorbis-dev
cd "$(dirname "$0")/.." || exit 1
set -e
VER=${1:?usage: tools/package_release_linux.sh <version> [<SDL3 prefix> [<Vulkan-Headers include>]]}
SDL=${2:-}
VKH=${3:-}
[ "$(uname)" = Linux ] || { echo "Linux only"; exit 1; }
git diff --quiet HEAD -- || { echo "uncommitted changes: commit first (the package is made from HEAD)"; exit 1; }
BUILD=private/release/build-linux
OUT=private/release/ACNHRecomp-$VER-linux-x64
rm -rf "${OUT:?}" "$OUT.tar.gz"
mkdir -p "$OUT"

# Dear ImGui (the launcher, and the player's settings screen: without it a stub), as tools/setup_acnh.py fetches it.
IMGUI=private/third_party/imgui
[ -f "$IMGUI/imgui.cpp" ] || { mkdir -p "$IMGUI" && curl -fsSL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz |
                               tar -xz --strip-components=1 -C "$IMGUI"; }

echo "== launcher and player"
[ -f "$BUILD/CMakeCache.txt" ] ||
  CC=${CC:-clang} CXX=${CXX:-clang++} cmake -S . -B "$BUILD" -G Ninja -DNX_GAME=acnh -DNX_GEN_DIR="$PWD/$BUILD/no-gen" \
        -DCMAKE_BUILD_TYPE=Release -DNX_GAME_FROM_FOLDER=ON ${SDL:+"-DCMAKE_PREFIX_PATH=$SDL"} \
        ${VKH:+"-DNX_VULKAN_INCLUDE=$VKH"} >/dev/null
cmake --build "$BUILD" --target acnhrecomp_launcher acnh

R=$OUT/ACNHRecomp
mkdir -p "$R"
cp "$BUILD/acnhrecomp_launcher" "$R/ACNHRecomp Launcher"
cp "$BUILD/acnh" "$R/acnh"
strip "$R/ACNHRecomp Launcher" "$R/acnh"
# The libraries a stock system may lack (or have another version of), beside the executables: their sonames, found
# through $ORIGIN. The C and C++ runtimes, X11 / Wayland, the Vulkan loader and the GPU driver stay the system's.
for lib in libSDL3.so.0 libvpx.so libvorbis.so.0 libogg.so.0; do
    path=$(ldd "$R/acnh" "$R/ACNHRecomp Launcher" | awk -v l="$lib" 'index($1, l) == 1 {print $3; exit}')
    [ -f "$path" ] || { echo "$lib: not linked (ldd)"; exit 1; }
    cp -L "$path" "$R/$(basename "$path")"
done
for f in "$R/acnh" "$R/ACNHRecomp Launcher" "$R"/lib*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done
echo "needs: $(objdump -T "$R/acnh" "$R/ACNHRecomp Launcher" "$R"/lib*.so* | grep -o 'GLIBC_[0-9.]*\|GLIBCXX_[0-9.]*' |
               sort -uV | awk -F_ '{v[$1]=$0} END {print v["GLIBC"], v["GLIBCXX"]}')"

cat > "$R/Start ACNHRecomp.sh" <<'EOF'
#!/bin/bash
# Run it (or open it from the file manager): opens the launcher.
cd "$(dirname "$0")" || exit 1
exec "./ACNHRecomp Launcher" "$@"
EOF
chmod +x "$R/Start ACNHRecomp.sh"
tools/release_readme.sh "$R" linux
# A tarball keeps the executables executable.
tar -czf "$OUT.tar.gz" -C "$OUT" ACNHRecomp
rm -rf "${OUT:?}"
ls -la "$OUT.tar.gz"
