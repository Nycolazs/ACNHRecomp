#!/bin/bash
# Android (arm64-v8a): builds the app (lib<game>.so: the runtime, no game code; Home.java: the screen that asks for the
# game folder) and, from the game set up on this computer, the game's recompiled code as lib<game>_game.so, which
# goes into the game folder on the phone with the data (tools/push_android.sh), never into the APK. Packs the APK,
# signs it with a local debug key and, with --install, installs it on the phone connected by adb.
#
# usage: tools/build_android.sh [--install] [--jobs N] [--no-game]
#   --no-game: the app only, without the game set up here (what a release carries)
#   the Vulkan headers: private/third_party/Vulkan-Headers-1.4.357 (tools/setup_acnh.sh; the NDK's are older than
#   what the runtime calls)
#   needs: Android SDK with ndk;28.2.13676358, build-tools;35.0.0, platforms;android-35 (ANDROID_SDK_ROOT, default
#   ~/Library/Android/sdk on macOS, ~/Android/Sdk on Linux) and a JDK 17 (JAVA_HOME; Homebrew's openjdk@17 found by
#   itself)
cd "$(dirname "$0")/.." || exit 1
set -e
GAME=acnh
# macOS: Android Studio's SDK folder; Linux: ~/Android/Sdk.
if [ "$(uname)" = Darwin ]; then HOST=darwin-x86_64; DEF_SDK=$HOME/Library/Android/sdk; else HOST=linux-x86_64; DEF_SDK=$HOME/Android/Sdk; fi
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$DEF_SDK}}
NDK=$SDK/ndk/28.2.13676358
BT=$SDK/build-tools/35.0.0
PLATFORM=$SDK/platforms/android-35/android.jar
if [ -z "${JAVA_HOME:-}" ] && [ -d /opt/homebrew/opt/openjdk@17 ]; then export JAVA_HOME=/opt/homebrew/opt/openjdk@17; fi
[ -n "${JAVA_HOME:-}" ] && export PATH=$JAVA_HOME/bin:$PATH
BUILD=private/$GAME/build-android
OUT=private/$GAME/apk
INSTALL=0
# Compile jobs: the cores, as many as the free memory holds (~700 MB each, as tools/setup_acnh.py counts them);
# --jobs or JOBS= to choose.
if [ -n "${JOBS:-}" ]; then :
elif [ -r /proc/meminfo ]; then
  FREE_MB=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo)
  CORES=$(nproc)
else  # macOS: half of the memory, as setup_acnh.py
  FREE_MB=$(( $(sysctl -n hw.memsize) / 2097152 ))
  CORES=$(sysctl -n hw.ncpu)
fi
if [ -z "${JOBS:-}" ]; then
  JOBS=$(( (FREE_MB - 1000) / 700 ))
  [ "$JOBS" -gt "$CORES" ] && JOBS=$CORES
  [ "$JOBS" -lt 1 ] && JOBS=1
fi
GAMELIB=1
while [ $# -gt 0 ]; do
  case $1 in
    --install) INSTALL=1 ;;
    --jobs) JOBS=$2; shift ;;
    --no-game) GAMELIB=0 ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac
  shift
done
[ -d "$NDK" ] || { echo "NDK not found: $NDK"; exit 1; }
GEN=$PWD/private/$GAME/gen
if [ $GAMELIB = 0 ]; then
  BUILD=private/$GAME/build-android-app
  OUT=private/$GAME/apk-app
  GEN=$PWD/$BUILD/no-gen
elif [ ! -d "$GEN/main" ]; then
  echo "the game is not set up on this computer (python3 tools/setup_acnh.py --game <folder>); --no-game: the app only"
  exit 1
fi
mkdir -p "$(dirname "$OUT")"

echo "== configure ($BUILD)"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_STL=c++_shared \
  -DANDROID_PLATFORM=android-29 -DNX_GAME=$GAME -DNX_GEN_DIR="$GEN" -DNX_LAUNCHER=OFF \
  -DNX_VULKAN_SDK="$PWD/private/third_party/Vulkan-Headers-1.4.357" > "$OUT.cmake.log" 2>&1 ||
  { tail -20 "$OUT.cmake.log"; exit 1; }

TARGETS=$GAME
# (NX_ANDROID_GAME_IN_APP=ON in the build's cache, for comparisons: no separate game library)
[ $GAMELIB = 1 ] && ninja -C "$BUILD" -t targets all | grep -q "^lib${GAME}_game.so:" && TARGETS="$GAME ${GAME}_game"
echo "== build $TARGETS with $JOBS jobs (the first time compiles the whole game: ~$(( 300 / JOBS + 5 )) min)"
until mkdir private/build.lock 2>/dev/null; do echo "waiting for another build"; sleep 10; done
trap 'rmdir private/build.lock 2>/dev/null' EXIT
ninja -C "$BUILD" -j "$JOBS" $TARGETS
rmdir private/build.lock; trap - EXIT

echo "== package"
STRIP=$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-strip
rm -rf "$OUT"; mkdir -p "$OUT/lib/arm64-v8a"
"$STRIP" --strip-unneeded -o "$OUT/lib/arm64-v8a/lib$GAME.so" "$BUILD/lib$GAME.so"
cp "$NDK/toolchains/llvm/prebuilt/$HOST/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$OUT/lib/arm64-v8a/"
# The game's code, for the game folder (push_android.sh): beside the APK, not in it.
[ "$TARGETS" != "$GAME" ] && "$STRIP" --strip-unneeded -o "$OUT/lib${GAME}_game.so" "$BUILD/lib${GAME}_game.so"
# Home.java: javac, then d8 to classes.dex.
mkdir -p "$OUT/classes"
javac -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath "$PLATFORM" -classpath "$BT/core-lambda-stubs.jar" \
  -d "$OUT/classes" $(find android/java -name "*.java")
"$BT/d8" --release --min-api 29 --lib "$PLATFORM" --output "$OUT" $(find "$OUT/classes" -name "*.class")
# libadrenotools' hook libraries, and Turnip when it was built (tools/build_turnip.sh): the Adreno driver it loads.
cp $(find "$BUILD/_deps" -name "lib*hook*.so") "$OUT/lib/arm64-v8a/"
TURNIP=private/third_party/mesa/build-android/src/freedreno/vulkan/libvulkan_freedreno.so
[ -f "$TURNIP" ] && cp "$TURNIP" "$OUT/lib/arm64-v8a/" || echo "(no Turnip: tools/build_turnip.sh; the phone's driver is used)"
python3 tools/make_android_icon.py 2>/dev/null || true  # needs Pillow; the PNGs are kept in android/res otherwise
"$BT/aapt2" compile --dir android/res -o "$OUT/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" --manifest android/AndroidManifest.xml -I "$PLATFORM" "$OUT/res.zip"
(cd "$OUT" && zip -q -r base.apk lib classes.dex)
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
