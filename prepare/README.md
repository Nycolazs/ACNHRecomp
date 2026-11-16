# prepare/

The game folder's preparation, inside ACNHRecomp: from the player's extracted ExeFS and RomFS, ACNHRecomp recompiles
the modules, converts the shaders, compiles the game's library with a bundled zig and writes the game folder, with no
ACNHRecomp-Importer and never keys, NSP or XCI. So far it holds the recompiler.

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

`recomp/recomp.cmake` builds them; the top `CMakeLists.txt` includes it on desktop, except in a build of the generated
code (`NX_GEN_PROBE`). Tests (`-DNX_TESTS=ON`): `ctest -R recomp`.

## The source guard

The Python and the C++ must generate the same code. `recomp/SOURCE` holds the version of `recomp/*.py` this port
matches: sha256 over `name\0content\0` of `recomp/*.py` in name order, as `tools/setup_acnh.py`'s `recomp_hash()`
computes it (and the Importer wrote it as `gen/recomp_source.txt`). `recomp_source_test` fails when `recomp/*.py` no
longer has that hash. A change to the Python recompiler is then ported to `recomp/` here, checked to give the same
output (`python -m recomp` and `acnh_recomp` with the same arguments on the game's modules), and the new hash, which
the test prints, goes into `SOURCE`.
