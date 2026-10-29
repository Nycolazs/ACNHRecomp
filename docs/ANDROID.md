# Android

ACNHRecomp runs on Android phones with a Snapdragon (Adreno 6xx or newer), through Turnip (Mesa's Vulkan driver
for Adreno, carried inside the app). It was brought up and tuned on a Galaxy S20 FE (Snapdragon 865, Adreno 650):
on the island the game holds a steady 30 frames a second at 900 rendered rows, upscaled with AMD FSR 1 to the
2400x1080 screen (the full 20:9 screen, under the camera cutout), with the battery under ~41 °C after 20 minutes.

The app holds **nothing of the game**: it is the runtime only (a few MB), the same for everyone, and the releases
of this repository carry it ready to install. The game itself comes from **your own copy**: your computer
recompiles it (as for the desktop versions) and prepares a *game folder* for the phone: the recompiled code as
`libacnh_game.so`, the RomFS as `romfs.pak`, the shaders as `spirv.pak`, fonts and the translation. When the app
opens it asks for that folder, checks it and starts the game; next time it starts right away. That folder (and the
`libacnh_game.so` in it) must not be shared.

## With ACNHRecomp-Importer (the release)

1. Prepare your game folder with **ACNHRecomp-Importer** (on our Discord:
   [discord.gg/VJW9Dtdu7a](https://discord.gg/VJW9Dtdu7a)), for Android: its folder then has the game's code for the
   phone in `game/android-arm64/`.
2. Copy that folder to the phone (e.g. to `ACNH` on the memory card).
3. Install `ACNHRecomp-<version>-android.apk` from the
   [releases](https://github.com/Nycolazs/ACNHRecomp/releases), open **ACNH Recomp**, choose *Choose the game folder*
   and pick it (the app asks once for access to all files). The game starts; next time it starts right away.

The rest of this page builds the app and the game folder from this repository instead.

## What you need

- A computer with the game already set up by `tools/setup_acnh.py` (README, "Installation"): macOS (Apple
  Silicon) or Linux x86-64. Its `private/acnh` folder (generated code, `spirv`, `fonts`, the RomFS link) is what the
  phone build uses.
- The Android SDK with `ndk;28.2.13676358`, `build-tools;35.0.0` and `platforms;android-35` (Android Studio's SDK
  Manager, or `sdkmanager "ndk;28.2.13676358" "build-tools;35.0.0" "platforms;android-35"`), and a JDK 17
  (`brew install openjdk@17` on macOS). The SDK is looked for in `ANDROID_SDK_ROOT`, else `~/Library/Android/sdk`
  (macOS) or `~/Android/Sdk` (Linux).
- Turnip: `tools/build_turnip.sh` builds it from Mesa's sources (needs Homebrew's `bison`, `flex` and Python 3.10+),
  or take `libvulkan_freedreno.so` from a release of this repository and put it in
  `private/third_party/mesa/build-android/src/freedreno/vulkan/`.
- The phone: Android 10 or newer, USB debugging on (Settings > About > tap the build number 7 times, then
  Developer options > USB debugging), connected by USB, `adb devices` listing it. About 11 GB free on the memory card
  or the phone's storage.

## Prepare the game folder, install the app

All in one (checks what is missing and says how to get it):

```sh
tools/android_install.sh --save      # --save: also your computer's save (the phone's own is kept beside it)
```

It builds the app and `libacnh_game.so` (the first build compiles the whole game: 30-60 min), installs the app and
copies the game folder to the phone: the app's folder on the memory card when there is one
(`Android/data/io.github.acnhrecomp/files`), else the phone's own storage. The copy can be interrupted and run
again: it resumes, and later runs re-send only what changed (the phone's own settings are kept).

Or step by step:

```sh
tools/build_android.sh --install     # the app (APK) and private/acnh/apk/libacnh_game.so
tools/push_android.sh                # the game folder to the phone (~10 GB)
```

`tools/build_android.sh --no-game` builds only the app, without any game set up on the computer (what a release
carries).

### Opening the app

The first time, **ACNH Recomp** shows its home screen:

- *Use the game found*: the folder `android_install.sh` copied into the app's own folder.
- *Choose the game folder*: any other folder with the prepared game (e.g. one copied by USB to `ACNH` on the memory
  card). The app then asks for access to all files once: Android needs it to read a folder outside the app's own.

The game starts as soon as the folder checks out (the first time a few seconds more: `libacnh_game.so` is copied
into the app). To pick another folder later: long press on the app icon > *Change game folder*.

If the app was updated and the folder was prepared by an older version, the home screen says so: run
`tools/android_install.sh` again (it rebuilds `libacnh_game.so` for the new app).

The pt-BR translation: install it on the computer as for the desktop version (the setup installs a
`*ptbr*.zip` found in the game folder or `private/translations/`); `push_android.sh` sends `mods/` with it.

### Your save

To continue the island you play on the computer, copy its save to the phone (with the game closed on both):

```sh
D=/storage/<card>/Android/data/io.github.acnhrecomp/files   # adb shell ls /storage to find <card>
adb push private/acnh/user/device_save "$D/user/"
adb push private/acnh/users.txt "$D/"
```

Saves are written to `user/device_save` on the phone; copy it back the same way with `adb pull`.

## Playing

Open **ACNH Recomp** on the phone. The first start takes a minute or two: shaders are prepared once, then kept.

- **Touch controls**: the left half of the screen is the stick; A, B, X, Y at the bottom right; L/ZL, R/ZR in the
  top corners; - and + at the top centre with the settings gear between them; a small d-pad bottom left.
- **A controller** (Bluetooth or USB) hides the touch controls by itself while it is connected. The settings can
  also hide them for good.
- **Settings**: the gear at the top centre (or Minus + Plus on a controller); drag a finger over a page to scroll
  it. Language (pt-BR included), aspect, resolution, upscaling, frame generation, frame rate. Resolution, aspect,
  upscaling (FSR or bilinear), its sharpness, anti-aliasing, frame generation, the FPS counter and the thermal mode
  apply at once; a setting that only applies when the game starts again says so under it, and *Restart now* at the
  bottom starts the game again. The FPS counter shows the game's own frames a second (and, with frame generation,
  the pictures shown). On a phone they show no keyboard hints.

### Settings that matter on a phone

| Setting | Default on Android | Notes |
|---|---|---|
| Frame generation | Off | built-in: the game's 30 fps shown at 60, ~4 ms more GPU a frame, warmer, and what moves past something else smears every other picture (no motion vectors from the game) |
| Resolution | Auto | the rows the GPU keeps at 30 fps (900 on an Adreno 650) |
| Upscaling | AMD FSR 1 | from those rows to the screen; bilinear is a little cheaper and softer |
| Aspect | Auto | the phone's own shape (20:9), HUD at the edges |
| Prepare shaders while loading / async shaders | on | new objects can show a few frames late instead of freezing the game |
| Thermal mode | Balanced | see below |
| Clock control (root) | off | rooted phones only, see below |

### Samsung phones: the GPU clock cap

Samsung's system manager caps the GPU (441 of 587 MHz on the S20 FE) and the CPU, even with the phone cold. The game
then runs at ~25 fps instead of 30. On a **rooted** phone, *Clock control (root)* in the settings lifts the GPU cap
while the game runs (the app asks Magisk for root once: allow it). The CPU caps are never raised: the game does not get
faster with them, only warmer. A phone that gets hot (75-80 °C on the S20 FE after minutes of play) throttles by
itself; a cooler helps.

### Thermal mode

*Thermal mode* (`video.thermal`; `NX_THERMAL=performance|balanced|cool` in `config.txt` wins over it, for tests) sets
how warm the phone may get (runtime/src/thermal.cpp):

- On every phone, the system's performance hints (ADPF) get the game's real frame period as the target (33.3 ms at
  30 fps, less a margin: 15% balanced, 5% cool) instead of 16.7 ms, which kept the clocks up for nothing.
- With *Clock control (root)*, every 2 s the GPU clock cap and the big cores' cap go to the lowest steps that hold the
  frame time (balanced: the GPU at most 85% busy, the CPU work at most 80% of the frame; cool: 96% and 88%, and a few
  late frames allowed). Late frames raise them at once. Hot (Android's thermal headroom or the CPU at 75 °C balanced,
  68 °C cool) lowers the ceilings a step (cool: two) before Samsung's own throttling. The log says why
  (`thermal:` lines), and a status line every 30 s.
- *Performance* is the behaviour before: with root, the GPU's top clock; the CPU as the system sets it.

Every cap written is put back when the game goes to the background and when it ends (the root shell restores them
when the app's end of its socket closes: also after a crash or a kill).

## Measured (Galaxy S20 FE, island)

20 minutes of walking, menus and talking at the game's noon, 900 rows + FSR 1, frame generation off (2026-10-28):
30.3 game frames a second on average, no two-second window under 30, 7 hitches; the battery at most 40.3 °C, the
CPU 72-78 °C. With frame generation on: 30.4 (60 shown), 122 hitches, battery 41.7 °C, CPU 78-86 °C. At 1080 rows
the GPU is at its limit once Samsung's thermal manager caps it to 400 MHz (27-30 fps when the room is warm).

Earlier (900 rows, frame generation):

| | Before (2026-10-26) | Now |
|---|---|---|
| Game frames a second | 23-25, frames 36-50 ms apart | 29.7-30.0, 31-36 ms apart |
| Shown | ~40-48 | ~60 |
| Hitches over 10 minutes standing | many | 0 |

These are at night in the game (it follows the phone's clock). In daylight, with the sun's shadows, the same spot
runs at 26-28 game frames a second: the replay (the NVN commands translated to Vulkan, on one core) takes 31-34 ms a
frame there, and it gets slower the busier the GPU is, as both share the memory (28.5 ms with frame generation off;
the floors of the memory bus and L3 clocks raised made no difference). The phone reaches 78-83 °C within minutes.
Native 1080 rows run at ~26 game frames a second on the Adreno 650 (the GPU is the limit there).
Benchmarks: `NX_TIME_OFFSET=<seconds>` in `config.txt` moves the game's clock, for the same light every run.

## Troubleshooting

- **Black screen or the app closes at start**: `adb shell cat <data folder>/nx.log | tail -50` and open an issue
  with it. `adb logcat -s acnh` shows the same log live.
- **The home screen says the folder has no prepared game**: it lacks `libacnh_game.so` or `analysis/main.bin`: run
  `tools/push_android.sh` again, or pick the folder that has them.
- **Settings for testing** go in `config.txt` in the data folder, one `NX_...=value` per line (they win over the
  settings screen). E.g. `NX_RENDER_HEIGHT=1080` for native 1080 rows.
