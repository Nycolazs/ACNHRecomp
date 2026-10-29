#!/bin/bash
# The macOS release package: the launcher and the player (acnh, built with NX_GAME_FROM_FOLDER: no game code), with
# what they need beside them so that nothing has to be installed: SDL3 (zlib), the Vulkan loader (Apache 2.0),
# MoltenVK (Apache 2.0) and its driver file, libvpx (BSD), libvorbis and libogg (BSD), and "Start ACNHRecomp.command"
# to double-click. The player plays the game folder ACNHRecomp-Importer prepared (game/macos-arm64/): the launcher
# only asks for that folder (README, "Installation").
# usage: tools/package_release_mac.sh <version>      -> private/release/ACNHRecomp-<version>-macOS-arm64.zip
# needs: cmake, ninja, brew install sdl3 molten-vk vulkan-loader vulkan-headers libvpx libvorbis
cd "$(dirname "$0")/.." || exit 1
set -e
VER=${1:?usage: tools/package_release_mac.sh <version>}
[ "$(uname)" = Darwin ] || { echo "macOS only"; exit 1; }
git diff --quiet HEAD -- || { echo "uncommitted changes: commit first (the package is made from HEAD)"; exit 1; }
BREW=$(brew --prefix)
BUILD=private/release/build-mac
OUT=private/release/ACNHRecomp-$VER-macOS-arm64
rm -rf "${OUT:?}" "$OUT.zip"
mkdir -p "$OUT"

# Dear ImGui (the launcher, and the player's settings screen: without it a stub), as tools/setup_acnh.py fetches it.
IMGUI=private/third_party/imgui
[ -f "$IMGUI/imgui.cpp" ] || { mkdir -p "$IMGUI" && curl -fsSL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz |
                               tar -xz --strip-components=1 -C "$IMGUI"; }

echo "== launcher and player"
[ -f "$BUILD/CMakeCache.txt" ] ||
  cmake -S . -B "$BUILD" -G Ninja -DNX_GAME=acnh -DNX_GEN_DIR="$PWD/$BUILD/no-gen" -DCMAKE_BUILD_TYPE=Release \
        -DNX_GAME_FROM_FOLDER=ON -DCMAKE_PREFIX_PATH="$BREW" >/dev/null
cmake --build "$BUILD" --target acnhrecomp_launcher acnh

R=$OUT/ACNHRecomp
mkdir -p "$R"
cp "$BUILD/acnhrecomp_launcher" "$R/ACNHRecomp Launcher"
cp "$BUILD/acnh" "$R/acnh"

# Each Homebrew library the two executables (and those libraries) link, copied beside them and named through
# @executable_path; then the Vulkan loader and MoltenVK, which are loaded at run time (platform_mac.cpp).
bundle() {  # <binary>: its Homebrew dependencies, recursively
    local dep base
    for dep in $(otool -L "$1" | awk 'NR > 1 {print $1}' | grep -E "^($BREW|/opt/homebrew|/usr/local)/"); do
        base=$(basename "$dep")
        if [ ! -f "$R/$base" ]; then
            cp -L "$dep" "$R/$base"
            chmod u+w "$R/$base"
            install_name_tool -id "@executable_path/$base" "$R/$base"
            bundle "$R/$base"
        fi
        install_name_tool -change "$dep" "@executable_path/$base" "$1"
    done
}
cp -L "$BREW/opt/vulkan-loader/lib/libvulkan.1.dylib" "$R/libvulkan.1.dylib"
cp -L "$BREW/opt/molten-vk/lib/libMoltenVK.dylib" "$R/libMoltenVK.dylib"
chmod u+w "$R"/*.dylib
install_name_tool -id "@executable_path/libvulkan.1.dylib" "$R/libvulkan.1.dylib"
install_name_tool -id "@executable_path/libMoltenVK.dylib" "$R/libMoltenVK.dylib"
for b in "$R/ACNHRecomp Launcher" "$R/acnh" "$R/libvulkan.1.dylib" "$R/libMoltenVK.dylib"; do bundle "$b"; done
cat > "$R/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "./libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
EOF
for f in "$R"/*.dylib "$R/acnh" "$R/ACNHRecomp Launcher"; do codesign --force -s - "$f" 2>/dev/null; done
if otool -L "$R/acnh" "$R/ACNHRecomp Launcher" "$R"/*.dylib | grep -E "$BREW|/opt/homebrew|/usr/local"; then
  echo "a library still points at Homebrew"; exit 1
fi

cat > "$R/Start ACNHRecomp.command" <<'EOF'
#!/bin/bash
# Double-click: opens the launcher (the first time macOS may ask: right-click > Open).
cd "$(dirname "$0")" || exit 1
xattr -dr com.apple.quarantine . 2>/dev/null
exec "./ACNHRecomp Launcher"
EOF
chmod +x "$R/Start ACNHRecomp.command"
tools/release_readme.sh "$R" macos
(cd "$OUT" && ditto -c -k --sequesterRsrc --keepParent ACNHRecomp "../$(basename "$OUT").zip")
rm -rf "${OUT:?}"
ls -la "$OUT.zip"
