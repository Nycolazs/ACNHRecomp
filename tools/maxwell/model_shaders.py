#!/usr/bin/env python3
"""The shader code blocks that only models carry, as one extraction group.

Many Model/*.Nin_NX_NVN.zs archives (ACNH 3.0.2) hold their own Park_UBER.Nin_NX_NVN.bfsha: the same shading model as
Shader/Park_UBER_All, but with the variations that model's materials use, and some of those are in no other
container (15 fragment variants seen on the island: character faces, the Nook Stop, tulips...). Without them a
program loses its fragment stage and draws depth only, a black silhouette. A full extraction of every model would
write the same blocks tens of thousands of times, so this keeps only what the other groups do not have.

Usage: model_shaders.py <romfs>/Model <shaders_extracted> [--jobs N]
Writes <shaders_extracted>/model/{code_<hash>.bin, ctrl_<hash>.bin, manifest.tsv}, the layout spirv_harness --batch reads.
"""
import hashlib
import multiprocessing
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import containers  # noqa: E402

GROUP = "model"


def known_hashes(root):
    known = set()
    for dirpath, _, files in os.walk(root):
        if "manifest.tsv" not in files or os.path.basename(dirpath) == GROUP:
            continue
        with open(os.path.join(dirpath, "manifest.tsv")) as m:
            next(m, None)
            for line in m:
                f = line.rstrip("\n").split("\t")
                if len(f) > 10:
                    known.add(f[10])
    return known


_known = set()


def _init(known):
    global _known
    _known = known


def scan(path):
    """The stages of one model file whose code no other group has: (manifest fields, code, control)."""
    try:
        data = containers.read_file(path)
        c = containers.open_blob(data, 0, len(data), os.path.basename(path))
    except Exception as e:  # a model the parsers do not know is not a shader container
        return path, [], f"{type(e).__name__}: {e}"
    out, seen = [], set()
    if c:
        for s in containers.all_stages(c):
            if s.hash in _known or s.hash in seen:
                continue
            seen.add(s.hash)
            src = s.data if s.data is not None else data
            code = bytes(src[s.code_off:s.code_off + s.code_size])
            ctrl = bytes(src[s.ctrl_off:s.ctrl_off + s.ctrl_size])
            out.append(((s.id, s.kind, s.code_off, s.code_size, s.ctrl_off, s.ctrl_size, s.start, s.length,
                         s.const_off, s.const_size, s.hash), code, ctrl))
    return path, out, ""


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    models, root = args[0], args[1]
    jobs = int(args[args.index("--jobs") + 1]) if "--jobs" in args else max(1, (os.cpu_count() or 2) // 2)
    out = os.path.join(root, GROUP)
    real = os.path.realpath(out)
    if "--allow-external" not in args and not real.startswith(os.path.realpath(os.path.join(containers.REPO, "private")) + os.sep):
        sys.exit(f"refusing to write outside private/: {out}")
    known = known_hashes(root)
    files = sorted(os.path.join(models, f) for f in os.listdir(models) if f.endswith(".Nin_NX_NVN.zs"))
    print(f"{len(files)} model archives, {len(known)} code blocks known from the other groups", flush=True)
    os.makedirs(real, exist_ok=True)
    written, rows, failed = set(), 0, 0
    with open(os.path.join(real, "manifest.tsv"), "w", newline="\n") as m, \
            multiprocessing.Pool(jobs, _init, (known,)) as pool:
        m.write("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\t"
                "const_size\tcode_sha1_16\tcode_file\tctrl_file\n")
        for n, (path, stages, error) in enumerate(pool.imap_unordered(scan, files, chunksize=16), 1):
            failed += bool(error)
            for f, code, ctrl in stages:
                h = f[10]
                if h in written:
                    continue
                code_file = f"code_{h}.bin"
                ctrl_file = f"ctrl_{hashlib.sha1(ctrl).hexdigest()[:16]}.bin"
                open(os.path.join(real, code_file), "wb").write(code)
                open(os.path.join(real, ctrl_file), "wb").write(ctrl)
                written.add(h)
                sid = os.path.basename(path) + "/" + f[0].split("/", 1)[-1]
                m.write(f"{sid}\t{f[1]}\t{f[2]:#x}\t{f[3]:#x}\t{f[4]:#x}\t{f[5]:#x}\t{f[6]:#x}\t{f[7]:#x}\t"
                        f"{f[8]:#x}\t{f[9]:#x}\t{h}\t{code_file}\t{ctrl_file}\n")
                rows += 1
            if n % 2000 == 0:
                print(f"  {n}/{len(files)} archives, {rows} new code blocks", flush=True)
    print(f"{rows} code blocks only in models -> {out} ({failed} archives not parsed)")


if __name__ == "__main__":
    main()
