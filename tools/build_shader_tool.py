#!/usr/bin/env python3
"""Builds the shader tool the game folder's preparation runs (prepare/prepare.cpp, the Shaders stage): this
repository's tools/maxwell/spirv_harness (Ryujinx's shader translator, MIT, with ACNHRecomp's patches, and the
extraction of the game's shaders), compiled ahead of time into one native executable (.NET NativeAOT, so no .NET is
needed where it runs). It is run as
    spirv_harness --prepare <romfs> <analysis/main.bin> --out <work> --spirv <game folder>/spirv --jobs N --no-val

Taken from ACNHRecomp-Importer v1.0.7 (tools/build_shader_tool.py, MIT, same author), building this checkout's
harness instead of a separate one.

usage: build_shader_tool.py <rid> <dest folder> [ACNHRecomp checkout (default: this one)]
  rid: linux-x64, win-x64, osx-arm64, linux-bionic-arm64 (Android: the NDK's clang on PATH, ANDROID_NDK_ROOT set)
Needs the .NET 10 SDK. The translator's sources (Ryubing, at the commit tools/setup_acnh.py pins) are downloaded into
the checkout's private/third_party/ryubing when missing, checked against their SHA-256 (RYUBING_SHA256).
"""
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The SHA-256 of Codeberg's archive (.tar.gz) of each Ryubing commit tools/setup_acnh.py may pin (RYUBING): what is
# compiled into the shader tool every package carries is exactly that. A new commit there needs its hash here.
RYUBING_SHA256 = {
    "a82350bb77": "aba9789bdcf3fda9e90f965636e9e85e9dcc299333fbf5451bcf22ecfd608506",
}


def fetch_ryubing(repo):
    dest = os.path.join(repo, "private", "third_party", "ryubing")
    if os.path.exists(os.path.join(dest, "Ryujinx.sln")):
        return
    with open(os.path.join(repo, "tools", "setup_acnh.py"), encoding="utf-8") as f:
        commit = re.search(r'^RYUBING = "([0-9a-f]+)"', f.read(), re.M).group(1)
    want = RYUBING_SHA256.get(commit)
    if not want:
        sys.exit(f"Ryubing {commit} (tools/setup_acnh.py) has no SHA-256 in RYUBING_SHA256 (tools/build_shader_tool.py)")
    url = f"https://codeberg.org/Software_arch/Ryubing/archive/{commit}.tar.gz"
    print("downloading", url, flush=True)
    data = urllib.request.urlopen(url, timeout=120).read()
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        sys.exit(f"{url}: SHA-256 {got}, not the expected {want}: not extracted")
    os.makedirs(dest, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
        members = []
        for m in t.getmembers():
            parts = m.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            m.name = parts[1]
            members.append(m)
        t.extractall(dest, members=members, filter="tar")


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    rid, dest = sys.argv[1:3]
    repo = os.path.abspath(sys.argv[3] if len(sys.argv) == 4 else HERE)
    fetch_ryubing(repo)
    out = os.path.join(repo, "private", "build", f"shader-tool-{rid}")
    cmd = ["dotnet", "publish", os.path.join(repo, "tools", "maxwell", "spirv_harness", "SpirvHarness.csproj"),
           "-c", "Release", "-r", rid, "-p:PublishAot=true", "-p:InvariantGlobalization=true",
           "-p:DebugType=none", "-p:StripSymbols=true", "-o", out]
    if "bionic" in rid:
        cmd += ["-p:DisableUnsupportedError=true", "-p:PublishAotUsingRuntimePack=true"]
    print(" ".join(cmd), flush=True)
    # MSBuild takes environment variables as properties, names in any case: the release workflow's VERSION (v0.11.0)
    # became the packages' Version, which NuGet's restore refused ("not a valid version string")
    env = {k: v for k, v in os.environ.items() if k.upper() not in ("VERSION", "VERSIONPREFIX", "VERSIONSUFFIX",
                                                                      "PACKAGEVERSION", "CONFIGURATION", "PLATFORM")}
    env["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1"
    subprocess.check_call(cmd, env=env)
    exe = "spirv_harness.exe" if rid.startswith("win") else "spirv_harness"
    os.makedirs(dest, exist_ok=True)
    shutil.copy2(os.path.join(out, exe), os.path.join(dest, exe))
    print("shader tool:", os.path.join(dest, exe), os.path.getsize(os.path.join(dest, exe)), "bytes")


if __name__ == "__main__":
    main()
