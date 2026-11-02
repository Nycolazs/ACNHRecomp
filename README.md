# ACNHRecomp

**A native PC port of Animal Crossing: New Horizons, made by static recompilation.**

<p align="center">
  <a href="https://discord.gg/VJW9Dtdu7a">
    <img src="https://img.shields.io/badge/Discord-Join%20Community-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord Community">
  </a>
</p>

ACNHRecomp turns the Switch code of *Animal Crossing: New Horizons* (title ID `01006F8002326000`; version 3.0.2
recommended, 3.0.3, 2.0.7 and 1.0.0 supported) into native code for your computer. It is not an emulator: the game's AArch64 machine code is
translated ahead of time into C++ and compiled for the host, the system services the game calls are reimplemented
natively, and the game's NVN graphics calls are translated to **Vulkan**. It is built on the framework of
[SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp).

> [!IMPORTANT]
> **No game files are included.** This repository contains no code, assets, keys or firmware from the game, and
> neither does anything the project shares. It does not decrypt or unpack game containers either: it reads a folder
> with the game's executables and files that you prepared yourself from your own legally obtained copy, and builds the
> port from it on your computer. Never distribute the resulting executable or your game folder: they contain the game.

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
- 💬 **Help & Support**: Get quick guidance with building and troubleshooting.
- ⚙️ **Testing & Hardware**: Share logs, test results across different CPUs and GPUs, and report bugs.
- 📢 **Development Updates**: Get real-time announcements, test builds, and sneak peeks at upcoming features.

## Contents

- [Community](#community)
- [Features](#features)
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
  pads), a Switch or Xbox/PlayStation confirm-button layout, and in-game button icons redrawn for Xbox,
  PlayStation or Switch. Fully remappable keyboard controls. **Party Play** on macOS and Linux: up to four
  controllers, one per player, joining as on the console (the first to press a button is player 1).
- **Mouse and keyboard:** the mouse works as the console's touch screen (window buttons, list items and dialogue
  choices take a click; a click while a message waits advances it), and the PC keyboard types names and messages
  wherever the game's text entry is open.
- **Save management:** import from Switch backups (JKSV, Checkpoint), from emulators (yuzu and its forks,
  Ryujinx/Ryubing) or from another ACNHRecomp; export in a format JKSV and Checkpoint can restore on a console;
  automatic backups.
- **All 14 game languages**, plus Brazilian Portuguese through a fan translation you install yourself, loaded as a
  RomFS mod (LayeredFS).
- **DLC** (add-on content) from your game folder's `aoc/`.
- **Island clock** follows your computer's date, time and time zone, as on the console.
- **Performance work for modest hardware:** presets for integrated GPUs, an option to bypass the CPU-heavy audio
  reverb, pipelined GPU submits, AES-NI acceleration of the game's data decryption during loads, and a shader
  pre-compilation pass at startup.

## Status

The project is a work in progress. Everything below was measured on Linux x86-64 with an **Intel Iris Xe** (Mesa)
GPU; see [docs/STATUS.md](docs/STATUS.md) for the full, regularly updated log.

| Area | State |
|---|---|
| CPU recompilation (`main`, `sdk`, `subsdk0`) | Working: about 19 million instructions, 0 unhandled |
| Shaders (Maxwell to SPIR-V) | Working: 6,200+ distinct blocks, 0 invalid |
| Boot, system services, audio, clock, input | Working |
| Opening: Nook Inc. counter scene in 3D, character creation | Working |
| Opening movie (VP9 + Vorbis), arrival on the island | Working |
| Free roaming on the island | Working, 30 fps on an Intel Iris Xe |
| First save (written at the end of the first day) | Not yet verified in a full playthrough |
| 60 fps | Not native yet (see below) |
| Online features through Nintendo (Dodo codes, Nook Link, Dream Suite) | Unavailable |
| Visits between PCs (airport, Local Play), over LAN or the internet, no server | Works on Windows; [docs/LOCAL_PLAY.md](docs/LOCAL_PLAY.md) |

**Known issues**

- Some villagers (birds) may render with part of the head black. Starting villagers are random, so it does not
  always reproduce.
- **60 fps is not native yet.** The game's logic advances once per frame, so presenting at 60 fps makes the whole
  game run twice as fast. A real 60 fps mode requires halving the step of every frame-counting system and is not
  done yet. Play at 30 fps.
- The first save only happens at the end of the first in-game day, and that path has not been verified end to end
  in a full playthrough. Back up and be prepared for surprises.
- Nintendo's online services are not available: the game sees a console with no internet connection. Visits between
  PCs go through Local Play instead (page Online in the settings, [docs/LOCAL_PLAY.md](docs/LOCAL_PLAY.md)).

**Platforms**

| Platform | State |
|---|---|
| Linux x86-64 | Tested (Ubuntu, clang 21, Intel Iris Xe / Mesa ANV) |
| macOS (Apple Silicon, MoltenVK) | Tested (M1, macOS 26/27, Homebrew's MoltenVK) |
| Windows x86-64 | Builds with `tools/setup_acnh.py` (clang-cl, SDL3); the launcher is in the releases. Contributed by a fork, not fully tested here |
| Android (Snapdragon, Turnip) | Tested (Galaxy S20 FE / Snapdragon 865: a steady 30 fps drawn at 900 rows and upscaled with FSR 1 to the screen, battery under ~41 °C after 20 minutes; frame generation optional). The app holds no game and is in the releases; it asks for the game folder your computer prepares from your own game, see [docs/ANDROID.md](docs/ANDROID.md) |

## Disclaimer

ACNHRecomp is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Nintendo.
*Animal Crossing* is a trademark of Nintendo. The project does not provide, and will not help you obtain, game
files, keys or firmware, and contains no tool to decrypt or unpack them. It only works with a copy of the game you
prepared yourself from hardware and software you own.

A pre-commit hook ([tools/check_no_game_data.sh](tools/check_no_game_data.sh)) refuses commits that contain game
files, keys, shader binaries, recompiled game code or binary blobs. Everything derived from your dump stays in
`private/`, which is ignored by git.

This project was built with AI: nearly all new code, tools and documentation were written by Claude (Anthropic,
via Claude Code), directed and tested by a person. Its base, SuperMarioWonderRecomp, was also AI-assisted.

## Requirements

### Your game folder

A folder **prepared by ACNHRecomp-Importer** from your own copy of the game.

> **ACNHRecomp only accepts game folders made by ACNHRecomp-Importer.** The launcher and `tools/setup_acnh.py`
> refuse any other folder, including an ExeFS + RomFS dump extracted with other tools. ACNHRecomp-Importer is a
> separate companion app (Windows, macOS, Linux and Android) that turns your NSP or XCI, its update and your console's
> keys into this folder, and recompiles the game's code on the way. It is **available on our Discord:
> [discord.gg/VJW9Dtdu7a](https://discord.gg/VJW9Dtdu7a)**. Get it there, prepare your own copy with it, and point the
> launcher (or `tools/setup_acnh.py --game`) at the folder it writes.

The folder's layout:

| Path | Contents | Required |
|---|---|---|
| `exefs/` | `main`, `sdk`, `subsdk0` (the game's executables, NSO) and `main.npdm` | Yes |
| `romfs/` | The game's files, with the update applied (about 10.5 GB). `extract/romfs/` is accepted too | Yes |
| `aoc/<index>/` | Add-on content (DLC), one folder per add-on | Optional |
| `game.ini`, `.import/` | What ACNHRecomp-Importer imported (title, update version, main's build id) and its record of a finished import: how the launcher and the setup recognise its folders | Yes |
| `gen/` | The game's code recompiled to C++ by the importer. When it was made by the same recompiler as this repository's `recomp/`, the setup uses it instead of recompiling | Optional |

Version 3.0.2 (or 3.0.3) gets every fix of the port (native 60 FPS, wide screens, the mouse); 2.0.7 and 1.0.0 play
with picture, sound and the mouse; any other version is built as it is ([docs/VERSIONS.md](docs/VERSIONS.md)). The
setup links to this folder instead of copying it: keep it where it is.

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

Windows: Python 3 (`winget install Python.Python.3.13`) and the Visual Studio C++ build tools (`winget install
Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools
--includeRecommended"`); the setup fetches LLVM (clang-cl) and SDL3 itself.

The setup script downloads everything else into `private/third_party/`: a Python virtual environment (capstone,
lz4, zstandard, fonttools, cmake, ninja, unicorn), Vulkan-Headers 1.4.357, the .NET SDK 10.0.401 (unless the system
has .NET 10), the Ryubing shader translator source and Dear ImGui.

## Installation

### Downloads

The [releases](https://github.com/Nycolazs/ACNHRecomp/releases) are ready to play: none of them has any part of the
game, and none needs a compiler. You need two things:

1. **Your game folder**, made by [ACNHRecomp-Importer](#your-game-folder) (on our
   [Discord](https://discord.gg/VJW9Dtdu7a)) from your own copy of the game. Run it once and keep the folder it writes.
2. **The package for your system** from the [latest release](https://github.com/Nycolazs/ACNHRecomp/releases/latest):

| File | System |
|---|---|
| `ACNHRecomp-<version>-windows-x64.zip` | Windows 10/11, x64 |
| `ACNHRecomp-<version>-macOS-arm64.zip` | macOS 14 or newer, Apple Silicon (M1 or newer) |
| `ACNHRecomp-<version>-linux-x64.tar.gz` | Linux x86-64, glibc 2.35+ (Ubuntu 22.04, Fedora 36, Debian 12 and newer) |
| `ACNHRecomp-<version>-android.apk` | Android 10+, Snapdragon (Adreno 6xx or newer): see [docs/ANDROID.md](docs/ANDROID.md) |
| `libvulkan_freedreno.so` | The Turnip Vulkan driver (Mesa, MIT), already inside the APK; for `tools/android_install.sh` |

#### Windows

1. Unzip `ACNHRecomp-<version>-windows-x64.zip` into a folder of your own (for example `Documents\ACNHRecomp`), not
   inside the zip viewer.
2. Double-click **`ACNHRecomp Launcher.exe`** (or `Start ACNHRecomp.bat`). If SmartScreen warns about an unknown
   app: *More info* > *Run anyway*.
3. Choose your game folder. The game starts.
4. On the launcher's home page, **Add to desktop** puts a shortcut with the ACNHRecomp icon on the desktop and in the
   Start menu (search for "ACNHRecomp"). Keep the unzipped folder where it is: the shortcut points into it.

#### macOS

1. Unzip `ACNHRecomp-<version>-macOS-arm64.zip`. It holds **`ACNHRecomp.app`** and `Start ACNHRecomp.command`.
2. The first time, double-click **`Start ACNHRecomp.command`**: the app is not signed by Apple, and this lets macOS
   open it (it removes the download quarantine) and then opens it. If macOS still refuses, right-click
   `ACNHRecomp.app` > *Open* > *Open*, or *System Settings* > *Privacy & Security* > *Open Anyway*.
3. Choose your game folder. The game starts.
4. On the launcher's home page, **Install in Applications** copies `ACNHRecomp.app` into Applications (or
   `~/Applications` without administrator rights) and puts it on the desktop. From then on open it from Launchpad,
   Spotlight or the Dock like any app; the downloaded folder can be deleted.

#### Linux

1. Extract the package: `tar -xzf ACNHRecomp-<version>-linux-x64.tar.gz`.
2. Run **`./Start ACNHRecomp.sh`** in the `ACNHRecomp` folder (or double-click it in your file manager). You need a
   Vulkan driver: `sudo apt install mesa-vulkan-drivers` (or your distribution's, or NVIDIA's).
3. Choose your game folder. The game starts.
4. On the launcher's home page, **Add to desktop** adds ACNHRecomp, with its icon, to the applications menu and to
   the desktop (`~/.local/share/applications/io.github.acnhrecomp.desktop`). Keep the extracted folder where it is.

#### The first start, updates and versions

- **Brazilian Portuguese and 60 FPS** come with the package: the first start copies the pt-BR fan translation and
  the files of tmouh's 60 FPS SpeedFix into your game folder (no download). Português (Brasil) is then in the game's
  languages (chosen by itself on a Portuguese system), and the game runs at 60 FPS at its normal speed unless you pick
  30. A game folder whose code was prepared without the SpeedFix stays at 30. A new package with newer files copies
  them again; `game.mod_fetch = off` in `config.ini` turns the copy off.
- **New versions:** the launcher looks for a newer release on GitHub each time it opens and says so on its home page,
  with a button to its download page. *Settings* > *Look for new versions when the launcher opens* turns it off. To
  update, download the new package and replace the old folder (or app): your game folder, saves and settings are
  not in it, so nothing is lost.
- **The version in use** is at the bottom of the launcher's *Settings*, and in the game's settings (F1) > *About*.
- **The icon:** the launcher, the game's window, the taskbar / Dock and the shortcuts all show the ACNHRecomp icon.

The launcher asks for one thing, the game folder ACNHRecomp-Importer wrote, and starts the game; the player beside
it (`acnh`) loads the game's code from that folder (`game/<platform>/`, see [Building from source](#building-from-source)).
Each package carries what it needs (SDL3, and on macOS the Vulkan loader and MoltenVK); a GPU with a Vulkan 1.3
driver is the one requirement. The packages are made by the repository's **Build** workflow (`.github/workflows/build.yml`,
`tools/package_release_*.sh`; the icons are in `assets/`).

The sections below are for building from this repository instead (developers, other versions of the game).

### From the repository, with the launcher

The launcher is a small program, built from this repository alone, that does the whole setup with you: it asks
for your game folder, checks this computer, recompiles the game and then starts it. It uses the same look as the in-game
settings screen, follows the system language (12 languages, changeable at any time from the top right) and works
from small laptop screens to 4K.

```sh
git clone <this repository> ACNHRecomp && cd ACNHRecomp
tools/launcher.sh            # builds the launcher the first time (cmake, a C++ compiler and SDL3), then opens it
```

On macOS, double-clicking `Jogar ACNH.command` does the same (and, once the game is set up, starts it).

**The short way.** The first time, the launcher opens straight on the system's folder dialog: pick your game
folder, and if this computer has what it needs the setup runs by itself and the game starts when it is done (a
pt-BR `.zip` in Downloads is taken along). The pages below only show up when something is missing or fails, or when
the dialog is closed. Later the launcher starts the game after a 3-second countdown; a click or a key keeps it
open on its home page (`launcher.autostart = 0` in `launcher.ini`: no countdown).

By hand: `cmake -S . -B build/launcher -DNX_GAME=acnh && cmake --build build/launcher --target acnhrecomp_launcher`,
then run `build/launcher/acnhrecomp_launcher`.

1. **Welcome.** Choose *Set up from my game files*, or *I already have a recompiled build* and pick its folder (an
   install folder made by the launcher, `private/acnh`, or this repository).
2. **Your game.** The game folder (see [Requirements](#your-game-folder)) and, optionally, the pt-BR translation
   `.zip` (not distributed here). Choose them, let Search find them in your usual folders, or drop them on the
   window. The folder is checked at once: its layout, its executables and the game version from main's build id
   (nothing is decrypted or unpacked).
3. **Install.** Where the game goes (default `~/.local/share/ACNHRecomp`, `~/Library/Application Support/ACNHRecomp`
   on macOS), and a check of this computer: compiler, Python, curl/tar, SDL3 development files, Vulkan driver,
   movie libraries, disk space and memory, with the command that installs what is missing.
4. **Build.** Runs `tools/setup_acnh.py --game <game folder> --out <install folder>` and follows it: stages, progress (ninja's own
   counter during the compile, with an estimate of the time left), the full log, Stop. If a stage fails, the page
   says which, keeps the log open and offers *Copy log*, *Open logs folder* and *Try again* (finished stages are
   not redone).
5. **Play.** Starts the game with its data folder (`NX_DATA_ROOT`), and the launcher closes.

On later starts the launcher opens on a home page: **Play**, **Settings** (game language including pt-BR, display,
rendering, upscaling, frame rate; written to the game's `config.ini`), **Rebuild** (after updating the
repository) and **Change build folder**. Its own settings are in `launcher.ini` in the user's settings folder
(`~/.local/share/ACNHRecomp` on Linux).

Windows: the launcher runs `tools/setup_acnh.py` with Python as on the other systems (the Python launcher `py`,
or `python` on PATH).

### From the command line

```sh
git clone <this repository> ACNHRecomp && cd ACNHRecomp
python3 tools/setup_acnh.py --game ~/ACNH-game          # Windows: py tools\setup_acnh.py --game C:\ACNH-game
```

To add DLC later, put it in the game folder's `aoc/` and run the setup again. `tools/setup_acnh.sh` runs the same
script, for scripts that call it by that name.

`--out <folder>` puts the game's files (import, generated code, build, logs) in that folder instead of
`private/acnh`; start the game from such a folder with `NX_DATA_ROOT=<folder> <folder>/build/acnh`.

The script runs in stages and skips every stage whose output already exists. If something fails, fix it and run
`tools/setup_acnh.py` again (once the game folder is linked, no arguments are needed).

| Stage | Output | Time (12 threads, 30 GB RAM) |
|---|---|---|
| Tools | `private/venv`, `private/third_party/*`, shader harness | about 5 min |
| Game | `private/acnh/{exefs,extract/romfs,aoc}` linked to the game folder, `analysis/*.bin` | seconds |
| Recompile | `private/acnh/gen/{main,sdk,subsdk0}` (1.6 GB of C++) | about 8 min |
| Shaders | `private/acnh/spirv` | about 2 min |
| Fonts and button icons | `private/acnh/fonts` | under a minute |
| Build | `private/acnh/build/acnh` | 1 to 2 hours the first time |

| Variable | Effect |
|---|---|
| `JOBS=<n>` | Compiler jobs (default: RAM in GB divided by 3, at most the core count) |
| `FORCE=<stage>` | Redo one stage: `game`, `recomp`, `shaders`, `harness`, `glyphs` |

To check your game folder: the version stage prints the version and main's build id, and the recompiler ends with
`unhandled 0` for each of the three modules.

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
| Video | Quality preset (quality, balanced, low), windowed / fullscreen, window size, game mode (1080p TV mode or 720p handheld mode), internal resolution (540p to 2160p/4K, or the window's), upscaling (bilinear or FSR 1, with sharpness), anti-aliasing (FXAA, supersampling), anisotropic filtering, aspect ratio (16:9, automatic from the window, 16:10, 4:3, 21:9, 32:9, phone ratios; applied live), frame rate, frame generation (built-in, or lsfg-vk if installed), GPU (automatic, dedicated, integrated), prepare shaders at startup |
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
| `video.antialiasing` | `off`, `fxaa`, `ssaa15`, `ssaa2` (supersampling 1.5x / 2x) | `NX_AA` |
| `video.anisotropy` | `game`, `2`, `4`, `8`, `16` | `NX_ANISOTROPY` |
| `video.aspect` | `16:9`, `auto`, `16:10`, `4:3`, `21:9`, `32:9`, phone ratios, any `W:H` | `NX_ULTRAWIDE` |
| `video.gpu` | `auto`, `discrete`, `integrated` | `NX_GPU` |
| `video.frame_rate` | `30`, `60` | `NX_PRESENT_INTERVAL` |
| `video.frame_gen` | `off`, `builtin`, `lsfg` | `NX_FRAMEGEN` (`builtin`) |
| `video.shader_prepare` | `0`, `1` | |
| `audio.volume` | `0`-`100` | |
| `audio.reverb` | `0`, `1` | `NX_AUDIO_REVERB` |
| `audio.mute_background` | `0`, `1` | |
| `input.layout` | `console`, `xbox` (`nintendo` is read as `console`) | |
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

**Brazilian Portuguese** is available through a community fan translation. It is **not part of this repository or
of the release packages** (it consists of modified game files): the released player downloads it into your game
folder at its first start (see [The first start, updates and versions](#the-first-start-updates-and-versions)). In
a build from this repository, install it from your own copy of the package:

```sh
python3 tools/setup_acnh.py --mod ptbr=~/Downloads/<translation package>.zip
```

Then choose "Português (Brasil)" as the game language (or leave it on Automatic on a Portuguese system). The
translation replaces the European Spanish texts, so the console language becomes Spanish and the mod's files are
layered on top.

**RomFS mods** in general work the same way (LayeredFS): `--mod <name>=<zip>` extracts a zip with a `romfs/` folder
to `private/acnh/mods/<name>/romfs/`. When the game opens a file, the first enabled mod that has a file at the same
path wins; otherwise the original is used. Mods can only replace files that already exist in the game. Enable mods
with `game.mods = <name>[,<name>...]` in `config.ini`.

## Building from source

`tools/setup_acnh.py` is the supported path. What it does, step by step:

```sh
P=private/venv/bin/python
# 1. Link the game folder, and the modules as loaded for the shader extraction
ln -s ~/ACNH-game/exefs private/acnh/exefs; mkdir -p private/acnh/extract; ln -s ~/ACNH-game/romfs private/acnh/extract/romfs
for m in main sdk subsdk0; do $P tools/nso2bin.py private/acnh/exefs/$m private/acnh/analysis/$m.bin; done
# 2. Recompile the three modules (main with the hooks of its version: tools/acnh_version.py, games/acnh/versions.txt)
$P -m recomp private/acnh/exefs/main    private/acnh/gen/main    --name main    --base 0x4000000000
$P -m recomp private/acnh/exefs/sdk     private/acnh/gen/sdk     --name sdk     --base 0x4010000000
$P -m recomp private/acnh/exefs/subsdk0 private/acnh/gen/subsdk0 --name subsdk0 --base 0x4018000000
# 3. Extract and translate the shaders
$P tools/extract_shaders.py private/acnh
dotnet private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll \
    --batch private/acnh/shaders_extracted --out private/acnh/spirv --jobs 8 --spirv-val /usr/bin/spirv-val
# 4. Button icons from the game's own icon font
$P tools/make_button_glyphs.py --romfs private/acnh/extract/romfs --out private/acnh/fonts
# 5. Build
CC=clang CXX=clang++ cmake -S . -B private/acnh/build -G Ninja -DNX_GAME=acnh -DCMAKE_BUILD_TYPE=Release \
    -DNX_VULKAN_INCLUDE=$PWD/private/third_party/Vulkan-Headers-1.4.357/include
ninja -C private/acnh/build acnh
```

**The player** (what plays a folder ACNHRecomp-Importer prepared, with no game code of its own): configure with
`-DNX_GAME_FROM_FOLDER=ON` and put `acnh` (`acnh.exe`) beside the launcher. It loads the game's code from the folder
(`game/linux-x64/libacnh_game.so`, `game/macos-arm64/libacnh_game.dylib`, `game/windows-x64/acnh_game.dll`;
`runtime/src/game_folder.cpp`) and refuses a library built against other runtime headers (`NX_GAME_ABI`, a hash of
`runtime/include/nx/{cpu,module,runtime}.h`, which the importer carries a copy of). The Android app loads
`game/android-arm64/libacnh_game.so` the same way.

Notes:

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

**The setup stops with "no game in ...: pass --game".**
The game folder has not been linked yet: pass `--game <your game folder>` on the first run.

**The launcher or the setup says the folder was not prepared by ACNHRecomp-Importer.**
ACNHRecomp only accepts game folders made by ACNHRecomp-Importer. Download it on our Discord
([discord.gg/VJW9Dtdu7a](https://discord.gg/VJW9Dtdu7a)), prepare your game with it (your NSP or XCI, update 3.0.2 and
your console's keys) and choose the folder it writes. A folder it is still working on, or that it stopped preparing, is
not accepted either: let it finish.

**The setup says the game folder has no exefs/ and romfs/, or that romfs/ has no Shader/ folder.**
Choose the folder that holds both `exefs/` and `romfs/` (see [Requirements](#your-game-folder)), with the update
already applied to `romfs/`.

**The build runs out of memory.**
Lower the job count, for example `JOBS=4 python3 tools/setup_acnh.py`.

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
Visiting friends: yes, through Local Play at the airport, on a LAN or over the internet (page Online in the settings,
[docs/LOCAL_PLAY.md](docs/LOCAL_PLAY.md)). Nook Link and the rest of Nintendo's online services: no, they answer as a
console without an internet connection.

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
| `tools/maxwell/` | Shader container extraction and the Maxwell to SPIR-V harness |
| `launcher/` | The first-run launcher: file checks, setup pipeline, home page (no game code) |
| `tools/setup_acnh.py`, `tools/launcher.sh`, `tools/acnh_linux.sh`, `tools/acnh_mac.sh` | Setup and launch scripts |
| `tools/acnh_version.py`, `games/acnh/versions.txt` | The game version a folder holds, by main's build id, and its hooks |
| `tools/check_no_game_data.sh` | Pre-commit hook that blocks game data |
| `tests/` | Recompiler differential test (Unicorn) and runtime unit tests |
| `docs/` | `STATUS.md` (progress), `ACNH.md` (the game), `REFERENCES.md` (reference projects), `upstream/` (framework docs) |
| `private/` | Your dump, generated code and build (git-ignored, never committed) |

## How it works

```
 your game folder (exefs/, romfs/, aoc/)
          |
          |-- ExeFS: main, sdk, subsdk0 --> recomp/ (Python + Capstone): AArch64 -> C++ ---+
          |                                                                                |--> clang --> private/acnh/build/acnh
          |-- RomFS: Maxwell shaders ----> tools/maxwell + Ryubing translator -> SPIR-V ---+
          |                                                                                |
          '-- RomFS: models, textures, audio ... read at run time               runtime/ (C++): Horizon kernel,
                                                                                 IPC/services, nn::* HLE,
                                                                                 NVN -> Vulkan, SDL3
```

1. **Game folder.** The setup links your game folder's `exefs/`, `romfs/` and `aoc/` into the data folder, reads
   main's build id to pick the version's hooks (`games/acnh/versions.txt`), and lays the three modules out as they
   are loaded (`tools/nso2bin.py`) for the shader extraction.
2. **CPU.** Each AArch64 function becomes a C++ function. Guest memory is host memory: the modules are mapped at
   fixed addresses (`0x40_0000_0000` on Linux), so a game pointer is a host pointer, with no translation or byte
   swapping. Branches inside a function become `goto`, jump tables become `switch`, and indirect calls go through a
   lookup table. ACNH 3.0.2 uses SDK 20.5, so the recompiler gained new instructions (`sqshl`, `sqrshl`, `uqshl`,
   `sli`, `sri`, `tbx`, byte `pmull`, and others), each validated against Unicorn, and RELR relocations support.
3. **SDK and system.** The SDK module (`sdk`) (libc, libc++, `nn::os`, `nn::fs`, ...) is recompiled too, because
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
  recompiler, runtime, NVN to Vulkan and shader translation pipeline.
- [Ryujinx / Ryubing](https://codeberg.org/Software_arch/Ryubing) (MIT): the Maxwell shader translator, used
  offline.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT): the settings screen.
- [MHGURecomp](https://github.com/morerokk/MHGURecomp) by morerokk (MIT): the design of local play between PCs
  (`nn::ldn`, virtual sockets), [docs/LOCAL_PLAY.md](docs/LOCAL_PLAY.md).
- [ACNH 60 FPS + SpeedFix](https://github.com/tmouh/acnh-60fps) by tmouh (CC BY-NC 4.0): the 60 FPS game-speed
  rescaling, downloaded by `tools/setup_60fps.sh` (or, with a release, at the first start) and applied on your
  machine (not part of this repository).
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
