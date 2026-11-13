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
# Temporal AA and frame generation (nvn_temporal.cpp): AMD's FidelityFX SDK and NVIDIA's DLSS SDK, the files and
# versions tools/setup_acnh.py fetches. Without them CMake builds the player with stubs (no FSR, DLAA or FFX frame
# generation) while the settings still offer them.
FFX=v2.4.0 DLSS=v310.9.16  # as tools/setup_acnh.py
FFX_URL=https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$FFX/Kits/FidelityFX
DLSS_URL=https://raw.githubusercontent.com/NVIDIA/DLSS/$DLSS
get() {  # <file under private/third_party> <url>
    local out=private/third_party/$1
    [ -f "$out" ] || { mkdir -p "$(dirname "$out")" && curl -fsSL -o "$out.download" "$2" && mv "$out.download" "$out"; }
}
for h in api/include/ffx_api.h api/include/ffx_api_types.h api/include/vk/ffx_api_vk.h upscalers/include/ffx_upscale.h \
         framegeneration/include/ffx_framegeneration.h framegeneration/include/ffx_framegeneration_api_types.h \
         framegeneration/include/vk/ffx_api_framegeneration_vk.h; do get "ffx/$h" "$FFX_URL/$h"; done
get ffx/LICENSE.md "$FFX_URL/docs/license.md"
for d in amd_fidelityfx_loader_vk.dll amd_fidelityfx_upscaler_vk.dll amd_fidelityfx_framegeneration_vk.dll; do
    get "ffx/bin/$d" "$FFX_URL/signedbin/$d"; done
for h in nvsdk_ngx.h nvsdk_ngx_defs.h nvsdk_ngx_defs_vk.h nvsdk_ngx_helpers.h nvsdk_ngx_helpers_vk.h nvsdk_ngx_params.h \
         nvsdk_ngx_vk.h; do get "dlss/include/$h" "$DLSS_URL/include/$h"; done
get dlss/LICENSE.txt "$DLSS_URL/LICENSE.txt"
get dlss/lib/nvsdk_ngx_d.lib "$DLSS_URL/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
get dlss/bin/nvngx_dlss.dll "$DLSS_URL/lib/Windows_x86_64/rel/nvngx_dlss.dll"

echo "== launcher and player (clang-cl)"
# Configured every time: CMake looks for the FidelityFX and DLSS SDKs when it configures.
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DNX_GAME=acnh \
        -DNX_GEN_DIR="$PWD/$BUILD/no-gen" -DCMAKE_BUILD_TYPE=Release -DNX_GAME_FROM_FOLDER=ON \
        "-DCMAKE_PREFIX_PATH=$SDL;$MOVIE" "-DNX_VULKAN_INCLUDE=$VKH" >/dev/null
cmake --build "$BUILD" --target acnhrecomp_launcher acnh

R=$OUT/ACNHRecomp
mkdir -p "$R"
cp "$BUILD/acnhrecomp_launcher.exe" "$R/ACNHRecomp Launcher.exe"
cp "$BUILD/acnh.exe" "$R/acnh.exe"
cp "$SDL/lib/x64/SDL3.dll" "$R/"
# The FidelityFX and DLSS DLLs the player loads at run time (the build copies them beside acnh.exe only when it
# compiled FSR / DLAA / FFX frame generation in), and their licences.
for d in amd_fidelityfx_loader_vk.dll amd_fidelityfx_upscaler_vk.dll amd_fidelityfx_framegeneration_vk.dll nvngx_dlss.dll; do
    [ -f "$BUILD/$d" ] || { echo "$BUILD/$d not found: the player was built without FidelityFX / DLSS"; exit 1; }
    cp "$BUILD/$d" "$R/"
done
mkdir -p "$R/licenses"
cp private/third_party/ffx/LICENSE.md "$R/licenses/FidelityFX-LICENSE.md"
cp private/third_party/dlss/LICENSE.txt "$R/licenses/DLSS-LICENSE.txt"
# The Visual C++ runtime the executables import (app-local: no installer needed).
CRT=$(ls -d "$(cygpath -u "$VCToolsRedistDir")"/x64/Microsoft.VC*.CRT | head -1)
cp "$CRT"/vcruntime140*.dll "$CRT"/msvcp140*.dll "$R/"  # msvcp140_1 too (the player's <memory_resource>)
# Each DLL the executables import is a system one or beside them.
for exe in "$R/acnh.exe" "$R/ACNHRecomp Launcher.exe"; do
    for dll in $(MSYS_NO_PATHCONV=1 dumpbin /nologo /dependents "$(cygpath -w "$exe")" | tr -d '\r' |
                 awk '/^ +[^ ]+\.(dll|DLL)$/ {print $1}'); do
        case "${dll,,}" in api-ms-win-*|kernel32.dll|user32.dll|shell32.dll|imm32.dll|winmm.dll|dxgi.dll|ole32.dll|\
                           psapi.dll|advapi32.dll|gdi32.dll|setupapi.dll|version.dll|oleaut32.dll|ws2_32.dll|bcrypt.dll) continue ;; esac
        ls "$R" | grep -qix "$dll" || { echo "$exe imports $dll, which is not in the package"; exit 1; }
    done
done
printf '@echo off\r\nrem Double-click: opens the launcher, which asks for the game folder ACNHRecomp-Importer prepared.\r\ncd /d "%%~dp0"\r\nstart "" "ACNHRecomp Launcher.exe"\r\n' > "$R/Start ACNHRecomp.bat"
# The pt-BR translation and the SpeedFix files, which the player copies into the game folder (mod_fetch.cpp).
tools/bundle_mods.sh "$R"
tools/release_readme.sh "$R" windows "$VER"
if command -v zip >/dev/null; then (cd "$OUT" && zip -qr "../$(basename "$OUT").zip" ACNHRecomp)
else (cd "$OUT" && 7z a -tzip -bso0 "../$(basename "$OUT").zip" ACNHRecomp); fi
rm -rf "${OUT:?}"
ls -la "$OUT.zip"
