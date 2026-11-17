#!/bin/bash
# The Linux release package: the launcher and the player (acnh, built with NX_GAME_FROM_FOLDER: no game code), with
# SDL3 (zlib), libvpx (BSD), libvorbis and libogg (BSD) beside them, found through $ORIGIN, and "Start ACNHRecomp.sh".
# The launcher prepares the game folder from the player's extracted ExeFS and RomFS (prepare/README.md) with the tools
# beside it, tools/zig (tools/fetch_zig.py) and tools/shaders (tools/build_shader_tool.py), and acnh_prepare, its
# command line; the player plays that folder (game/linux-x64/). The Vulkan loader and driver are the system's. Built on
# the oldest system it should run on (CI: Ubuntu 22.04, glibc 2.35).
# usage: tools/package_release_linux.sh <version> [<SDL3 prefix> [<Vulkan-Headers include>]]
#   <SDL3 prefix>: an SDL3 install (cmake --install of SDL's SDL3-<x.y.z>.tar.gz); default: the system's
#   <Vulkan-Headers include>: default: the system's
#                                                    -> private/release/ACNHRecomp-<version>-linux-x64.tar.gz
# needs: clang, cmake, ninja, patchelf, libvulkan-dev, libvpx-dev, libvorbis-dev, python3, the .NET 10 SDK and zlib1g-dev
#        (the shader tool, NativeAOT), and the network (zig from ziglang.org, checked against its SHA-256)
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
cmake --build "$BUILD" --target acnhrecomp_launcher acnh acnh_prepare

R=$OUT/ACNHRecomp
mkdir -p "$R"
cp "$BUILD/acnhrecomp_launcher" "$R/ACNHRecomp Launcher"
cp "$BUILD/acnh" "$R/acnh"
cp "$BUILD/acnh_prepare" "$R/acnh_prepare"
cp assets/acnhrecomp.png "$R/acnhrecomp.png"  # the icon of the shortcut "Add to desktop" makes (launcher/desktop.cpp)
strip "$R/ACNHRecomp Launcher" "$R/acnh" "$R/acnh_prepare"
# The libraries a stock system may lack (or have another version of), beside the executables: their sonames, found
# through $ORIGIN. The C and C++ runtimes, X11 / Wayland, the Vulkan loader and the GPU driver stay the system's.
for lib in libSDL3.so.0 libvpx.so libvorbis.so.0 libogg.so.0; do
    path=$(ldd "$R/acnh" "$R/ACNHRecomp Launcher" | awk -v l="$lib" 'index($1, l) == 1 {print $3; exit}')
    [ -f "$path" ] || { echo "$lib: not linked (ldd)"; exit 1; }
    cp -L "$path" "$R/$(basename "$path")"
done
for f in "$R/acnh" "$R/ACNHRecomp Launcher" "$R"/lib*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done
echo "needs: $(objdump -T "$R/acnh" "$R/ACNHRecomp Launcher" "$R/acnh_prepare" "$R"/lib*.so* | grep -o 'GLIBC_[0-9.]*\|GLIBCXX_[0-9.]*' |
               sort -uV | awk -F_ '{v[$1]=$0} END {print v["GLIBC"], v["GLIBCXX"]}')"

echo "== the preparation's tools"
# Where the launcher and acnh_prepare look for them (launcher/prepare_job.cpp, prepare/prepare_cli.cpp): tools/zig/zig
# (zig, trimmed to the four targets, with the sizes.txt the preparation checks it against) and
# tools/shaders/spirv_harness (the shader tool, NativeAOT: no .NET needed where it runs). zig is kept between runs in
# private/release/zig-x86_64-linux.
PY=${PYTHON:-python3}
ZIG=private/release/zig-x86_64-linux
[ -f "$ZIG/sizes.txt" ] || "$PY" tools/fetch_zig.py x86_64-linux "$ZIG"
mkdir -p "$R/tools"
cp -a "$ZIG" "$R/tools/zig"
"$PY" tools/build_shader_tool.py linux-x64 "$R/tools/shaders"
[ -x "$R/tools/zig/zig" ] && [ -x "$R/tools/shaders/spirv_harness" ] || { echo "the preparation's tools are missing"; exit 1; }

cat > "$R/Start ACNHRecomp.sh" <<'EOF'
#!/bin/bash
# Run it (or open it from the file manager): opens the launcher.
cd "$(dirname "$0")" || exit 1
exec "./ACNHRecomp Launcher" "$@"
EOF
chmod +x "$R/Start ACNHRecomp.sh"
# The pt-BR translation and the SpeedFix files, which the player copies into the game folder (mod_fetch.cpp).
tools/bundle_mods.sh "$R"
tools/release_readme.sh "$R" linux "$VER" "$BUILD"
# A tarball keeps the executables executable.
tar -czf "$OUT.tar.gz" -C "$OUT" ACNHRecomp
rm -rf "${OUT:?}"
ls -la "$OUT.tar.gz"
