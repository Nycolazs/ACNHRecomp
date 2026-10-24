#!/bin/bash
# Android (arm64-v8a): builds lib<game>.so with the NDK, packs it into an APK (NativeActivity, no Java), signs it
# with a local debug key and, with --install, installs it on the phone connected by adb.
# The game data goes to the phone with tools/push_android.sh.
#
# usage: tools/build_android.sh [--install] [--jobs N]
#   the Vulkan headers: private/third_party/Vulkan-Headers-1.4.357 (tools/setup_acnh.sh; the NDK's are older than
#   what the runtime calls)
#   needs: Android SDK with ndk;28.2.13676358, build-tools;35.0.0, platforms;android-35 (ANDROID_SDK_ROOT, default
#   ~/Library/Android/sdk) and a JDK (JAVA_HOME, default Homebrew's openjdk@17)
cd "$(dirname "$0")/.." || exit 1
set -e
GAME=acnh
SDK=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}
NDK=$SDK/ndk/28.2.13676358
BT=$SDK/build-tools/35.0.0
PLATFORM=$SDK/platforms/android-35/android.jar
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}
export PATH=$JAVA_HOME/bin:$PATH
BUILD=private/$GAME/build-android
OUT=private/$GAME/apk
INSTALL=0
JOBS=4
while [ $# -gt 0 ]; do
  case $1 in
    --install) INSTALL=1 ;;
    --jobs) JOBS=$2; shift ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac
  shift
done
[ -d "$NDK" ] || { echo "NDK not found: $NDK"; exit 1; }

echo "== configure ($BUILD)"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 -DNX_GAME=$GAME -DNX_GEN_DIR="$PWD/private/$GAME/gen" -DNX_LAUNCHER=OFF \
  -DNX_VULKAN_SDK="$PWD/private/third_party/Vulkan-Headers-1.4.357" > "$OUT.cmake.log" 2>&1 ||
  { tail -20 "$OUT.cmake.log"; exit 1; }

echo "== build lib$GAME.so (the first time compiles the whole game: ~30 min)"
until mkdir private/build.lock 2>/dev/null; do echo "waiting for another build"; sleep 10; done
trap 'rmdir private/build.lock 2>/dev/null' EXIT
ninja -C "$BUILD" -j "$JOBS" $GAME
rmdir private/build.lock; trap - EXIT

echo "== package"
rm -rf "$OUT"; mkdir -p "$OUT/lib/arm64-v8a"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-unneeded -o "$OUT/lib/arm64-v8a/lib$GAME.so" \
  "$BUILD/lib$GAME.so"
# libadrenotools' hook libraries, and Turnip when it was built (tools/build_turnip.sh): the Adreno driver it loads.
cp $(find "$BUILD/_deps" -name "lib*hook*.so") "$OUT/lib/arm64-v8a/"
TURNIP=private/third_party/mesa/build-android/src/freedreno/vulkan/libvulkan_freedreno.so
[ -f "$TURNIP" ] && cp "$TURNIP" "$OUT/lib/arm64-v8a/" || echo "(no Turnip: tools/build_turnip.sh; the phone's driver is used)"
python3 tools/make_android_icon.py 2>/dev/null || true  # needs Pillow; the PNGs are kept in android/res otherwise
"$BT/aapt2" compile --dir android/res -o "$OUT/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" --manifest android/AndroidManifest.xml -I "$PLATFORM" "$OUT/res.zip"
(cd "$OUT" && zip -q -r base.apk lib)
"$BT/zipalign" -f -P 16 4 "$OUT/base.apk" "$OUT/aligned.apk"
KEY=private/android-debug.keystore
[ -f "$KEY" ] || keytool -genkeypair -keystore "$KEY" -storepass android -keypass android -alias debug \
  -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=ACNH Recomp debug" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KEY" --ks-pass pass:android --out "$OUT/$GAME.apk" "$OUT/aligned.apk"
ls -la "$OUT/$GAME.apk"

if [ $INSTALL = 1 ]; then
  echo "== install"
  adb install -r "$OUT/$GAME.apk"
fi
