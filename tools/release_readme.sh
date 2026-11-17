#!/bin/bash
# The text files of a release package (tools/package_release_*.sh), in <folder>: README.txt (how to play, for that
# system), LICENSE (this project's), THIRD-PARTY.txt (the libraries beside the executables and built in),
# LICENSE-capstone*.txt (Capstone is built into the launcher and acnh_prepare; BSD 3-clause and LLVM's licence ask for
# their notice in a binary distribution), SPEEDFIX.txt (tmouh's SpeedFix, built in with them: CC BY-NC 4.0 asks for its
# attribution) and, with <build>, LICENSE-mbedtls.txt (Mbed TLS is linked into acnh; Apache 2.0 asks for its licence
# text in the package).
# usage: tools/release_readme.sh <folder> windows|macos|linux [<version>] [<build>]
cd "$(dirname "$0")/.." || exit 1
set -e
DIR=${1:?usage: tools/release_readme.sh <folder> windows|macos|linux}
OS=${2:?usage: tools/release_readme.sh <folder> windows|macos|linux}
case $OS in
  windows) START='double-click "Start ACNHRecomp.bat" (or "ACNHRecomp Launcher.exe")'
           KEEP='"Add to desktop" on the launcher'"'"'s home page puts a shortcut with the icon on the desktop and in
   the Start menu.'
           NEEDS='Windows 10 or 11 (x64) and a GPU with a Vulkan 1.3 driver (NVIDIA, AMD or Intel; keep it up to date).'
           LIBS='SDL3 (zlib licence); the Microsoft Visual C++ runtime (vcruntime140, msvcp140: redistributable);
   AMD FidelityFX (amd_fidelityfx_*_vk.dll, MIT) and NVIDIA DLSS (nvngx_dlss.dll, NVIDIA DLSS SDK licence): see licenses\.'
           EOL=$'\r\n' ;;
  macos)   START='double-click "Start ACNHRecomp.command" the first time (it lets macOS open ACNHRecomp.app, which
   is not signed by Apple, and opens it). If macOS still refuses: right-click ACNHRecomp.app, choose Open, then
   Open again (or System Settings > Privacy & Security > Open Anyway).'
           KEEP='"Install in Applications" on the launcher'"'"'s home page copies ACNHRecomp.app into Applications and
   puts it on the desktop: from then on open it from Launchpad, Spotlight or the Dock.'
           NEEDS='a Mac with Apple Silicon (M1 or newer) and macOS 15 or newer.'
           LIBS='SDL3 (zlib licence), the Vulkan loader and MoltenVK (Apache 2.0), libvpx, libvorbis and libogg (BSD).'
           EOL=$'\n' ;;
  linux)   START='run "./Start ACNHRecomp.sh" (or open it from your file manager).'
           KEEP='"Add to desktop" on the launcher'"'"'s home page adds ACNHRecomp, with its icon, to the applications
   menu and to the desktop (keep this folder where it is: the shortcut points into it).'
           NEEDS='a 64-bit x86 PC (a CPU with AVX2: Intel Haswell / AMD Zen or newer), glibc 2.35 or newer (Ubuntu 22.04,
   Fedora 36, Debian 12 and newer) and a Vulkan 1.3 driver (Mesa, or NVIDIA'"'"'s): sudo apt install mesa-vulkan-drivers'
           LIBS='SDL3 (zlib licence), libvpx, libvorbis and libogg (BSD). The Vulkan loader and driver are the system'"'"'s.'
           EOL=$'\n' ;;
  *) echo "unknown system: $OS (windows, macos or linux)"; exit 1 ;;
esac
VERSION=${3:-$(git describe --tags --always 2>/dev/null || echo dev)}

text() {
cat <<EOF
ACNHRecomp $VERSION
$(printf '%*s' $((11 + ${#VERSION})) '' | tr ' ' '=')

A native PC port of Animal Crossing: New Horizons made by static recompilation. This package contains NO part of
the game: you need your own copy, with update 3.0.3, whose ExeFS and RomFS you extract yourself with your emulator.

This is a work in progress made by fans. Expect bugs: graphical glitches, crashes, sound or input issues. Back up
your saves (the "user" folder inside your game folder) before playing.

1. Extract your game files
   Install update 3.0.3 in your emulator, then extract the ExeFS and the RomFS of your own copy: Ryujinx,
   right-click the game > Extract Data > ExeFS, and again > RomFS; yuzu or Eden, right-click > Dump RomFS (Full),
   with Dump ExeFS turned on in the debugging settings. ACNHRecomp never reads NSP, XCI or NCA files and never uses
   keys.

2. Start ACNHRecomp
   Extract this package anywhere, whole (its tools prepare the game), then
   $START

3. Prepare your game
   In the launcher, choose "Prepare the game from your extracted files", pick what you extracted and where the
   game folder goes (about 11 GB), and wait: it recompiles the game's code and compiles it for this computer, once.
   Then Play. Next time, the launcher remembers the folder (its home page: Play, Settings). A folder prepared
   before, by ACNHRecomp or by ACNHRecomp-Importer, can be chosen as it is.

4. Keep it handy
   $KEEP

Brazilian Portuguese and 60 FPS: the first start downloads the pt-BR fan translation and the files of the 60 FPS
SpeedFix into your game folder (a few seconds, checked before use). Portuguese is then in the game's languages,
and the game runs at 60 FPS at its normal speed. Without internet it starts normally and tries again the next day.

New versions: the launcher looks for one when it opens and says so on its home page, with a button to download
it (Settings: "Look for new versions" turns it off). The version in use is in the launcher's Settings and in the
game's settings (About).

Needs: $NEEDS

Settings: in the launcher (Settings), or in the game with F1 (on a controller: Minus + Plus). Version 3.0.2 or
3.0.3 of the game gets every fix of the port.

Problems, questions, saves: our Discord (https://discord.gg/VJW9Dtdu7a) or the issues of
https://github.com/Nycolazs/ACNHRecomp. A report made in the game's settings (About > Report a problem) helps a lot.

Not affiliated with or endorsed by Nintendo. Animal Crossing is a trademark of Nintendo. Only play a copy you own.

Libraries beside the executables (THIRD-PARTY.txt): $LIBS
EOF
}
text | sed "s/\$/${EOL%$'\n'}/" > "$DIR/README.txt"
cp LICENSE "$DIR/LICENSE.txt"
cp third_party/capstone/LICENSE.TXT "$DIR/LICENSE-capstone.txt"
cp third_party/capstone/LICENSE_LLVM.TXT "$DIR/LICENSE-capstone-llvm.txt"
cp prepare/recomp/acnh/SPEEDFIX.md "$DIR/SPEEDFIX.txt"
if [ -n "$4" ]; then
  cp "$4/_deps/mbedtls-src/LICENSE" "$DIR/LICENSE-mbedtls.txt"
fi
{
  echo "ACNHRecomp release packages carry these libraries, unmodified, under their own licences:"
  echo
  echo "$LIBS"
  echo
  echo "SDL3: https://github.com/libsdl-org/SDL (zlib). Vulkan loader: https://github.com/KhronosGroup/Vulkan-Loader"
  echo "(Apache 2.0). MoltenVK: https://github.com/KhronosGroup/MoltenVK (Apache 2.0). libvpx: https://chromium.googlesource.com/webm/libvpx"
  echo "(BSD 3-clause). libvorbis and libogg: https://xiph.org (BSD 3-clause). Dear ImGui (in the launcher and the"
  echo "settings screen): https://github.com/ocornut/imgui (MIT). Mbed TLS (linked into acnh, for Nextendo Online):"
  echo "https://github.com/Mbed-TLS/mbedtls (Apache 2.0, text in LICENSE-mbedtls.txt)."
  echo
  echo "Built into the launcher and acnh_prepare (the preparation of the game folder): Capstone 5.0.7, which the"
  echo "recompiler disassembles the game's code with (https://www.capstone-engine.org, BSD 3-clause, with LLVM's licence"
  echo "for its tables: LICENSE-capstone.txt, LICENSE-capstone-llvm.txt); and tmouh's \"ACNH 60 FPS + SpeedFix\" patches of"
  echo "main for 3.0.2 and 3.0.3 (https://github.com/tmouh/acnh-60fps), unchanged, by tmouh, licensed CC BY-NC 4.0"
  echo "(https://creativecommons.org/licenses/by-nc/4.0/; SPEEDFIX.txt), with which the preparation recompiles main for"
  echo "the native 60 FPS."
  echo
  echo "The preparation's tools (tools/): zig 0.17.0 (https://ziglang.org, MIT; trimmed, its own licence and those of"
  echo "the C libraries' headers it carries are in tools/zig), which compiles the game's code; and the shader converter"
  echo "(tools/shaders/spirv_harness): Ryujinx's shader translator (Ryubing, https://codeberg.org/Software_arch/Ryubing, MIT) with"
  echo "this project's extraction of the game's shaders, compiled ahead of time with .NET (https://dot.net, MIT)."
} | sed "s/\$/${EOL%$'\n'}/" > "$DIR/THIRD-PARTY.txt"
