#!/bin/bash
# The macOS release package: the launcher and the player (acnh, built with NX_GAME_FROM_FOLDER: no game code), with
# what they need beside them so that nothing has to be installed: SDL3 (zlib), the Vulkan loader (Apache 2.0),
# MoltenVK (Apache 2.0) and its driver file, libvpx (BSD), libvorbis and libogg (BSD), and "Start ACNHRecomp.command"
# to double-click. The launcher prepares the game folder from the player's extracted ExeFS and RomFS
# (prepare/README.md) with the tools in Contents/Resources/tools: zig (tools/fetch_zig.py) and the shader tool
# (tools/build_shader_tool.py); acnh_prepare, its command line, is beside the launcher. The player plays that folder
# (game/macos-arm64/).
# usage: tools/package_release_mac.sh <version>      -> private/release/ACNHRecomp-<version>-macOS-arm64.zip
# needs: cmake, ninja, brew install sdl3 molten-vk vulkan-loader vulkan-headers libvpx libvorbis; python3, the .NET 10
#        SDK (the shader tool, NativeAOT) and the network (zig from ziglang.org)
cd "$(dirname "$0")/.." || exit 1
set -e
# The oldest macOS the package runs on: the compiler, Info.plist and the check at the end follow it.
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-15.0}
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
cmake --build "$BUILD" --target acnhrecomp_launcher acnh acnh_prepare

R=$OUT/ACNHRecomp
# ACNHRecomp.app: the launcher is the app (its icon, Applications, the Dock); the player, acnh_prepare, the libraries
# and MoltenVK go beside it in Contents/MacOS, where the launcher (files.cpp own_dir) and the player
# (platform_mac.cpp) look; MoltenVK's ICD file and the preparation's tools (zig's lib folder is thousands of headers
# and sources, no code to sign one by one) in Contents/Resources (code only in MacOS, for the bundle's signature).
APP=$R/ACNHRecomp.app
M=$APP/Contents/MacOS
mkdir -p "$M" "$APP/Contents/Resources"
cp "$BUILD/acnhrecomp_launcher" "$M/ACNHRecomp Launcher"
cp "$BUILD/acnh" "$M/acnh"
cp "$BUILD/acnh_prepare" "$M/acnh_prepare"
cp assets/acnhrecomp.icns "$APP/Contents/Resources/acnhrecomp.icns"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>ACNHRecomp</string>
    <key>CFBundleDisplayName</key><string>ACNHRecomp</string>
    <key>CFBundleIdentifier</key><string>io.github.acnhrecomp</string>
    <key>CFBundleExecutable</key><string>ACNHRecomp Launcher</string>
    <key>CFBundleIconFile</key><string>acnhrecomp</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${VER#v}</string>
    <key>CFBundleVersion</key><string>${VER#v}</string>
    <key>LSMinimumSystemVersion</key><string>${MACOSX_DEPLOYMENT_TARGET:-15.0}</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

# Each Homebrew library the two executables (and those libraries) link, copied beside them and named through
# @executable_path; then the Vulkan loader and MoltenVK, which are loaded at run time (platform_mac.cpp).
bundle() {  # <binary>: its Homebrew dependencies, recursively
    local dep base
    for dep in $(otool -L "$1" | awk 'NR > 1 {print $1}' | grep -E "^($BREW|/opt/homebrew|/usr/local)/"); do
        base=$(basename "$dep")
        if [ ! -f "$M/$base" ]; then
            cp -L "$dep" "$M/$base"
            chmod u+w "$M/$base"
            install_name_tool -id "@executable_path/$base" "$M/$base"
            bundle "$M/$base"
        fi
        install_name_tool -change "$dep" "@executable_path/$base" "$1"
    done
}
cp -L "$BREW/opt/vulkan-loader/lib/libvulkan.1.dylib" "$M/libvulkan.1.dylib"
cp -L "$BREW/opt/molten-vk/lib/libMoltenVK.dylib" "$M/libMoltenVK.dylib"
chmod u+w "$M"/*.dylib
install_name_tool -id "@executable_path/libvulkan.1.dylib" "$M/libvulkan.1.dylib"
install_name_tool -id "@executable_path/libMoltenVK.dylib" "$M/libMoltenVK.dylib"
for b in "$M/ACNHRecomp Launcher" "$M/acnh" "$M/acnh_prepare" "$M/libvulkan.1.dylib" "$M/libMoltenVK.dylib"; do
    bundle "$b"
done
cat > "$APP/Contents/Resources/MoltenVK_icd.json" <<'ICD'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../MacOS/libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
ICD
# The pt-BR translation and the SpeedFix files (Resources: data, not code), which the player copies into the game
# folder (mod_fetch.cpp). Before the signature, which covers them.
tools/bundle_mods.sh "$APP/Contents/Resources"
# The preparation's tools, where the launcher and acnh_prepare look for them in the app (launcher/prepare_job.cpp,
# prepare/prepare_cli.cpp: ../Resources/tools from Contents/MacOS): tools/zig/zig (trimmed to the four targets, with
# the sizes.txt the preparation checks it against) and tools/shaders/spirv_harness (NativeAOT: no .NET needed where it
# runs). zig is kept between runs in private/release/zig-aarch64-macos. Signed with the rest: Apple Silicon runs no
# unsigned code.
TOOLS=$APP/Contents/Resources/tools
ZIG=private/release/zig-aarch64-macos
[ -f "$ZIG/sizes.txt" ] || python3 tools/fetch_zig.py aarch64-macos "$ZIG"
mkdir -p "$TOOLS"
cp -a "$ZIG" "$TOOLS/zig"
python3 tools/build_shader_tool.py osx-arm64 "$TOOLS/shaders"
[ -x "$TOOLS/zig/zig" ] && [ -x "$TOOLS/shaders/spirv_harness" ] || { echo "the preparation's tools are missing"; exit 1; }
for f in "$M"/*.dylib "$M/acnh" "$M/acnh_prepare" "$TOOLS/zig/zig" "$TOOLS/shaders/spirv_harness"; do
    codesign --force -s - "$f" 2>/dev/null
done
codesign --force -s - "$APP"  # the bundle last: its seal covers the files above
if otool -L "$M/acnh" "$M/ACNHRecomp Launcher" "$M/acnh_prepare" "$M"/*.dylib | grep -E "$BREW|/opt/homebrew|/usr/local"; then
  echo "a library still points at Homebrew"; exit 1
fi
# Every binary must load on the macOS the bundle declares (LSMinimumSystemVersion): Homebrew builds its libraries for
# the runner's macOS, whatever MACOSX_DEPLOYMENT_TARGET says.
MIN=${MACOSX_DEPLOYMENT_TARGET:-15.0}
for f in "$M/acnh" "$M/ACNHRecomp Launcher" "$M/acnh_prepare" "$M"/*.dylib "$TOOLS/zig/zig" "$TOOLS/shaders/spirv_harness"; do
  need=$(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b = 1} b && $1 == "minos" {print $2; exit}')
  if [ -n "$need" ] && [ "$(printf '%s\n%s\n' "$need" "$MIN" | sort -V | tail -1)" != "$MIN" ]; then
    echo "$(basename "$f") needs macOS $need, newer than the declared $MIN"; exit 1
  fi
done

cat > "$R/Start ACNHRecomp.command" <<'CMD'
#!/bin/bash
# Double-click the first time: frees ACNHRecomp.app of the download quarantine (macOS blocks unsigned downloaded
# apps) and opens it. Afterwards ACNHRecomp.app opens by itself; its home page installs it in Applications.
cd "$(dirname "$0")" || exit 1
xattr -dr com.apple.quarantine . 2>/dev/null
open ACNHRecomp.app
CMD
chmod +x "$R/Start ACNHRecomp.command"
tools/release_readme.sh "$R" macos "$VER" "$BUILD"
(cd "$OUT" && ditto -c -k --sequesterRsrc --keepParent ACNHRecomp "../$(basename "$OUT").zip")
rm -rf "${OUT:?}"
ls -la "$OUT.zip"
