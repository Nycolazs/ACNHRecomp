# ACNHRecomp

**A native PC port of Animal Crossing: New Horizons, made by static recompilation.**

<p align="center">
  <a href="https://discord.gg/VJW9Dtdu7a">
    <img src="https://img.shields.io/badge/Discord-Join%20Community-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord Community">
  </a>
</p>

<p align="center">
  <img src="docs/screenshots/island-arrival.jpg" alt="Arriving on the island, running natively" width="100%">
</p>

ACNHRecomp turns the Nintendo Switch code of *Animal Crossing: New Horizons* **3.0.2** or **3.0.3** (title ID
`01006F8002326000`) into native code for your computer. It is not an emulator: the game's AArch64 machine code is
translated ahead of time into C++ and compiled for the host, the system services the game calls are reimplemented
natively, and the game's NVN graphics calls are translated to **Vulkan**. It is built on the framework of
[SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp).

> [!IMPORTANT]
> **No game files are included.** This repository contains no code, assets, keys or firmware from the game, and
> neither does anything the project shares. You need your own legally obtained copy of the game, its 3.0.2 or 3.0.3 update
> and your console's keys; the build is produced on your computer from your dump. Never distribute the resulting
> executable: it contains game code recompiled from your dump.

> [!NOTE]
> The detailed developer documentation ([docs/STATUS.md](docs/STATUS.md), [docs/REFERENCES.md](docs/REFERENCES.md))
> is written in Portuguese for now; [docs/ACNH.md](docs/ACNH.md) is in English.

## Community

[![Join our Discord](https://img.shields.io/badge/Discord-Join%20our%20Community%20Server-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/VJW9Dtdu7a)

> [!TIP]
> **Join our Discord Server!** Connect with fellow players, testers, and developers:
> [**discord.gg/VJW9Dtdu7a**](https://discord.gg/VJW9Dtdu7a)

Whether you're playing, helping test builds across different hardware, or following the project's progress, everyone is welcome!

- 🏝️ **Community & Discussion**: Hang out, share your island, and discuss Animal Crossing recompilation.
- 💬 **Help & Support**: Get quick guidance with key dumping, game imports, building, and troubleshooting.
- ⚙️ **Testing & Hardware**: Share logs, test results across different CPUs and GPUs, and report bugs.
- 📢 **Development Updates**: Get real-time announcements, test builds, and sneak peeks at upcoming features.

## Contents

- [Community](#community)
- [Features](#features)
- [Screenshots](#screenshots)
- [Status](#status)
- [Disclaimer](#disclaimer)
- [Requirements](#requirements)
- [Installation](#installation)
- [Playing](#playing)
- [Settings](#settings)
- [Controls](#controls)
- [Saves](#saves)
- [Languages and mods](#languages-and-mods)
- [Building from source](#building-from-source)
- [Troubleshooting and FAQ](#troubleshooting-and-faq)
- [Repository layout](#repository-layout)
- [How it works](#how-it-works)
- [Credits and references](#credits-and-references)
- [License](#license)

## Features

- **Native code, no emulation.** The `main`, `sdk` and `subsdk0` modules are recompiled to C++ and compiled with
  clang; game pointers are host pointers.
- **NVN to Vulkan.** The game's graphics API is implemented over Vulkan; its Maxwell GPU shaders are translated
  offline to SPIR-V with the Ryujinx (Ryubing) shader translator.
- **In-game settings screen** (F1, or Minus + Plus on a controller) styled after the game, with its menu in
  12 languages detected from the system.
- **Resolution and display options:** 1080p (TV mode) or 720p (handheld mode), AMD FSR 1 upscaling with adjustable
  sharpness, wider aspect ratios (16:10, 21:9, 32:9 and phone ratios), windowed or fullscreen.
- **Controllers:** any controller SDL3 recognises (Xbox, PlayStation, Switch Pro Controller / Joy-Con, generic
  pads), a Nintendo or Xbox/PlayStation confirm-button layout, and in-game button icons redrawn for Xbox,
  PlayStation or Switch. Fully remappable keyboard controls.
- **Save management:** import from Switch backups (JKSV, Checkpoint), from emulators (yuzu and its forks,
  Ryujinx/Ryubing) or from another ACNHRecomp; export in a format JKSV and Checkpoint can restore on a console;
  automatic backups.
- **All 14 game languages**, plus Brazilian Portuguese through a fan translation you install yourself, loaded as a
  RomFS mod (LayeredFS).
- **DLC** (add-on content) from your own NSPs.
- **Island clock** follows your computer's date, time and time zone, as on the console.
- **Performance work for modest hardware:** presets for integrated GPUs, an option to bypass the CPU-heavy audio
  reverb, pipelined GPU submits, AES-NI acceleration of the game's data decryption during loads, and a shader
  pre-compilation pass at startup.

## Screenshots

Captured from the recompiled game running on Linux (x86-64, Vulkan), at 1080p.

| | |
|:---:|:---:|
| ![The island from above](docs/screenshots/island-aerial.jpg) | ![Walking among the island's trees](docs/screenshots/island-trees.jpg) |
| *The island from the seaplane* | *Exploring the island* |
| ![Tom Nook's welcome at Nook Inc.](docs/screenshots/nook-inc.jpg) | ![Choosing a character style](docs/screenshots/character-style.jpg) |
| *Nook Inc.'s Deserted Island Getaway Package* | *Creating a resident* |
| ![Picking an island layout](docs/screenshots/island-choice.jpg) | ![Brazilian Portuguese fan translation](docs/screenshots/ptbr-dialogue.jpg) |
| *Picking an island layout* | *Brazilian Portuguese through a fan-translation mod* |

### Settings and launcher

| | |
|:---:|:---:|
| ![Settings screen, video page](docs/screenshots/settings-video.jpg) | ![Settings screen, controls page](docs/screenshots/settings-controls.jpg) |
| *In-game settings (F1): video* | *In-game settings: controls* |
| ![Launcher welcome page](docs/screenshots/launcher-welcome.jpg) | ![Launcher building the game](docs/screenshots/launcher-build.jpg) |
| *The launcher: first start* | *The launcher recompiling the game on your computer* |

## Status

The project is a work in progress. Everything below was measured on Linux x86-64 with an **Intel Iris Xe** (Mesa)
GPU; see [docs/STATUS.md](docs/STATUS.md) for the full, regularly updated log.

| Area | State |
|---|---|
| Import (base NSP + 3.0.2 or 3.0.3 update, BKTR patch applied) | Working: 103,400 RomFS files, 10.5 GB |
| CPU recompilation (`main`, `sdk`, `subsdk0`) | Working: about 19 million instructions, 0 unhandled |
| Shaders (Maxwell to SPIR-V) | Working: 6,200+ distinct blocks, 0 invalid |
| Boot, system services, audio, clock, input | Working |
| Opening: Nook Inc. counter scene in 3D, character creation | Working |
| Opening movie (VP9 + Vorbis), arrival on the island | Working |
| Free roaming on the island | Working, 30 fps on an Intel Iris Xe |
| First save (written at the end of the first day) | Not yet verified in a full playthrough |
| 60 fps | Not native yet (see below) |
| Online features (visits, Nook Link, Dream Suite) | Unavailable |

**Known issues**

- Some villagers (birds) may render with part of the head black. Starting villagers are random, so it does not
  always reproduce.
- **60 fps is not native yet.** The game's logic advances once per frame, so presenting at 60 fps makes the whole
  game run twice as fast. A real 60 fps mode requires halving the step of every frame-counting system and is not
  done yet. Play at 30 fps.
- The first save only happens at the end of the first in-game day, and that path has not been verified end to end
  in a full playthrough. Back up and be prepared for surprises.
- Online play is not available: the game sees a console with no internet connection.

**Platforms**

| Platform | State |
|---|---|
| Linux x86-64 | Tested (Ubuntu, clang 21, Intel Iris Xe / Mesa ANV) |
| macOS (Apple Silicon, MoltenVK) | Build and launch scripts exist; untested |
| Windows | Not supported yet |
| Android | Not supported yet |

## Disclaimer

ACNHRecomp is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Nintendo.
*Animal Crossing* is a trademark of Nintendo. The project does not provide, and will not help you obtain, game
files, keys or firmware. It only works with a copy of the game you dumped yourself from hardware you own.

A pre-commit hook ([tools/check_no_game_data.sh](tools/check_no_game_data.sh)) refuses commits that contain game
files, keys, shader binaries, recompiled game code or binary blobs. Everything derived from your dump stays in
`private/`, which is ignored by git.

This project was built with AI: nearly all new code, tools and documentation were written by Claude (Anthropic,
via Claude Code), directed and tested by a person. Its base, SuperMarioWonderRecomp, was also AI-assisted.

## Requirements

### Your game files

| File | Required |
|---|---|
| `prod.keys` dumped from your console | Yes |
| `title.keys` | Recommended |
| Base game NSP | Yes |
| Update 3.0.2 or 3.0.3 NSP | Yes |
| DLC NSPs (add-on content) | Optional |

### Computer

| | Linux (x86-64) | macOS (Apple Silicon, untested) |
|---|---|---|
| Compiler | clang 18 or newer (tested with 21) | Xcode command line tools / Homebrew LLVM |
| GPU | Vulkan 1.3 (tested: Intel Iris Xe, Mesa ANV) | MoltenVK |
| RAM | 16 GB (32 GB recommended to compile with more jobs) | 16 GB |
| Disk | About 30 GB free (10.5 GB RomFS, 1.6 GB generated C++, object files) | Same |

Ubuntu / Debian packages:

```sh
sudo apt install build-essential clang lld cmake ninja-build patch curl python3-venv spirv-tools pkg-config \
     libvulkan-dev libsdl3-dev libvpx-dev libvorbis-dev mesa-vulkan-drivers vulkan-tools
```

macOS (Homebrew):

```sh
brew install llvm cmake ninja sdl3 molten-vk vulkan-loader libvpx libvorbis
```

`libvpx` and `libvorbis` decode the opening movie (VP9 + Vorbis). Without them the game still runs, but the movie
is black.

The setup script downloads everything else into `private/third_party/`: a Python virtual environment (capstone,
lz4, zstandard, fonttools, cmake, ninja, unicorn), Vulkan-Headers 1.4.357, the .NET SDK 10.0.401, the Ryubing
shader translator source and Dear ImGui.

## Installation

### With the launcher (recommended)

The launcher is a small program, built from this repository alone, that does the whole setup with you: it asks
for your files, checks this computer, recompiles the game and then starts it. It uses the same look as the in-game
settings screen, follows the system language (12 languages, changeable at any time from the top right) and works
from small laptop screens to 4K.

```sh
git clone <this repository> ACNHRecomp && cd ACNHRecomp
tools/launcher.sh            # builds the launcher the first time (cmake, a C++ compiler and SDL3), then opens it
```

By hand: `cmake -S . -B build/launcher -DNX_GAME=acnh && cmake --build build/launcher --target acnhrecomp_launcher`,
then run `build/launcher/acnhrecomp_launcher`.

1. **Welcome.** Choose *Set up from my game files*, or *I already have a recompiled build* and pick its folder (an
   install folder made by the launcher, `private/acnh`, or this repository).
2. **Game files.** The keys (`prod.keys`, found automatically in `~/.switch` and the emulators' folders), the base
   game (NSP or XCI), update 3.0.2 or 3.0.3 (NSP), any DLC NSPs and, optionally, the pt-BR translation `.zip` (not
   distributed here). Choose them or drop them on the window. Each file is checked at once without decrypting
   anything: container type, and the title and version from the NSP's metadata, so a wrong game, the update in
   the base game's place or another update version is flagged before anything runs.
3. **Install.** Where the game goes (default `~/.local/share/ACNHRecomp`, `~/Library/Application Support/ACNHRecomp`
   on macOS), and a check of this computer: compiler, Python, curl/tar, SDL3 development files, Vulkan driver,
   movie libraries, disk space and memory, with the command that installs what is missing.
4. **Build.** Runs `tools/setup_acnh.sh --out <install folder>` and follows it: stages, progress (ninja's own
   counter during the compile, with an estimate of the time left), the full log, Stop. If a stage fails, the page
   says which, keeps the log open and offers *Copy log*, *Open logs folder* and *Try again* (finished stages are
   not redone).
5. **Play.** Starts the game with its data folder (`NX_DATA_ROOT`), and the launcher closes.

On later starts the launcher opens on a home page: **Play**, **Settings** (game language including pt-BR, display,
rendering, upscaling, frame rate; written to the game's `config.ini`), **Rebuild** (after updating the
repository) and **Change build folder**. Its own settings are in `launcher.ini` in the user's settings folder
(`~/.local/share/ACNHRecomp` on Linux).

Windows: the launcher builds and runs (`cmake --build ... --target acnhrecomp_launcher` with SDL3 available to
CMake), and can start a build made elsewhere, but the recompile pipeline is a bash script: run the setup on Linux,
macOS or inside WSL2, then choose that folder.

### From the command line

```sh
git clone <this repository> ACNHRecomp && cd ACNHRecomp
tools/setup_acnh.sh --keys ~/switch/prod.keys --title-keys ~/switch/title.keys \
                    --base ~/switch/base.nsp --update ~/switch/update.nsp \
                    --dlc ~/switch/dlc1.nsp --dlc ~/switch/dlc2.nsp
```

`--dlc` can be repeated, once per add-on NSP, or left out. To add a DLC later, run the same command again with the
`--dlc` options; the existing import is reused.

`--out <folder>` puts the game's files (import, generated code, build, logs) in that folder instead of
`private/acnh`; start the game from such a folder with `NX_DATA_ROOT=<folder> <folder>/build/acnh`.

The script runs in stages and skips every stage whose output already exists. If something fails, fix it and run
`tools/setup_acnh.sh` again (after the first import no arguments are needed).

| Stage | Output | Time (12 threads, 30 GB RAM) |
|---|---|---|
| Tools | `private/venv`, `private/third_party/*`, importer and shader harness | about 5 min |
| Import | `private/acnh/{analysis,exefs,extract/romfs}` | about 5 min |
| Recompile | `private/acnh/gen/{main,sdk,subsdk0}` (1.6 GB of C++) | about 8 min |
| Shaders | `private/acnh/spirv` | about 2 min |
| Fonts and button icons | `private/acnh/fonts` | under a minute |
| Build | `private/acnh/build/acnh` | 1 to 2 hours the first time |

| Variable | Effect |
|---|---|
| `JOBS=<n>` | Compiler jobs (default: RAM in GB divided by 3, at most the core count) |
| `FORCE=<stage>` | Redo one stage: `import`, `recomp`, `shaders`, `harness`, `glyphs` |

To check your dump: the importer prints `import complete: 103400 RomFS files, 10.50 GB`, and the recompiler ends
with `unhandled 0` for each of the three modules.

## Playing

**macOS, the simplest way:** double-click **`Jogar ACNH.command`** in the repository folder. It rebuilds whatever
changed in the runtime (a few seconds; nothing when up to date) and starts the game with your settings. To get the
same shortcut on the desktop, run once:

```sh
tools/install_mac_shortcut.sh
```

From a terminal:

```sh
tools/acnh_linux.sh              # quality: TV mode, 1080p
tools/acnh_linux.sh balanced     # 720p (handheld mode) upscaled with FSR 1
tools/acnh_linux.sh intel        # integrated GPU: 720p, pipelined submits, audio reverb off
tools/acnh_linux.sh lightest     # intel + FSR 1
tools/acnh_mac.sh                # macOS: quality | balanced (default) | performance
```

| Linux preset | Rendering | Upscaling | Audio reverb | Notes |
|---|---|---|---|---|
| `quality` (default) | 1080p | Bilinear | On | |
| `balanced` | 720p | FSR 1 | On | |
| `intel` | 720p | Bilinear | **Off** | Prefers the integrated GPU, pipelined submits |
| `lightest` | 720p | FSR 1 | **Off** | As `intel`, plus FSR 1 |

The environmental reverb (I3DL2) runs on the CPU, where the console has a DSP, and costs about one CPU core; the
light presets bypass it. It can also be turned off on any preset from the settings screen or with
`NX_AUDIO_REVERB=0`.

Useful environment variables (they take precedence over `config.ini`):

| Variable | Effect |
|---|---|
| `NX_FULLSCREEN=1`, `NX_RESOLUTION=WxH` | Fullscreen, window size (F11 or Alt+Enter toggles fullscreen) |
| `NX_HANDHELD=1` | Handheld mode (720p) |
| `NX_FSR=1` | FSR 1 upscaling |
| `NX_GPU=integrated` / `discrete` | GPU choice |
| `NX_AUDIO_REVERB=0` | Bypass the audio reverb |
| `NX_MUTE=1` | No sound |
| `NX_LOG=<file>` | Runtime log (default `private/acnh/nx_play.log`) |
| `NX_MEM_LIMIT_GB=<n>` | Stop the process above this memory use (Linux preset default: 10) |
| `NX_HEADLESS=1` | No window (automated tests) |

Debugging switches are listed in [docs/STATUS.md](docs/STATUS.md) and
[docs/upstream/nvn/SWITCHES.md](docs/upstream/nvn/SWITCHES.md).

## Settings

Open the settings screen at any time with **F1** on the keyboard or **Minus + Plus** together on a controller. It
is styled after the game, scales from laptop screens to 4K (with a vertical layout on narrow windows), and its menu
language follows the system: English, Brazilian Portuguese, Spanish, French, German, Italian, Dutch, Russian,
Japanese, Korean, Simplified Chinese and Traditional Chinese.

| Page | Options |
|---|---|
| Game | Game language (automatic from the system, the game's 14 languages, Brazilian Portuguese fan translation); menu language |
| Video | Quality preset (quality, balanced, low), windowed / fullscreen, window size, game mode (1080p TV mode or 720p handheld mode), internal resolution (540p to 2160p/4K, or the window's), upscaling (bilinear or FSR 1, with sharpness), aspect ratio (16:9, automatic from the window, 16:10, 4:3, 21:9, 32:9, phone ratios; applied live), frame rate, frame generation (built-in, or lsfg-vk if installed), GPU (automatic, dedicated, integrated), prepare shaders at startup |
| Audio | Master volume, environmental reverb, mute when the window is in the background |
| Controls | Connected controller, confirm/cancel layout, button icons, vibration, stick dead zone, keyboard remapping |
| Saves | Open the save folder, back up now, import, export, saves found on this computer, restore backups |
| About | Version, credits, license |

Changes are written to **`private/acnh/config.ini`**, which can also be edited by hand. Settings that need a
restart are marked on the screen. Environment variables set on the command line override the file.

| `config.ini` key | Values | Runtime switch |
|---|---|---|
| `game.language` | `auto`, `en-US`, `en-GB`, `es`, `es-419`, `fr`, `fr-CA`, `de`, `it`, `nl`, `ru`, `ja`, `ko`, `zh-CN`, `zh-TW`, `pt-BR` | `NX_LANGUAGE` |
| `game.mods` | Comma-separated mod names | `NX_MODS` |
| `ui.language` | `auto` or a menu language code | |
| `video.preset` | `custom`, `quality`, `balanced`, `low` (see below) | |
| `video.fullscreen` | `0`, `1` | `NX_FULLSCREEN` |
| `video.show_fps` | `0`, `1` | `NX_SHOW_FPS` |
| `video.window` | `auto` or `WxH` | `NX_RESOLUTION` |
| `video.mode` | `docked`, `handheld` | `NX_HANDHELD` |
| `video.upscaler` | `linear`, `fsr` | `NX_FSR` |
| `video.fsr_sharpness_pct` | `0`-`100` | `NX_FSR_SHARP` |
| `video.resolution` | `native`, `540`, `720`, `900`, `1080`, `1440`, `2160`, `window` | `NX_RENDER_HEIGHT` |
| `video.aspect` | `16:9`, `auto`, `16:10`, `4:3`, `21:9`, `32:9`, phone ratios, any `W:H` | `NX_ULTRAWIDE` |
| `video.gpu` | `auto`, `discrete`, `integrated` | `NX_GPU` |
| `video.frame_rate` | `30`, `60` | `NX_PRESENT_INTERVAL` |
| `video.frame_gen` | `off`, `builtin`, `lsfg` | `NX_FRAMEGEN` (`builtin`) |
| `video.shader_prepare` | `0`, `1` | |
| `audio.volume` | `0`-`100` | |
| `audio.reverb` | `0`, `1` | `NX_AUDIO_REVERB` |
| `audio.mute_background` | `0`, `1` | |
| `input.layout` | `nintendo`, `xbox` | |
| `controls.glyphs` | `auto`, `switch`, `xbox`, `playstation` | |
| `input.rumble` | `0`, `1` | |
| `input.deadzone` | `5`-`50` (percent) | |
| `input.key.<action>` | An SDL key name | |

`video.preset` sets the most expensive settings together. Chosen on the settings screen, it writes them (changing
one of them afterwards turns the preset to `custom`); written by hand in `config.ini`, it fills in only the keys the
file leaves out.

| Preset | `video.mode` | `video.resolution` | `video.upscaler` | `audio.reverb` | Other |
|---|---|---|---|---|---|
| `quality` | `docked` | `native` (1080p) | `linear` | `1` | |
| `balanced` | `handheld` | `native` (720p) | `fsr` | `1` | |
| `low` | `docked` | `540` | `fsr` | `0` | `video.frame_gen = off` |

`low` is meant for dual/quad-core CPUs, integrated GPUs (Intel UHD/Iris, older AMD APUs, Apple M1) and 4-6 GB of
RAM. Measured on an M1 (MoltenVK) in the intro's 3D scenes, GPU time per frame (timestamps per render pass, same
scripted input): TV mode at 1080p 14.9 ms, handheld mode (720p) 9.4 ms, handheld mode at 540p 8.2 ms, TV mode at
540p 6.8 ms, which is why `low` keeps TV mode and lowers `video.resolution` instead. The game's CPU work per frame
does not change with either (the draw replay takes 1.5-2.2 ms in all four); the reverb costs about one CPU core.

The game's logic runs once per frame, so `video.frame_rate = 60` also makes it run twice as fast (see
[Status](#status)). For 60 fps on screen at the normal speed, keep the frame rate at 30 and use frame generation:
`builtin` draws an in-between frame for each of the game's frames on the GPU (motion estimated between the two; about
1.5 ms on an Intel Iris Xe at 1080p) and shows it half a frame before the real one, which is the added latency. `lsfg`
uses [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) instead, with your own copy of Lossless Scaling's `Lossless.dll`.

## Controls

Any controller that SDL3 recognises works: Xbox, PlayStation, Switch Pro Controller and Joy-Con, and generic
gamepads.

- **Confirm / cancel layout:** Nintendo (A on the right) or Xbox/PlayStation (confirm on the bottom).
- **Button icons:** the game's own on-screen button prompts are redrawn for Xbox, PlayStation or Switch, or match
  the connected controller automatically. The icons are generated during setup from the game's own icon font
  (`tools/make_button_glyphs.py`), so no game artwork is shipped.
- **Keyboard:** every key can be remapped on the Controls page.

Default keyboard layout:

| Switch | Keyboard |
|---|---|
| Left stick | W A S D |
| Right stick (camera) | I J K L |
| D-pad | Arrow keys |
| A / B | Space / Left Shift |
| X / Y | X / F |
| L / R | Q / E |
| ZL / ZR | Z / C |
| Plus / Minus | Enter / Escape |
| Settings screen | F1 (controller: Minus + Plus) |
| Toggle fullscreen | F11 or Alt+Enter |

## Saves

As on the Switch, *New Horizons* keeps one island per console. The save lives in
`private/acnh/user/device_save/`. The game writes its first save at the end of the first day.

From the **Saves** page you can import a save from:

| Source | Notes |
|---|---|
| **JKSV** or **Checkpoint** backups | A folder or `.zip`, including straight from a mounted SD card (the newest backup is detected) |
| **yuzu** and forks: **suyu**, **sudachi**, **citron**, **eden**, **torzu** | Detected automatically, including Flatpak installs |
| **Ryujinx** / **Ryubing** | Detected automatically, including Flatpak installs |
| Another **ACNHRecomp** folder | |

- **Before every import or restore, the current save is backed up** to `private/acnh/user/backups/`. The 10 newest
  backups are kept, and "Back up now" makes one on demand.
- Imports are staged next to the save folder and moved in at once, so a failure never leaves a half-written save.
- After an import or restore, **quit and restart the game**, so it does not write the old island over the new one.
- **Export** writes the save to a folder (`ACNH-<date>-<time>/`) or a `.zip` in the layout that JKSV and Checkpoint
  restore on a console.

## Languages and mods

The game language can be any of the 14 languages the game ships with: English (US, UK), Spanish (Spain, Latin
America), French (France, Canada), German, Italian, Dutch, Russian, Japanese, Korean, and Simplified and
Traditional Chinese. On "Automatic" it follows the system locale.

**Brazilian Portuguese** is available through a community fan translation. It is **not distributed with this
project**: it consists of modified game files, and its author asks that it not be redistributed. If you have the
package, install it from your own copy:

```sh
tools/setup_acnh.sh --mod ptbr=~/Downloads/<translation package>.zip
```

Then choose "Português (Brasil)" as the game language (or leave it on Automatic on a Portuguese system). The
translation replaces the European Spanish texts, so the console language becomes Spanish and the mod's files are
layered on top.

**RomFS mods** in general work the same way (LayeredFS): `--mod <name>=<zip>` extracts a zip with a `romfs/` folder
to `private/acnh/mods/<name>/romfs/`. When the game opens a file, the first enabled mod that has a file at the same
path wins; otherwise the original is used. Mods can only replace files that already exist in the game. Enable mods
with `game.mods = <name>[,<name>...]` in `config.ini`.

## Building from source

`tools/setup_acnh.sh` is the supported path. What it does, step by step:

```sh
P=private/venv/bin/python
# 1. Import ExeFS + RomFS with the update applied (and any DLC)
private/build/importer/nximport --exefs --title 01006F8002326000 --out private/acnh \
    private/keys/prod.keys title.keys base.nsp update.nsp
# 2. Recompile the three modules
$P -m recomp private/acnh/exefs/main    private/acnh/gen/main    --name main    --base 0x4000000000
$P -m recomp private/acnh/exefs/sdk     private/acnh/gen/sdk     --name sdk     --base 0x4010000000
$P -m recomp private/acnh/exefs/subsdk0 private/acnh/gen/subsdk0 --name subsdk0 --base 0x4018000000
# 3. Extract and translate the shaders
tools/extract_shaders.sh private/acnh
dotnet private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll \
    --batch private/acnh/shaders_extracted --out private/acnh/spirv --jobs 8 --spirv-val /usr/bin/spirv-val
# 4. Button icons from the game's own icon font
$P tools/make_button_glyphs.py --romfs private/acnh/extract/romfs --out private/acnh/fonts
# 5. Build
CC=clang CXX=clang++ cmake -S . -B private/acnh/build -G Ninja -DNX_GAME=acnh -DCMAKE_BUILD_TYPE=Release \
    -DNX_VULKAN_INCLUDE=$PWD/private/third_party/Vulkan-Headers-1.4.357/include
ninja -C private/acnh/build acnh
```

Notes:

- The setup script filters malformed lines out of `prod.keys` (some key dumps contain entries the importer does
  not need) and writes the cleaned copy to `private/keys/`.
- The recompiler is re-run when `recomp/emit.py` is newer than the generated code.
- Recompiling `main` needs about 3 GB of RAM; compiling the generated C++ is the long step, and memory, not cores,
  usually limits the job count.
- Contributors: install the pre-commit hook with
  `ln -s ../../tools/check_no_game_data.sh .git/hooks/pre-commit`.
- The recompiler can be checked against Unicorn with `tests/difftest.py` (differential test of every instruction
  shape).

## Troubleshooting and FAQ

**Reporting a bug or a crash.**
Settings (Plus + Minus) > About > **Create problem report** writes a `.zip` on the desktop with this session's log,
the previous one (where a crash or freeze shows up), your settings and the computer's model, with no game data and
your home folder's path replaced by `~`. Attach it to a GitHub issue. If the game crashes before the settings screen
opens, run `NX_REPORT=1 private/acnh/build/acnh` once after the crash.

**The picture tears in full screen.** Settings > Video > VSync (on by default).

**Native 60 FPS at the normal game speed.** Run `tools/setup_60fps.sh` once: it downloads tmouh's
[ACNH 60 FPS + SpeedFix](https://github.com/tmouh/acnh-60fps) (CC BY-NC 4.0; not part of this repository), builds
its parameter file from your dump and recompiles `main` with it as a second version of every function it changes.
Then Settings > Video > Frame rate > 60. The game's speed follows the frame rate, so it plays slower wherever the
computer cannot hold 60.

**The setup stops with "first run: pass --keys, --base and --update".**
The import has not been done yet; pass all three on the first run.

**The importer fails or the file count is wrong.**
Check that the update is exactly **3.0.2** or **3.0.3**, that your `prod.keys` come from a console with firmware recent enough
for it, and pass `--title-keys` if you have them. The import log is in `private/acnh/logs/import.log`.

**The build runs out of memory.**
Lower the job count, for example `JOBS=4 tools/setup_acnh.sh`.

**The opening movie is black.**
Install `libvpx` and `libvorbis` (development packages) and rebuild.

**The game runs too slowly on a laptop.**
Use `tools/acnh_linux.sh intel` or `lightest`, turn off the environmental reverb, and keep 720p rendering.
Leave "Prepare shaders at startup" on to avoid stutter in places you have already visited.

**Everything runs twice as fast.**
The frame rate is set to 60 fps. Set it back to 30 fps (see [Status](#status)).

**The window does not open on GNOME / Wayland.**
Some GNOME setups make libdecor/GTK abort the process; the runtime prefers X11/XWayland for that reason. Check
`private/acnh/nx_play.log`.

**Can I visit friends or use Nook Link?**
No. Online services answer as a console without an internet connection.

**Does time pass on my island?**
Yes. The island follows your computer's date, time and time zone.

**Where are the logs?**
`private/acnh/nx_play.log` for the game and `private/acnh/logs/` for the setup stages.

**Can I get a prebuilt download?**
No. A build contains recompiled game code, so it has to be produced from your own dump.

**Still need help or ran into an unexpected bug?**
Join our [Discord Community](https://discord.gg/VJW9Dtdu7a) with your hardware details and log files (`private/acnh/nx_play.log`).

## Repository layout

| Path | Contents |
|---|---|
| `recomp/` | AArch64 to C++ static recompiler (Python, Capstone) |
| `runtime/` | Shared runtime: Horizon kernel, IPC and services, `nn::*` HLE, NVN to Vulkan, settings UI, platform layers |
| `games/acnh/` | ACNH-specific code (name, program ID, engine hooks, diagnostics) |
| `tools/importer/` | NSP/XCI import: keys, tickets, NCA, BKTR, RomFS |
| `tools/maxwell/` | Shader container extraction and the Maxwell to SPIR-V harness |
| `launcher/` | The first-run launcher: file checks, setup pipeline, home page (no game code) |
| `tools/setup_acnh.sh`, `tools/launcher.sh`, `tools/acnh_linux.sh`, `tools/acnh_mac.sh` | Setup and launch scripts |
| `tools/check_no_game_data.sh` | Pre-commit hook that blocks game data |
| `tests/` | Recompiler differential test (Unicorn) and runtime unit tests |
| `docs/` | `STATUS.md` (progress), `ACNH.md` (the game), `REFERENCES.md` (reference projects), `upstream/` (framework docs) |
| `private/` | Your dump, generated code and build (git-ignored, never committed) |

## How it works

```
 your dump (base NSP + update)          tools/importer (nximport): keys, NCA, BKTR, RomFS
          |
          |-- ExeFS: main, sdk, subsdk0 --> recomp/ (Python + Capstone): AArch64 -> C++ ---+
          |                                                                                |--> clang --> private/acnh/build/acnh
          |-- RomFS: Maxwell shaders ----> tools/maxwell + Ryubing translator -> SPIR-V ---+
          |                                                                                |
          '-- RomFS: models, textures, audio ... read at run time               runtime/ (C++): Horizon kernel,
                                                                                 IPC/services, nn::* HLE,
                                                                                 NVN -> Vulkan, SDL3
```

1. **Import.** The importer reads your NSPs without hactool, decrypts the NCAs and applies the update's BKTR
   patch (including the newer format) to produce the ExeFS and a complete RomFS. DLC RomFS goes to
   `private/acnh/aoc/<index>/`.
2. **CPU.** Each AArch64 function becomes a C++ function. Guest memory is host memory: the modules are mapped at
   fixed addresses (`0x40_0000_0000` on Linux), so a game pointer is a host pointer, with no translation or byte
   swapping. Branches inside a function become `goto`, jump tables become `switch`, and indirect calls go through a
   lookup table. ACNH 3.0.2 and 3.0.3 use SDK 20.5, so the recompiler gained new instructions (`sqshl`, `sqrshl`, `uqshl`,
   `sli`, `sri`, `tbx`, byte `pmull`, and others), each validated against Unicorn, and RELR relocations support.
3. **SDK and system.** Nintendo's `sdk` module (libc, libc++, `nn::os`, `nn::fs`, ...) is recompiled too, because
   `main` embeds its object layouts; `subsdk0` (`nn::movie`) is the third module. The runtime acts as the dynamic
   loader and a minimal Horizon kernel (threads, synchronization, memory, SVCs) and replaces `nn::*` functions by
   name (HLE): file system and saves in host folders, input through SDL3, the clock from the host, audio, and a
   VP8/VP9 decoder (libvpx) for the opening movie in place of the console's hardware decoder.
4. **Graphics.** The game asks `nvnBootstrapLoader` for every NVN function by name; the runtime returns its own,
   which record command buffers and replay them in Vulkan. Shaders are translated **ahead of time** from Maxwell to
   SPIR-V with the Ryujinx/Ryubing translator, keyed by the hash of the code block. Textures are de-tiled from the
   block-linear layout, and ASTC/BCn are decoded.
5. **Performance.** Inside each recompiled function the guest's general, vector and flag registers live in C++
   locals rather than in the CPU context in memory (`recomp/regcache.py`). They are written back only around calls,
   system calls and returns, and only when they may have changed. The host compiler can then keep them in machine
   registers instead of reloading them after every guest store. The I3DL2 reverb, the costliest guest function,
   runs about 2.2 times faster this way; the gain is larger on ARM64 hosts, which have as many registers as the
   guest. The game decrypts its data with AArch64 AES instructions while loading; on x86 they map to AES-NI when
   the CPU has it (the software path cost about 1000 times more). Vulkan pipelines are cached on disk and can be
   prepared at startup.

More detail: [docs/ACNH.md](docs/ACNH.md) (the game's modules, RomFS and shader containers),
[docs/STATUS.md](docs/STATUS.md) (problems found and fixed) and [docs/upstream/](docs/upstream/) (the framework).

## Credits and references

- [SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) (MIT): the base framework:
  recompiler, runtime, NVN to Vulkan, importer and shader translation pipeline.
- [Ryujinx / Ryubing](https://codeberg.org/Software_arch/Ryubing) (MIT): the Maxwell shader translator, used
  offline.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT): the settings screen.
- [ACNH 60 FPS + SpeedFix](https://github.com/tmouh/acnh-60fps) by tmouh (CC BY-NC 4.0): the 60 FPS game-speed
  rescaling, downloaded by `tools/setup_60fps.sh` and applied on your machine (not redistributed here).
- [SDL3](https://github.com/libsdl-org/SDL), [Capstone](https://www.capstone-engine.org/),
  [Unicorn](https://www.unicorn-engine.org/), libvpx, libvorbis, AMD FidelityFX FSR 1.
- Projects studied before starting (summary in [docs/REFERENCES.md](docs/REFERENCES.md)):
  [LASRecomp](https://github.com/peiikai/LASRecomp) (clock, saves, shared fonts, keyboard),
  [SwitchRecomp](https://github.com/bgyss/SwitchRecomp) (HIPC/CMIF/TIPC specifications, validation) and
  [uDPR](https://github.com/shatter9652/uDPR) (resumable staged setup).
- The authors of the Brazilian Portuguese fan translation, which this project supports loading but does not
  distribute.

## License

ACNHRecomp is released under the [MIT License](LICENSE). Code inherited from SuperMarioWonderRecomp keeps its
original copyright notice.

This license covers the project's own source code only. It grants no rights to *Animal Crossing: New Horizons*,
which is the property of Nintendo.
