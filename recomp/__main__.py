"""Recompile an NSO module to C++.

Usage: python -m recomp <module.nso> <out dir> --name main --base 0x4000000000 [--jobs N] [--only 0xADDR ...]
                       [--hooks <file>] [--variant <file.pchtxt>]

Writes (all symbols are prefixed with the module name, so several modules link into one program):
  funcs.h         NXB (the module's load address) and declarations of every recompiled function
  table.cpp       nx::ModuleInfo: guest offset -> function, relocations, symbols, init arrays, segments
  code_NNN.cpp    the functions, split into translation units
  report.txt      coverage: unhandled instructions, unresolved indirect branches
  code_variant.cpp  with --variant: functions only the patched code has (routines put in unused space)

--variant: a patch of the module in the IPS text form emulators read (.pchtxt: "@flag offset_shift", "@enabled"
sections, "OFFSET WORDS" lines). Every function the patch changes gets two bodies, the module's and the patched one,
and its symbol picks one at run time by nx_variant_on (the runtime's NX_SPEEDFIX); table.cpp registers the patched
words, which the loader writes into the module's image when the flag is on (data, constants, literal pools). One
build then runs either way. The patched module is analyzed on its own (patches move branches).
"""
import argparse
import bisect
import collections
import contextlib
import dataclasses
import io
import multiprocessing
import os
import re
import time

from . import analysis, emit, nso

INSNS_PER_FILE = 12000


@contextlib.contextmanager
def open_if_changed(path):
    """Like open(path, "w"), but leaves the file (and its timestamp) alone when the text is the same, so a rebuild
    after regenerating compiles only the files that changed."""
    buf = io.StringIO()
    yield buf
    text = buf.getvalue()
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("out")
    ap.add_argument("--name", default="main")
    ap.add_argument("--base", default="0x4000000000")
    ap.add_argument("--jobs", type=int, default=0, help="parallel emitters (default: cores, limited by free RAM)")
    ap.add_argument("--only", nargs="*", default=None, help="only these function addresses (debug)")
    ap.add_argument("--hooks", default=None,
                    help="file of instruction addresses (hex, one per line, '#' comments) that call nx_hook first")
    ap.add_argument("--variant", default=None, help="a .pchtxt patch of this module: a second body per changed "
                    "function, picked at run time (nx_variant_on)")
    args = ap.parse_args()
    base = int(args.base, 0)
    name = args.name

    t = time.time()
    m = nso.load(args.nso, name)
    an = analysis.analyze(m)
    print(f"analysis: {len(an.functions)} functions, {len(an.jump_tables)} jump tables ({time.time() - t:.1f}s)")

    os.makedirs(args.out, exist_ok=True)
    e = emit.Emitter(an, prefix=name)
    if args.hooks:
        with open(args.hooks) as hf:
            e.hooks = {int(ln.split("#")[0], 16) for ln in hf if ln.split("#")[0].strip()}
        print(f"  {len(e.hooks)} hooks from {args.hooks}")
    funcs = an.functions
    global _variant
    _variant = None
    if args.variant:
        _variant = build_variant(m, an, read_pchtxt(args.variant), name, e.hooks)
    if args.only:
        want = {int(x, 16) for x in args.only}
        funcs = [f for f in funcs if f.start in want]

    with open_if_changed(os.path.join(args.out, "funcs.h")) as h:
        # --base is an address in the reference layout (modules at 0x40_0000_0000); the build's NX_BASE (nx/cpu.h,
        # per host) moves all modules together, so the same generated code serves every platform.
        h.write(f"#pragma once\n#include <nx/cpu.h>\n\n#undef NXB\n#define NXB (NX_BASE + {base - REF_BASE:#x}ull)\n\n")
        for f in an.functions:
            h.write(f"void {e.fn(f.start)}(nx::CPU& c);\n")
        if _variant:
            h.write('\nextern "C" bool nx_variant_on;  // the runtime\'s NX_SPEEDFIX: the --variant bodies\n')
            h.write('extern "C" uint64_t nx_variant_lo, nx_variant_hi;  // NX_VARIANT_RANGE: only these (bisecting)\n')
            h.write("static inline bool nx_variant_ok(uint64_t a) { return a >= nx_variant_lo && a < nx_variant_hi; }\n")
            for f in _variant["new"]:
                h.write(f"void {e.fn(f.start)}(nx::CPU& c);\n")

    # Translation units: consecutive functions up to INSNS_PER_FILE instructions each, the same split whatever the
    # number of jobs, so a rerun rewrites only the files whose code changed.
    chunks, first, n = [], 0, 0
    for k, f in enumerate(funcs):
        n += (f.end - f.start) // 4
        if n >= INSNS_PER_FILE or k == len(funcs) - 1:
            chunks.append((len(chunks), first, k + 1))
            first, n = k + 1, 0
    files = [f"code_{i:03d}.cpp" for i, _, _ in chunks]
    global _job
    _job = (e, funcs, args.out)
    jobs = max(1, min(args.jobs or default_jobs(), len(chunks)))
    print(f"  emitting {len(funcs)} functions into {len(chunks)} files with {jobs} jobs")
    done = 0
    if jobs == 1:
        results = map(_emit_chunk, chunks)
    else:
        # Forked workers share the analysis (copy-on-write) instead of each redoing it.
        pool = multiprocessing.get_context("fork").Pool(jobs)
        results = pool.imap_unordered(_emit_chunk, chunks)
    for handled, unhandled, count in results:
        e.handled += handled
        e.unhandled.update(unhandled)
        done += count
        if done // 8000 != (done - count) // 8000:
            print(f"  emitted {done}/{len(funcs)} functions ({time.time() - t:.0f}s)", flush=True)
    if jobs > 1:
        pool.close()
        pool.join()

    if _variant:
        v = _variant
        with open_if_changed(os.path.join(args.out, "code_variant.cpp")) as out:
            out.write('#include "funcs.h"\n\n')
            out.write("\n\n".join(v["emitter"].function(f) for f in v["new"]))
            out.write("\n")
        print(f"  variant: {len(v['changed'])} functions with two bodies, {len(v['new'])} only in the patched code, "
              f"{len(v['words'])} words")
    elif os.path.exists(os.path.join(args.out, "code_variant.cpp")):
        os.remove(os.path.join(args.out, "code_variant.cpp"))
    write_table(os.path.join(args.out, "table.cpp"), m, an, e, name, base)

    with open(os.path.join(args.out, "report.txt"), "w") as out:
        out.write(f"functions {len(funcs)}, handled {e.handled}, unhandled {sum(e.unhandled.values())}\n")
        out.write(f"jump tables {len(an.jump_tables)}, unresolved br (indirect tail calls) {len(an.unresolved_br)}\n\n")
        for mn, n in e.unhandled.most_common():
            out.write(f"{n:8} {mn}\n")
    print(f"done: {len(files)} files, handled {e.handled}, unhandled {sum(e.unhandled.values())} "
          f"({time.time() - t:.0f}s)")
    for mn, n in e.unhandled.most_common(30):
        print(f"  {n:7} {mn}")


REF_BASE = 0x4000000000  # where --base addresses are measured from (NX_BASE of the reference layout)

_job = None  # (emitter, functions, out dir), set before the workers fork
_variant = None  # --variant: {"emitter", "changed": {start: patched function}, "new": [...], "words": [(off, word)]}


def read_pchtxt(path):
    """(module offset, 32-bit word) pairs of the enabled sections of a .pchtxt (IPS text) patch."""
    enabled, words = False, []
    for line in open(path):
        line = line.split("//")[0].strip()
        if not line:
            continue
        if line == "@enabled":
            enabled = True
        elif line == "@disabled":
            enabled = False
        elif line == "@stop":
            break
        elif enabled and (mm := re.fullmatch(r"([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)", line)):
            # The offsets are the module's; offset_shift is what a converter adds for the IPS (the NSO header).
            off, data = int(mm.group(1), 16), bytes.fromhex(mm.group(2))
            if len(data) % 4:
                raise SystemExit(f"{path}: {line}: not whole words")
            words += [(off + k, int.from_bytes(data[k:k + 4], "little")) for k in range(0, len(data), 4)]
    return words


def build_variant(m, an, words, name, hooks):
    """The patched module analyzed, and which of its functions differ from the module's."""
    t = time.time()
    m2 = dataclasses.replace(m, image=bytearray(m.image))
    for off, w in words:
        m2.image[off:off + 4] = w.to_bytes(4, "little")
    an2 = analysis.analyze(m2, patched=frozenset(off for off, _ in words if m.in_text(off)))
    e2 = emit.Emitter(an2, prefix=name)
    e2.hooks = hooks
    text = sorted(off for off, _ in words if m.in_text(off))
    def touched(f):
        k = bisect.bisect_left(text, f.start)
        return k < len(text) and text[k] < f.end
    base = {f.start for f in an.functions}
    by_start = {f.start: f for f in an2.functions}
    changed = {f.start: by_start[f.start] for f in an.functions
               if f.start in by_start and (touched(f) or touched(by_start[f.start]))}
    new = [f for f in an2.functions if f.start not in base]
    print(f"  variant analysis: {len(an2.functions)} functions ({time.time() - t:.0f}s)")
    return {"emitter": e2, "changed": changed, "new": new, "words": words}


def variant_function(e, f):
    """f's code; with --variant and f changed by the patch, both bodies and a dispatcher under f's symbol."""
    code = e.function(f)
    if not _variant or f.start not in _variant["changed"]:
        return code
    sym = e.fn(f.start)
    head = f"void {sym}(nx::CPU& NX_RESTRICT c) {{"
    v1 = _variant["emitter"].function(_variant["changed"][f.start])
    if head not in code or head not in v1:
        raise SystemExit(f"--variant: no header for {sym}")
    return (code.replace(head, f"static void {sym}__v0(nx::CPU& NX_RESTRICT c) {{", 1) + "\n\n" +
            v1.replace(head, f"static void {sym}__v1(nx::CPU& NX_RESTRICT c) {{", 1) + "\n\n" +
            f"{head}\n    if (nx_variant_on && nx_variant_ok({f.start:#x})) {sym}__v1(c); else {sym}__v0(c);\n}}")


def _emit_chunk(chunk):
    """One translation unit: returns its counts, the emitter's totals are summed by the parent."""
    i, lo, hi = chunk
    e, funcs, out_dir = _job
    e.handled, e.unhandled = 0, collections.Counter()
    code = [variant_function(e, f) for f in funcs[lo:hi]]
    with open_if_changed(os.path.join(out_dir, f"code_{i:03d}.cpp")) as out:
        out.write('#include "funcs.h"\n\n')
        out.write("\n\n".join(code))
        out.write("\n")
    return e.handled, e.unhandled, hi - lo


def default_jobs():
    """One job per core, as many as the free memory allows (a worker grows to about 300 MB)."""
    cores = os.cpu_count() or 1
    try:
        with open("/proc/meminfo") as f:
            avail = next(int(l.split()[1]) for l in f if l.startswith("MemAvailable:")) // 1024
    except (OSError, StopIteration):
        try:  # macOS
            avail = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") // (1 << 20) // 2
        except (ValueError, OSError):
            return 1
    return max(1, min(cores, (avail - 1000) // 300))


def write_table(path, m, an, e, name, base):
    with open_if_changed(path) as out:
        out.write('#include "funcs.h"\n#include <nx/module.h>\n\n')
        out.write("static const nx::FuncEntry funcs[] = {\n")
        # With --variant, also the starts only the patched code has (its routines, and where they branch back into
        # a function): indirect calls reach them by address. Sorted, as the loader searches it.
        entries = sorted([f.start for f in an.functions] + [f.start for f in (_variant["new"] if _variant else [])])
        for a in entries:
            out.write(f"    {{{a:#x}, {e.fn(a)}}},\n")
        out.write("};\n\n")
        out.write("static const nx::Reloc relocs[] = {\n")
        for r in m.relocs:
            out.write(f"    {{{r.offset:#x}, {r.type}, {r.sym}, {r.addend}ll}},\n")
        out.write("};\n\n")
        out.write("static const nx::Symbol symbols[] = {\n")
        for s in m.symbols:
            out.write(f"    {{{cstr(s.name)}, {s.value:#x}, {s.size}, {s.type}, {s.bind}, {int(s.imported)}}},\n")
        out.write("};\n\n")
        ia = ", ".join(f"{a:#x}" for a in m.init_array + [0])
        fa = ", ".join(f"{a:#x}" for a in m.fini_array + [0])
        out.write(f"static const uint64_t init_array[] = {{{ia}}};\n")
        out.write(f"static const uint64_t fini_array[] = {{{fa}}};\n\n")
        out.write(f"extern const nx::ModuleInfo nx_module_{name} = {{\n")
        out.write(f"    {cstr(name)}, NX_BASE + {base - REF_BASE:#x}ull,\n")
        out.write(f"    {m.text[0]:#x}, {m.text[1]:#x}, {m.ro[0]:#x}, {m.ro[1]:#x}, "
                  f"{m.data[0]:#x}, {m.data[1]:#x}, {m.bss[0]:#x}, {m.bss[1]:#x},\n")
        out.write(f"    funcs, {len(entries)},\n")
        out.write(f"    relocs, {len(m.relocs)},\n")
        out.write(f"    symbols, {len(m.symbols)},\n")
        out.write(f"    init_array, {len(m.init_array)}, fini_array, {len(m.fini_array)},\n")
        out.write("};\n")
        if _variant:  # (offset, word) pairs, written into the image by the loader when the variant is on
            out.write("\n#include <nx/runtime.h>\n")
            w = ", ".join(f"{o:#x}, {x:#010x}" for o, x in _variant["words"])
            out.write(f"\nstatic const uint32_t variant_words[] = {{{w}}};\n")
            out.write(f"static nx::VariantRegistrar variant_registrar({cstr(name)}, variant_words, "
                      f"{len(_variant['words'])});\n")


if __name__ == "__main__":
    main()
