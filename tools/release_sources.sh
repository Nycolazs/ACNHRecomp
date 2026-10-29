#!/bin/bash
# The sources a release package carries (tools/package_release_*.sh), at the root of <folder>: HEAD (git archive:
# nothing of the game, nothing of private/) without what only development uses (docs, tests, CI, the profiling,
# debugging and code-generation tools, the packaging itself) and without the other systems' scripts. What stays is
# what the launcher's setup (tools/setup_acnh.py) and the build read, and the scripts README names for players.
# usage: tools/release_sources.sh <folder> windows|macos|linux
cd "$(dirname "$0")/.." || exit 1
set -e
DIR=${1:?usage: tools/release_sources.sh <folder> windows|macos|linux}
OS=${2:?usage: tools/release_sources.sh <folder> windows|macos|linux}
X=(.github .gitignore docs tests
   tools/package_release_*.sh tools/release_sources.sh tools/check_no_game_data.sh
   tools/bc tools/intel tools/play tools/shaders tools/*.ps1 tools/maxwell/spirv_harness/msl_check.swift
   tools/burst_anomalies.py tools/cpu_profile.py tools/detect_explode.py tools/find_*.py tools/frames.py tools/probe.py
   tools/translate_shader_misses.sh tools/gen_vk.py tools/gen_shaders.py tools/gen_spirv_inc.py tools/gen_mods.py)
case $OS in
  # No shell on a stock Windows, and the Android app is set up from macOS or Linux (docs/ANDROID.md).
  windows) X+=(android "*.sh" "*.command" tools/patches tools/make_android_icon.py tools/pack_spirv.py) ;;
  macos) X+=(tools/acnh_linux.sh) ;;
  linux) X+=("*.command" tools/acnh_mac.sh tools/install_mac_shortcut.sh) ;;
  *) echo "unknown system: $OS (windows, macos or linux)"; exit 1 ;;
esac
mkdir -p "$DIR"
git archive --format=tar HEAD -- . "${X[@]/#/:(exclude)}" | tar -x -C "$DIR"
