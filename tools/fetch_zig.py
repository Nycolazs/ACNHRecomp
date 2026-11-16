#!/usr/bin/env python3
"""Puts the zig that compiles the game's library (prepare/gamelib/: the recompiled code for each platform) into a
folder, trimmed to what the four targets use: the libc headers of Linux (glibc, musl), Windows (MinGW) and macOS,
libc++, compiler-rt, and the standard library zig builds them with. About a third of the full download. The archive is
checked against the SHA-256 ziglang.org publishes for it (pinned below), and the folder gets sizes.txt, the size of
every file kept, which the preparation checks before compiling (prepare/gamelib/gamelib.cpp: check_toolchain), so a
file an antivirus or a cleaner emptied or removed is told at once rather than as a compile error.

Taken from ACNHRecomp-Importer v1.0.7 (tools/fetch_zig.py, MIT, same author), with the checksums and sizes.txt added.

usage: fetch_zig.py <host> <dest> [--android]   host: x86_64-linux, aarch64-linux (also Android's), x86_64-windows,
                                                      aarch64-macos
       fetch_zig.py --trim <zig folder> [--android]   (an unpacked zig, trimmed in place, sizes.txt written)
  --android: only what the android-arm64 target uses (musl's headers, libc++, compiler-rt, the standard library):
             the Android app's zig (tools/build_android.sh), which builds the phone's own library and no other
             platform's. About 75 MB instead of 175 MB, unpacked on the phone from the APK.
"""
import hashlib
import io
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile

VERSION = "0.17.0"

# https://ziglang.org/download/index.json, "0.17.0": shasum of each archive
SHA256 = {
    "x86_64-linux": "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026",
    "aarch64-linux": "9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8",
    "x86_64-windows": "b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428",
    "aarch64-macos": "b607e9b9234790a008116ae5bdb71c6243b84b9fb42a53a9e70fde41c06c536a",
}

# lib/libc/include: the folders the targets' headers come from (gamelib.cpp: x86_64-linux-gnu, aarch64-linux-musl,
# x86_64-windows-gnu, aarch64-macos)
KEEP_INCLUDE = {
    "aarch64-linux-any", "aarch64-linux-gnu", "aarch64-linux-musl", "x86-linux-any", "x86-linux-gnu",
    "x86_64-linux-musl", "any-linux-any", "any-windows-any", "any-darwin-any", "generic-glibc", "generic-musl",
}
DROP = ["doc", "lib/docs", "lib/libtsan", "lib/libc/wasi", "lib/libc/freebsd", "lib/libc/netbsd", "lib/libc/openbsd"]
# --android: android-arm64 alone (gamelib.cpp: aarch64-linux-musl, -nostdlib), without Windows', macOS' and glibc's
ANDROID_KEEP_INCLUDE = {"aarch64-linux-any", "aarch64-linux-musl", "any-linux-any", "generic-musl"}
ANDROID_DROP = ["lib/libc/mingw", "lib/libc/darwin", "lib/libc/glibc"]


def write_sizes(root):
    """sizes.txt: "<size>\t<path>" for every file of the folder (paths relative, '/' separated), sizes.txt itself
    left out."""
    lines = []
    for d, _, files in os.walk(root):
        for name in files:
            p = os.path.join(d, name)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            if rel != "sizes.txt":
                lines.append(f"{os.path.getsize(p)}\t{rel}\n")
    lines.sort(key=lambda s: s.split("\t", 1)[1])
    with open(os.path.join(root, "sizes.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# zig {VERSION}, trimmed by tools/fetch_zig.py: the size of each file\n")
        f.writelines(lines)


def trim(root, android=False):
    for d in DROP + (ANDROID_DROP if android else []):
        shutil.rmtree(os.path.join(root, d), ignore_errors=True)
    inc = os.path.join(root, "lib", "libc", "include")
    for name in os.listdir(inc):
        if name not in (ANDROID_KEEP_INCLUDE if android else KEEP_INCLUDE):
            shutil.rmtree(os.path.join(inc, name))
    write_sizes(root)


def fetch(host, dest, android=False):
    if host not in SHA256:
        sys.exit(f"unknown host {host}: one of {', '.join(SHA256)}")
    dest = os.path.abspath(dest)
    base = f"zig-{host}-{VERSION}"
    ext = "zip" if "windows" in host else "tar.xz"
    url = f"https://ziglang.org/download/{VERSION}/{base}.{ext}"
    print("downloading", url, flush=True)
    data = urllib.request.urlopen(url, timeout=120).read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SHA256[host]:
        sys.exit(f"{url}: SHA-256 {digest}, expected {SHA256[host]} (a damaged or changed download): nothing written")
    tmp = dest + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    if ext == "zip":
        zipfile.ZipFile(io.BytesIO(data)).extractall(tmp)
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as t:
            t.extractall(tmp, filter="tar")
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.move(os.path.join(tmp, base), dest)
    shutil.rmtree(tmp)
    trim(dest, android)
    total = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(dest) for f in fs)
    print(f"zig {VERSION} in {dest}: {total / 1e6:.0f} MB", flush=True)


def main():
    args = sys.argv[1:]
    android = "--android" in args
    args = [a for a in args if a != "--android"]
    if len(args) == 2 and args[0] == "--trim":
        trim(args[1], android)
    elif len(args) == 2 and not args[0].startswith("-"):
        fetch(args[0], args[1], android)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
