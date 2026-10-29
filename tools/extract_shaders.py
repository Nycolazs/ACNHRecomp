#!/usr/bin/env python3
"""Extracts every Maxwell shader container of the game's RomFS, plus the shader blocks embedded in `main`
(docs/ACNH.md, "Shaders"), on every OS.

usage: extract_shaders.py [game dir] [--jobs N] [--full]     (default: private/acnh; jobs: half the cores)
  --full  also extract every Model/ container whole (61k files; the blocks only models have are extracted anyway,
          by model_shaders.py)
Output: <game dir>/shaders_extracted/<group>/..., one group per container.

tools/maxwell/spirv_harness (`--extract <romfs> <main.bin> --out <dir>`, or `--prepare` to also translate) is a C# port
of this script, containers.py, exefs_shaders.py and model_shaders.py that writes the same tree; keep them in step.
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
X = os.path.join(ROOT, "tools", "maxwell", "containers.py")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game", nargs="?", default=os.path.join(ROOT, "private", "acnh"))
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--full", action="store_true")
    args = ap.parse_args()
    py = sys.executable
    r = os.path.join(args.game, "extract", "romfs")
    o = os.path.join(args.game, "shaders_extracted")
    if not os.path.isdir(r):
        sys.exit(f"no {r}: import the game first")
    os.makedirs(o, exist_ok=True)

    # One job per container: "<group> <container>". Group names come from the file name without the platform suffix.
    groups = []

    def grp(path, prefix):
        groups.append((f"{prefix}_{os.path.basename(path).split('.')[0]}", path))

    def files(d):
        d = os.path.join(r, d)
        return sorted(os.path.join(d, f) for f in os.listdir(d)) if os.path.isdir(d) else []

    for f in files("Shader"):  # Park_UBER_All (bfsha), Park_Proc (agl BAHS), apkg
        grp(f, "shader")
    for f in files("Effect"):  # VFXB with GRSR/GRSC BNSH
        grp(f, "effect")
    # System/: GSys and Agl resources (BAHS), the font and primitive renderer shaders (bare BNSH). The font shaders
    # and the primitive drawer hold the vertex shader of the final full-screen pass: without them nothing reaches
    # the window.
    for dirpath, _, names in sorted(os.walk(os.path.join(r, "System"))):
        for n in sorted(names):
            grp(os.path.join(dirpath, n), "system_" + os.path.basename(dirpath))
    for f in files(os.path.join("Swkbd", "shader")):
        if f.endswith("_Nx.arc.szs"):
            grp(f, "swkbd")
    for f in files("Layout"):  # 960 Ui2d archives, ~200 with bgsh
        grp(f, "layout")
    if args.full:
        for f in files("Model"):
            grp(f, "model")

    def extract(job):
        name, path = job
        with open(os.path.join(o, name + ".log"), "w") as log:
            rc = subprocess.call([py, X, path, "--extract", os.path.join(o, name), "--unique"], stdout=log,
                                 stderr=subprocess.STDOUT)
        return name, rc

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for name, rc in pool.map(extract, groups):
            if not rc:
                continue
            # Most layouts (and some other archives) hold no shader at all: not a failure.
            with open(os.path.join(o, name + ".log"), errors="replace") as log:
                if "no shader container recognised" in log.read():
                    continue
            print(f"FAIL {name}")

    # Containers without shaders leave an empty folder (or none): drop them and their logs.
    for d in os.listdir(o):
        p = os.path.join(o, d)
        if os.path.isdir(p) and not [f for f in os.listdir(p) if f != "manifest.tsv"]:
            shutil.rmtree(p, ignore_errors=True)
    for f in os.listdir(o):
        if f.endswith(".log") and not os.path.isdir(os.path.join(o, f[:-4])):
            os.remove(os.path.join(o, f))

    # The code blocks embedded in `main` that no container holds. After the others: it skips what they already have.
    with open(os.path.join(o, "exefs.log"), "w") as log:
        if subprocess.call([py, os.path.join(ROOT, "tools", "maxwell", "exefs_shaders.py"),
                            os.path.join(args.game, "analysis", "main.bin"), "--extract", os.path.join(o, "exefs")],
                           stdout=log, stderr=subprocess.STDOUT):
            print("FAIL exefs")
    # The variations only models carry (their own Park_UBER.bfsha): without them some materials draw depth only, black
    # (character faces, the Nook Stop). Last: it keeps only the blocks no other group has. ~15 min for 60k archives.
    with open(os.path.join(o, "model.log"), "w") as log:
        if subprocess.call([py, os.path.join(ROOT, "tools", "maxwell", "model_shaders.py"), os.path.join(r, "Model"), o,
                            "--jobs", str(args.jobs)], stdout=log, stderr=subprocess.STDOUT):
            print("FAIL model")
    ngroups = sum(1 for d in os.listdir(o) if os.path.isdir(os.path.join(o, d)))
    nblocks = sum(1 for _, _, fs in os.walk(o) for f in fs if f.endswith(".code.bin") or (f.startswith("code_") and f.endswith(".bin")))
    print(f"groups: {ngroups}  blocks: {nblocks}")


if __name__ == "__main__":
    main()
