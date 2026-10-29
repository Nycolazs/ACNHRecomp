#!/bin/bash
# The text files of a release package (tools/package_release_*.sh), in <folder>: README.txt (how to play, for that
# system), LICENSE (this project's) and THIRD-PARTY.txt (the libraries beside the executables).
# usage: tools/release_readme.sh <folder> windows|macos|linux
cd "$(dirname "$0")/.." || exit 1
set -e
DIR=${1:?usage: tools/release_readme.sh <folder> windows|macos|linux}
OS=${2:?usage: tools/release_readme.sh <folder> windows|macos|linux}
case $OS in
  windows) START='double-click "Start ACNHRecomp.bat" (or "ACNHRecomp Launcher.exe")'
           NEEDS='Windows 10 or 11 (x64) and a GPU with a Vulkan 1.3 driver (NVIDIA, AMD or Intel; keep it up to date).'
           LIBS='SDL3 (zlib licence); the Microsoft Visual C++ runtime (vcruntime140, msvcp140: redistributable).'
           EOL=$'\r\n' ;;
  macos)   START='double-click "Start ACNHRecomp.command". The first time macOS may refuse to open it: right-click it,
   choose Open, then Open again.'
           NEEDS='a Mac with Apple Silicon (M1 or newer) and macOS 14 or newer.'
           LIBS='SDL3 (zlib licence), the Vulkan loader and MoltenVK (Apache 2.0), libvpx, libvorbis and libogg (BSD).'
           EOL=$'\n' ;;
  linux)   START='run "./Start ACNHRecomp.sh" (or open it from your file manager).'
           NEEDS='a 64-bit x86 PC (a CPU with AVX2: Intel Haswell / AMD Zen or newer), glibc 2.35 or newer (Ubuntu 22.04,
   Fedora 36, Debian 12 and newer) and a Vulkan 1.3 driver (Mesa, or NVIDIA'"'"'s): sudo apt install mesa-vulkan-drivers'
           LIBS='SDL3 (zlib licence), libvpx, libvorbis and libogg (BSD). The Vulkan loader and driver are the system'"'"'s.'
           EOL=$'\n' ;;
  *) echo "unknown system: $OS (windows, macos or linux)"; exit 1 ;;
esac
VERSION=$(git describe --tags --always 2>/dev/null || echo dev)

text() {
cat <<EOF
ACNHRecomp ($VERSION)
=====================

A native PC port of Animal Crossing: New Horizons made by static recompilation. This package contains NO part of
the game: you need your own copy, prepared with ACNHRecomp-Importer.

This is a work in progress made by fans. Expect bugs: graphical glitches, crashes, sound or input issues. Back up
your saves (the "user" folder inside your game folder) before playing.

1. Get ACNHRecomp-Importer
   ACNHRecomp-Importer is a separate companion app that turns your own copy of the game (NSP or XCI, its update and
   your console's keys) into the game folder ACNHRecomp plays. It is available on our Discord:

       https://discord.gg/VJW9Dtdu7a

   Run it on your copy and wait until it finishes. ACNHRecomp only accepts folders prepared by it.

2. Start ACNHRecomp
   Extract this package anywhere, then $START

3. Choose your game folder
   The launcher asks for one thing: the folder ACNHRecomp-Importer wrote. Choose it and the game starts. Next
   time, the launcher remembers it (its home page: Play, Settings).

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
{
  echo "ACNHRecomp release packages carry these libraries, unmodified, under their own licences:"
  echo
  echo "$LIBS"
  echo
  echo "SDL3: https://github.com/libsdl-org/SDL (zlib). Vulkan loader: https://github.com/KhronosGroup/Vulkan-Loader"
  echo "(Apache 2.0). MoltenVK: https://github.com/KhronosGroup/MoltenVK (Apache 2.0). libvpx: https://chromium.googlesource.com/webm/libvpx"
  echo "(BSD 3-clause). libvorbis and libogg: https://xiph.org (BSD 3-clause). Dear ImGui (in the launcher and the"
  echo "settings screen): https://github.com/ocornut/imgui (MIT)."
} | sed "s/\$/${EOL%$'\n'}/" > "$DIR/THIRD-PARTY.txt"
