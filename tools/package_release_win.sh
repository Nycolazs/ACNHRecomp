#!/bin/bash
# The Windows release package, made on Windows (Git Bash) in a Visual Studio x64 environment with clang-cl, as the
# game's library is (the player and game/windows-x64/acnh_game.dll share C++ exceptions: one compiler family): the
# launcher and the player (acnh.exe, built with NX_GAME_FROM_FOLDER: no game code), SDL3.dll (zlib) and the Visual C++
# runtime (redistributable) beside them, and "Start ACNHRecomp.bat". The player plays the game folder
# ACNHRecomp-Importer prepared: the launcher only asks for that folder (README, "Installation"). libvpx and libvorbis
# are linked in (vcpkg's static libraries); the Vulkan loader comes with the GPU driver.
# usage: tools/package_release_win.sh <version> <SDL3 VC prefix> <movie libraries prefix> <Vulkan-Headers include>
#   <SDL3 VC prefix>: SDL's SDL3-devel-<x.y.z>-VC.zip, extracted (https://github.com/libsdl-org/SDL/releases)
#   <movie libraries prefix>: vcpkg's installed/x64-windows-static-md-release (libvpx, libvorbis)
#                                                    -> private/release/ACNHRecomp-<version>-windows-x64.zip
# needs: clang-cl, cmake, ninja, zip (or 7z), and the environment of vcvars64.bat
cd "$(dirname "$0")/.." || exit 1
set -e
USAGE="usage: tools/package_release_win.sh <version> <SDL3 VC prefix> <movie libraries prefix> <Vulkan-Headers include>"
VER=${1:?$USAGE}
SDL=${2:?$USAGE}
MOVIE=${3:?$USAGE}
VKH=${4:?$USAGE}
[ -f "$SDL/lib/x64/SDL3.dll" ] || { echo "$SDL/lib/x64/SDL3.dll not found"; exit 1; }
[ -n "$VCToolsRedistDir" ] || { echo "run it in a Visual Studio x64 environment (vcvars64.bat)"; exit 1; }
git diff --quiet HEAD -- || { echo "uncommitted changes: commit first (the package is made from HEAD)"; exit 1; }
BUILD=private/release/build-win
OUT=private/release/ACNHRecomp-$VER-windows-x64
rm -rf "${OUT:?}" "$OUT.zip"
mkdir -p "$OUT"

# Dear ImGui (the launcher, and the player's settings screen: without it a stub), as tools/setup_acnh.py fetches it.
IMGUI=private/third_party/imgui
[ -f "$IMGUI/imgui.cpp" ] || { mkdir -p "$IMGUI" && curl -fsSL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz |
                               tar -xz --strip-components=1 -C "$IMGUI"; }

echo "== launcher and player (clang-cl)"
[ -f "$BUILD/CMakeCache.txt" ] ||
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DNX_GAME=acnh \
        -DNX_GEN_DIR="$PWD/$BUILD/no-gen" -DCMAKE_BUILD_TYPE=Release -DNX_GAME_FROM_FOLDER=ON \
        "-DCMAKE_PREFIX_PATH=$SDL;$MOVIE" "-DNX_VULKAN_INCLUDE=$VKH" >/dev/null
cmake --build "$BUILD" --target acnhrecomp_launcher acnh

R=$OUT/ACNHRecomp
mkdir -p "$R"
cp "$BUILD/acnhrecomp_launcher.exe" "$R/ACNHRecomp Launcher.exe"
cp "$BUILD/acnh.exe" "$R/acnh.exe"
cp "$SDL/lib/x64/SDL3.dll" "$R/"
# The Visual C++ runtime the executables import (app-local: no installer needed).
CRT=$(ls -d "$(cygpath -u "$VCToolsRedistDir")"/x64/Microsoft.VC*.CRT | head -1)
for dll in vcruntime140.dll vcruntime140_1.dll msvcp140.dll; do cp "$CRT/$dll" "$R/"; done
printf '@echo off\r\nrem Double-click: opens the launcher, which asks for the game folder ACNHRecomp-Importer prepared.\r\ncd /d "%%~dp0"\r\nstart "" "ACNHRecomp Launcher.exe"\r\n' > "$R/Start ACNHRecomp.bat"
tools/release_readme.sh "$R" windows
if command -v zip >/dev/null; then (cd "$OUT" && zip -qr "../$(basename "$OUT").zip" ACNHRecomp)
else (cd "$OUT" && 7z a -tzip -bso0 "../$(basename "$OUT").zip" ACNHRecomp); fi
rm -rf "${OUT:?}"
ls -la "$OUT.zip"
