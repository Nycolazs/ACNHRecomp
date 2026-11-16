# prepare/

The game folder's preparation, inside ACNHRecomp: from the player's extracted ExeFS and RomFS, ACNHRecomp recompiles
the modules, converts the shaders, compiles the game's library with a bundled zig and writes the game folder, with no
ACNHRecomp-Importer and never keys, NSP or XCI (nothing here decrypts; a package lying beside the extracted files is
said and left alone).

```
acnh_prepare --exefs DIR --romfs DIR [--dlc DIR[:index]]... --out DIR [--platforms this|all|linux-x64,windows-x64,macos-arm64,android-arm64]
             [--no-60fps] [--in-place] [--zig PATH] [--zig-lib DIR] [--shader-tool PATH] [--cache DIR] [--jobs N]
acnh_prepare GAME --out DIR ...      GAME: one folder holding exefs/ and romfs/ (or an emulator's dump folder)
```

The launcher runs the same stages (its Prepare page, `launcher/prepare_job.cpp`): the picks probed and their findings
shown translated (the `prepare.*` keys in `runtime/src/ui/i18n_launcher.inc`), the free space against
`prep::space_needed()`, then `prep::start()` and `prep::status()` on its Preparing page; the folder then becomes the
game folder. `acnh_prepare` prints each stage and its progress; the exit code is 0 when the folder is ready, 2 when it is refused (the files,
the folder, the space, the tools), 1 when a stage failed, 4 when cancelled (Ctrl+C), 64 for a wrong command line. Run
again on the same folder, what is already right is kept: a library built from this main for this runtime, romfs.pak,
the add-ons, the shaders (another platform builds that platform only).

## The stages (`prepare.h`, `nx_prepare`)

`prep::start(Options)` runs them on a worker thread, `prep::status()` gives the stage, its fraction and the whole's,
the current item, the platform k of N, the log lines (each with its stage; an empty one marks where a stage began)
and, at the end, the exit code and the error; `prep::cancel()` stops it (a child process too, with every process it
started: its process group, a Job Object on Windows). Taken from
ACNHRecomp-Importer v1.0.7 (`src/nxcore_api.cpp`): its stages after the extraction.

| Stage | |
|---|---|
| Check | `io/` `probe()` of the picks (3.0.3 needed, 3.0.2 accepted with a note), the game folder (empty or prepared before; never inside the RomFS), the free space, zig against its `sizes.txt`, the shader tool |
| ExeFS | `exefs/main, sdk, subsdk0, main.npdm` copied; `analysis/<module>.bin`, the flat image ending where the data segment ends (the loader's size rule), and `analysis/main.npdm` |
| Recompile | `gen/<module>/` (`recomp/`), main with the hooks of its version and, on 3.0.2 / 3.0.3, the native 60 FPS variant (tmouh's SpeedFix as a second body); if the variant does not recompile, main without it |
| Shaders | the shader tool: `spirv_harness --prepare <romfs> analysis/main.bin --out .import/shaders_extracted --spirv spirv --jobs N --no-val`; kept only when converted for this main (`.import/shaders.build_id`; a folder from before it: `game.ini`'s build id, when it was complete), else `spirv/` and `spirv.pak` are made again |
| Packs | `romfs.pak` from the player's RomFS (packages left out; not with `--in-place`; kept when packed for this main, `.import/romfs.pak.build_id`, from a RomFS of the same files), `aoc/<index>/` copies of the add-ons, `spirv.pak` (the packed `.spv` then go; `spirv/summary.txt` and `results.tsv` stay). The folder's own `romfs.pak` as the RomFS is refused when packed for another main (a change of version cut short) |
| Compile | `game/<platform>/` for each platform chosen (`gamelib/`); if it fails with the variant, main again without it and every platform again, except when the toolchain itself is at fault; a library of a platform not chosen, built from another main, is removed; then `gen/` goes |
| Finish | `game.ini` (first line names ACNHRecomp; `title`, `version`, `build_id`), `.import/index.tsv` (the loose files: the RomFS in place, the add-ons) and `.import/complete` (a stage that replaces what a finished folder holds, an add-on, `romfs.pak`, the shaders, takes it away first) |

`--in-place` (taken on its own when the RomFS already is the game folder's `romfs/`): the RomFS stays loose where it
is, as the game folder's `romfs/` or `extract/romfs/`; `--out` defaults to the folder holding it. An add-on whose
index its folder's name does not give (`01006F8002327xxx`, `aoc/<n>`) is add-on 1, Happy Home Paradise, with a note;
`--dlc DIR:index` chooses. `NX_PREPARE_SKIP_RECOMP=1` (tests): no recompile; the compile builds `gen/` as it is when
there is one (`tools/make_fake_gen.py`'s stand-in).

The stages overwrite and delete `gen/` and `spirv/`, so a game folder (in place or not) whose `gen/` or `spirv/` the
preparation did not make is refused: they carry its mark (`.acnhrecomp_prepare`), or `gen/recomp_source.txt`, or are in
a folder a preparation finished before (`game.ini` naming ACNHRecomp, `.import/`). `tools/setup_acnh.py`'s data folder
(`extract/romfs/` and a `gen/` that is `NX_GEN_DIR`) is such a folder.

The folder is the one ACNHRecomp-Importer wrote, played the same way: the launcher's `prepared_folder()` takes both
(`launcher/files.cpp`), the player loads `game/<platform>/` (`runtime/src/game_folder.cpp`), `analysis/` (`loader.cpp`),
`romfs.pak` or `romfs/` and `aoc/<n>/` (`hle_fs.cpp`), `spirv.pak` (`nvn_shaders.cpp`).

## gamelib/

The game's library from `gen/`, compiled and linked by zig as child processes (`posix_spawn`, `CreateProcessW`), for
windows-x64 (`acnh_game.dll`), linux-x64 and android-arm64 (`libacnh_game.so`) and macos-arm64 (`libacnh_game.dylib`):
the biggest files first, as many jobs as the memory holds, funcs.h precompiled per module, a response file for the
link, Android's `DT_NEEDED` stubs (`libacnh.so`, `libc++_shared.so`, ...), `cpu_model.c` on x86-64. It is compiled
against this checkout's `runtime/include/nx/*.h` and `runtime/src/game_abi.cpp` (built in) with the top
`CMakeLists.txt`'s `NX_GAME_ABI`, so a library and the player of the same checkout always agree; `abi.txt`,
`build.txt` and `rev.txt` beside it say what it was built from, and one of another runtime is built again. Hot lists
(`gamelib/hot/<build id>.txt`, none yet) build the functions the game spends its time in at -O2 and the rest faster.
From the Importer's `src/gamelib/` (its runtime copies were identical to `runtime/`), without its package repair:
`check_toolchain()` compares the zig folder with the `sizes.txt` `tools/fetch_zig.py` writes.

## The tools it runs

- zig 0.17.0: `tools/fetch_zig.py <host> <dest>` (x86_64-linux, aarch64-linux, x86_64-windows, aarch64-macos), checked
  against ziglang.org's SHA-256 and trimmed to the four targets (about 335 MB, 12,000 files). The release carries it
  as `tools/zig/` beside the launcher and `acnh_prepare` (in ACNHRecomp.app: `Contents/Resources/tools/zig/`; Android:
  `libzig.so` in the APK's native libraries, `--android` trimming its lib folder to the phone's own platform, about
  75 MB, unpacked from the APK's `assets/zig-lib.zip` with its `sizes.txt`: `--zig-lib`).
- The shader tool: `tools/build_shader_tool.py <rid> <dest>` builds `tools/maxwell/spirv_harness` NativeAOT (one
  executable of about 5 MB, no .NET needed where it runs; .NET 10 SDK to build it), carried as `tools/shaders/`
  (in ACNHRecomp.app: `Contents/Resources/tools/shaders/`; Android: `linux-bionic-arm64`, `libspirv_harness.so` in the
  APK). `tools/package_release_*.sh` put both there.

## On Android (`prepare_jni.cpp`)

The app prepares the game folder on the phone itself: `libacnh_prepare.so` (`nx_prepare_jni`, apart from the
player's `libacnh.so`) gives `android/java/io/github/acnhrecomp/PrepareNative.java` the probe, `prep::start`,
`status` (with the log lines numbered, so the screen and the notification both read them) and `cancel`, as UTF-8
requests and JSON answers. `Prepare.java` is the screen (the check translated, the game folder, the add-on, the
stages), `PrepareService.java` the foreground service that runs it (wake lock, notification, the tools made ready);
`tools/build_android.sh` builds the library and carries the tools. The log goes to `.import/prepare.log` in the game
folder once it ends.

## recomp/

`recomp/*.py` (`python -m recomp`) ported to C++. Taken from ACNHRecomp-Importer v1.0.7 (`src/recomp/`, MIT, same
author) unchanged, except for the include paths and the command line's name.

- `nx_recomp`: static library, C++20 with no desktop-only code (it builds for Android too). `rc::recompile()`
  (`recomp.h`); `rc::acnh_data()` (`acnh_data.h`): `games/acnh/versions.txt`, `games/acnh/hooks*.txt` and the
  SpeedFix, embedded at build time.
- `acnh_recomp`: the command line, with `python -m recomp`'s arguments:
  `acnh_recomp <module.nso> <out dir> --name main --base 0x4000000000 [--jobs N] [--hooks f] [--variant f.pchtxt]`.
- Capstone 5.0.7, AArch64 only (`third_party/capstone/`, BSD-3-Clause). The generated code is built from Capstone's
  text, so the Python uses the same version (`tools/setup_acnh.py` installs `capstone==5.0.7`).
- `acnh/speedfix-*.pchtxt`: tmouh's SpeedFix for main, CC BY-NC 4.0 (`acnh/SPEEDFIX.md`).

`recomp/recomp.cmake` and `prepare.cmake` build them; the top `CMakeLists.txt` includes them on desktop, except in a
build of the generated code (`NX_GEN_PROBE`), and always on Android (`libacnh_prepare.so`). `io/`, `gamelib/` and `prepare.cpp` are plain C++20 too.

## The source guard

The Python and the C++ must generate the same code. `recomp/SOURCE` holds the version of `recomp/*.py` this port
matches: sha256 over `name\0content\0` of `recomp/*.py` in name order, as `tools/setup_acnh.py`'s `recomp_hash()`
computes it (and the Importer wrote it as `gen/recomp_source.txt`). `recomp_source_test` fails when `recomp/*.py` no
longer has that hash. A change to the Python recompiler is then ported to `recomp/` here, checked to give the same
output (`python -m recomp` and `acnh_recomp` with the same arguments on the game's modules), and the new hash, which
the test prints, goes into `SOURCE`.

## Tests

`-DNX_TESTS=ON`, `ctest -R "prepare|recomp"`, synthetic inputs only (no game data):

- `prepare_io_test`: the probe, the NSOs, the packs (byte for byte with `tools/pack_*.py`).
- `prepare_e2e`: the stages and `acnh_prepare` end to end on a synthetic ExeFS (`io/test_inputs.h`), RomFS, add-on
  and a package beside them, the recompile skipped and `fake_shader_tool` for the shaders; the folder checked as the
  player reads it, kept again on a second run, a cancel during the shader tool, the refusals, `--in-place`.
- `prepare_e2e_zig`: the Compile stage with a real zig (`NX_PREPARE_ZIG_DIR`, default `<build>/zig`, fetched by
  `tools/fetch_zig.py` when missing; skipped when it cannot be downloaded) on `tools/make_fake_gen.py`'s stand-in; the
  library loaded and its `nx_game_abi()` compared with the player's; a cancel while compiling (no compiler left
  running, on Linux) and the same folder then prepared to its end; a recompile of the synthetic modules failing and
  tried without the variant first.
- `prepare_launcher`: those folders through `launcher/files.cpp` (`check_file`, `build_contents`, and `find_build`
  with the player beside it in the build folder); an Importer folder still accepted, others refused and told as one
  to prepare; the synthetic extraction checked and found as what to prepare from (`Kind::Extract`).
- The launcher's pages, by hand: `NX_LAUNCHER_PAGE=prepare|preparing|choose` with `NX_LAUNCHER_PREPARE_IN`, `_DLC`,
  `_OUT`, `NX_LAUNCHER_PICK`, `NX_LAUNCHER_SHADER_TOOL=<build>/fake_shader_tool`, `NX_PREPARE_SKIP_RECOMP=1` (a
  `tools/make_fake_gen.py` stand-in in the folder for a library), and `NX_LAUNCHER_SHOT` for a picture
  (`SDL_VIDEODRIVER=offscreen` without a display): `launcher/main.cpp`.
