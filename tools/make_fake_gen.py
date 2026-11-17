#!/usr/bin/env python3
"""Writes a stand-in for the recompiled code (<game folder>/gen/) in the recompiler's layout, nothing of any game in
it: gen/<module>/funcs.h, code_NNN.cpp and table.cpp for main, sdk and subsdk0, so the game's library can be built and
linked for every platform as the preparation does (prepare/gamelib/), with as many files as the real game by default
(the test of the compile step: command lines, response files, folders with spaces; prepare/prepare_e2e_test.cpp).

Taken from ACNHRecomp-Importer v1.0.7 (tools/make_fake_gen.py, MIT, same author).

usage: make_fake_gen.py <game folder> [files of main (default 1380)]
"""
import os
import sys

MODULE_INFO = """extern const nx::ModuleInfo nx_module_{m} = {{
    "{m}", NX_BASE + 0x{base:x}ull,
    0x0, 0x{size:x}, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    funcs, {n},
    nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
}};
"""


def module(gen, m, base, files, per_file=4):
    d = os.path.join(gen, m)
    os.makedirs(d, exist_ok=True)
    offs = [0x10 * k for k in range(files * per_file)]
    with open(os.path.join(d, "funcs.h"), "w") as f:
        f.write("#pragma once\n#include <nx/cpu.h>\n\n#undef NXB\n#define NXB (NX_BASE + 0x%xull)\n\n" % base)
        for o in offs:
            f.write(f"void {m}_{o:08X}(nx::CPU& c);\n")
    for k in range(files):
        with open(os.path.join(d, f"code_{k:03d}.cpp"), "w") as f:
            f.write('#include "funcs.h"\n\n')
            for o in offs[k * per_file:(k + 1) * per_file]:
                f.write(f"void {m}_{o:08X}(nx::CPU& NX_RESTRICT c) {{\n"
                        f"    uint64_t x0 = c.x[0], x1 = c.x[1];\n"
                        f"    x0 = uint64_t(x0 + x1 * 0x{o + 3:x}ull); // add\n"
                        f"    c.x[0] = x0;\n"
                        f"}}\n\n")
    with open(os.path.join(d, "table.cpp"), "w") as f:
        f.write('#include "funcs.h"\n#include <nx/module.h>\n\nstatic const nx::FuncEntry funcs[] = {\n')
        for o in offs:
            f.write(f"    {{0x{o:x}, {m}_{o:08X}}},\n")
        f.write("};\n\n" + MODULE_INFO.format(m=m, base=base, size=offs[-1] + 0x10, n=len(offs)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gen = os.path.join(sys.argv[1], "gen")
    files = int(sys.argv[2]) if len(sys.argv) > 2 else 1380
    module(gen, "main", 0x0, files)
    module(gen, "sdk", 0x10000000, max(1, files // 10))
    module(gen, "subsdk0", 0x18000000, max(1, files // 19))
    # what the recompile stage leaves beside the code (prepare.cpp): the preparation then takes gen/ for its own
    with open(os.path.join(gen, "recomp_source.txt"), "w") as f:
        f.write("a stand-in (tools/make_fake_gen.py)\n")
    with open(os.path.join(sys.argv[1], "game.ini"), "w") as f:
        f.write("# a stand-in (tools/make_fake_gen.py)\nbuild_id = 00000000000000000000000000000000\n")


if __name__ == "__main__":
    main()
