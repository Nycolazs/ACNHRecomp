#!/bin/bash
# The Windows release package, made on macOS or Linux with mingw-w64: the sources the setup needs at its root
# (tools/release_sources.sh: nothing of the game) with the launcher already built beside them (cross-compiled), SDL3.dll and
# libwinpthread-1.dll beside it, and "Start ACNHRecomp.bat" to double-click. The launcher then checks the computer and
# sets the game up from the player's own files with tools/setup_acnh.py (Python and Visual Studio's C++ build tools,
# README "Requirements"), as on the other systems.
# usage: tools/package_release_win.sh <version> <SDL3 mingw prefix>
#   <SDL3 mingw prefix>: the x86_64-w64-mingw32 folder of SDL's SDL3-devel-<x.y.z>-mingw.tar.gz release
#   (https://github.com/libsdl-org/SDL/releases)        -> private/release/ACNHRecomp-<version>-windows-x64.zip
# needs: x86_64-w64-mingw32-g++ (brew install mingw-w64, or apt install g++-mingw-w64-x86-64), cmake, ninja
cd "$(dirname "$0")/.." || exit 1
set -e
VER=${1:?usage: tools/package_release_win.sh <version> <SDL3 mingw prefix>}
SDL=${2:?usage: tools/package_release_win.sh <version> <SDL3 mingw prefix>}
[ -f "$SDL/bin/SDL3.dll" ] || { echo "$SDL/bin/SDL3.dll not found"; exit 1; }
git diff --quiet HEAD -- || { echo "uncommitted changes: commit first (the package is made from HEAD)"; exit 1; }
BUILD=private/release/launcher-win
OUT=private/release/ACNHRecomp-$VER-windows-x64
rm -rf "${OUT:?}" "$OUT.zip"
mkdir -p "$OUT"

echo "== launcher (mingw-w64)"
TC=$PWD/$BUILD/mingw.cmake
mkdir -p "$BUILD"
cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH $SDL)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")
EOF
[ -f "$BUILD/CMakeCache.txt" ] ||
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release -DNX_GAME=acnh \
        -DNX_GEN_DIR="$PWD/$BUILD/no-gen" -DCMAKE_PREFIX_PATH="$SDL" >/dev/null
cmake --build "$BUILD" --target acnhrecomp_launcher

echo "== sources (tools/release_sources.sh)"
R=$OUT
tools/release_sources.sh "$R" windows
cp "$BUILD/acnhrecomp_launcher.exe" "$R/ACNHRecomp Launcher.exe"
x86_64-w64-mingw32-strip "$R/ACNHRecomp Launcher.exe"
cp "$SDL/bin/SDL3.dll" "$R/"
# The one runtime DLL mingw's libstdc++ threads still import (zlib-style licence, mingw-w64).
WINPTHREAD=$(x86_64-w64-mingw32-g++ -print-file-name=libwinpthread-1.dll)
[ -f "$WINPTHREAD" ] || WINPTHREAD=$(dirname "$(command -v x86_64-w64-mingw32-g++)")/../x86_64-w64-mingw32/bin/libwinpthread-1.dll
[ -f "$WINPTHREAD" ] || WINPTHREAD=$(find -L "$(brew --prefix mingw-w64 2>/dev/null || echo /usr)/" -name libwinpthread-1.dll -path "*x86_64*" 2>/dev/null | head -1)
cp "$WINPTHREAD" "$R/"
printf '@echo off\r\nrem Double-click: opens the launcher, which sets the game up from your own files (README, "Installation").\r\ncd /d "%%~dp0"\r\nstart "" "ACNHRecomp Launcher.exe"\r\n' > "$R/Start ACNHRecomp.bat"
(cd "$OUT" && zip -qr "../$(basename "$OUT").zip" .)
rm -rf "${OUT:?}"
ls -la "$OUT.zip"
