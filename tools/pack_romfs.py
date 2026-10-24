#!/usr/bin/env python3
"""Packs a RomFS folder (private/<game>/extract/romfs) into one file, romfs.pak, read by the runtime instead of the
loose files (hle_fs.cpp, "packed RomFS"). On a phone's memory card the ~100,000 loose files took twice their size
(exFAT clusters of 128 KiB) and every open was a slow lookup.

usage: pack_romfs.py <romfs dir> <out file | - | --size> [--from N] [--to M]   ("-": stdout; --from/--to: only bytes N..M-1
       of the pack, to send it in parts, see tools/push_android.sh)

Format: "NXRP", u32 version 1, u32 count, then per file u16 path length, the UTF-8 path ('/' separated, relative to
the RomFS), u64 offset, u64 size; then the files' bytes, each at a 16-byte aligned offset."""
import os
import struct
import sys


class Range:
    """Writes only the bytes of the pack in [lo, hi) to f, counting every byte offered."""

    def __init__(self, f, lo, hi):
        self.f, self.lo, self.hi, self.pos = f, lo, hi, 0

    def write(self, b):
        a, z = max(self.lo, self.pos), min(self.hi, self.pos + len(b))
        if a < z:
            self.f.write(b[a - self.pos:z - self.pos])
        self.pos += len(b)

    def done(self):
        return self.pos >= self.hi

    def flush(self):
        self.f.flush()


def main():
    root, out = sys.argv[1], sys.argv[2]
    lo = int(sys.argv[sys.argv.index("--from") + 1]) if "--from" in sys.argv else 0
    hi = int(sys.argv[sys.argv.index("--to") + 1]) if "--to" in sys.argv else 1 << 62
    files = []
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            files.append((os.path.relpath(p, root).replace(os.sep, "/"), p, os.path.getsize(p)))
    files.sort()
    header = bytearray(b"NXRP" + struct.pack("<II", 1, len(files)))
    for rel, _, _ in files:
        header += struct.pack("<H", len(rel.encode())) + rel.encode() + b"\0" * 16  # offset and size, set below
    off = (len(header) + 15) & ~15
    entries, at = bytearray(b"NXRP" + struct.pack("<II", 1, len(files))), off
    layout = []
    for rel, p, size in files:
        entries += struct.pack("<H", len(rel.encode())) + rel.encode() + struct.pack("<QQ", at, size)
        layout.append((p, at, size))
        at = (at + size + 15) & ~15
    assert len(entries) == len(header)
    if out == "--size":  # the pack's size, without writing it
        print(layout[-1][1] + layout[-1][2] if layout else len(entries))  # the last file has no padding after it
        return
    f = Range(sys.stdout.buffer if out == "-" else open(out, "wb"), lo, hi)
    f.write(entries)
    pos = len(entries)
    total = len(layout)
    for k, (p, at, size) in enumerate(layout):
        if f.done():
            break
        f.write(b"\0" * (at - pos))
        if at + size <= lo:  # wholly before the range: counted, not read
            f.pos += size
            pos = at + size
            continue
        with open(p, "rb") as src:
            while True:
                chunk = src.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
        pos = at + size
        if k % 5000 == 0:
            print(f"pack_romfs: {k}/{total} files, {pos >> 20} MiB", file=sys.stderr, flush=True)
    f.flush()
    print(f"pack_romfs: {total} files, {pos >> 20} MiB", file=sys.stderr)


if __name__ == "__main__":
    main()
