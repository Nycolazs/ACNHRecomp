#!/bin/bash
# Turnip, Mesa's Vulkan driver for Adreno GPUs, built for Android from an official Mesa release (MIT). On Snapdragon
# phones the APK carries it and the runtime loads it through libadrenotools instead of the phone's own driver
# (Settings > Video > GPU driver): the Adreno 6xx drivers phones ship are Vulkan 1.1 without dynamic rendering and
# drew nothing of the game, Turnip is Vulkan 1.3 with the extensions the runtime uses.
#
# usage: tools/build_turnip.sh     -> private/third_party/mesa/build-android/src/freedreno/vulkan/libvulkan_freedreno.so
#   needs: the Android NDK (as tools/build_android.sh), Homebrew's bison and flex (macOS's are too old)
cd "$(dirname "$0")/.." || exit 1
set -e
MESA=private/third_party/mesa
TAG=mesa-25.3.6
export PATH=/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$PWD/private/venv/bin:$PATH
private/venv/bin/pip install -q meson mako pyyaml
NDK=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/ndk/28.2.13676358
TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
CROSS=$PWD/private/android_cross.ini
cat > "$CROSS" <<EOF2
[binaries]
ar = '$TC/llvm-ar'
c = ['$TC/aarch64-linux-android29-clang']
cpp = ['$TC/aarch64-linux-android29-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-static-libstdc++', '-Wno-c++11-narrowing']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$TC/llvm-strip'
pkg-config = ['/usr/bin/false']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF2
[ -d "$MESA/.git" ] || git clone -q --depth 1 --branch "$TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$MESA"
cd "$MESA"
[ -f build-android/build.ninja ] || meson setup build-android --cross-file "$CROSS" \
  -Dbuildtype=release -Dplatforms=android -Dplatform-sdk-version=29 -Dandroid-stub=true -Dgallium-drivers= \
  -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
  -Dglx=disabled -Dllvm=disabled -Dzstd=disabled -Dvalgrind=disabled -Db_lto=false -Dstrip=true
ninja -C build-android src/freedreno/vulkan/libvulkan_freedreno.so
ls -la build-android/src/freedreno/vulkan/libvulkan_freedreno.so
