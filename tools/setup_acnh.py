#!/usr/bin/env python3
"""From your game folder to a native build, every stage in order (README, "Installation"), on Linux, macOS and Windows.
Each stage is skipped when its output is already there, so it can be run again after a failure or an update of the
repository. Lines starting with "== " name the stage that begins: the launcher (launcher/pipeline.cpp) follows them.

usage: python3 tools/setup_acnh.py --game <game folder> [--mod <name>=<zip>]... [--out <folder>]
       python3 tools/setup_acnh.py [--out <folder>]          (later runs: the game folder is linked, the rest is checked)
  The game folder holds the game's executables and files as your own console dump gives them: exefs/ (main, sdk,
  subsdk0, main.npdm) and romfs/ (the update applied; extract/romfs/ is accepted too), and aoc/<index>/ for add-on
  content. It is linked into the data folder, not copied: keep it where it is. Any version of the game builds
  (games/acnh/versions.txt, docs/VERSIONS.md); one version per --out folder.
  env: JOBS=<compile jobs> (default: free RAM / 700 MB, at most the core count)   FORCE=<stage> to redo one stage
       (game, recomp, shaders, harness, glyphs)

Stages: tools -> game -> version -> recompile -> shaders -> fonts -> build. Everything it writes stays in private/
(gitignored) and the --out folder. On Windows the tools stage also fetches LLVM (clang-cl, lld) and SDL3 into
private/third_party and uses the Visual Studio C++ build tools found by vswhere (the Windows SDK and C runtime).
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P = os.path.join(ROOT, "private")
T = os.path.join(P, "third_party")
MOVIE_LIBS = os.path.join(T, "vcpkg", "installed", "x64-windows-static-md")  # libvpx, libvorbis (vcpkg)
WIN = os.name == "nt"
MAC = sys.platform == "darwin"

VK_HEADERS = "1.4.357"
LLVM = "23.1.3"  # Windows only: clang-cl and lld-link (Linux and macOS use the system's clang)
SDL3 = "3.4.16"  # Windows only: SDL's VC development package (Linux and macOS: the system's SDL3)
IMGUI = "1.91.9"
RYUBING = "a82350bb77"
DOTNET = "10.0.401"


def say(stage):
    print(f"\n== {stage}", flush=True)


def fail(msg):
    print(msg, flush=True)
    sys.exit(1)


def force(stage):
    return os.environ.get("FORCE") == stage


def run(cmd, log=None, env=None, cwd=ROOT, check=True):
    """Runs cmd (a list); output to the log file when given (its last lines are shown on failure)."""
    exe = shutil.which(cmd[0], path=(env or os.environ).get("PATH")) or cmd[0]
    if log:
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            rc = subprocess.call([exe] + cmd[1:], stdout=f, stderr=subprocess.STDOUT, env=env, cwd=cwd)
        if rc and check:
            with open(log, encoding="utf-8", errors="replace") as f:
                print("".join(f.readlines()[-20:]))
            fail(f"failed: {' '.join(cmd[:3])} ... (log: {log})")
        return rc
    rc = subprocess.call([exe] + cmd[1:], env=env, cwd=cwd)
    if rc and check:
        fail(f"failed: {' '.join(cmd[:3])} ...")
    return rc


def fetch(url, dest_dir, strip=True):
    """Downloads a .tar.gz / .tar.xz / .zip and extracts it into dest_dir (its single top folder removed if strip)."""
    os.makedirs(dest_dir, exist_ok=True)
    tmp = dest_dir + ".download"
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    if url.endswith(".zip"):
        with zipfile.ZipFile(tmp) as z:
            names = z.namelist()
            top = names[0].split("/")[0] + "/"
            for i in z.infolist():
                n = i.filename[len(top):] if strip and i.filename.startswith(top) else i.filename
                if not n or n.startswith("/") or ".." in n.split("/"):
                    continue
                out = os.path.join(dest_dir, n)
                if i.is_dir():
                    os.makedirs(out, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with z.open(i) as a, open(out, "wb") as b:
                    shutil.copyfileobj(a, b)
    else:
        with tarfile.open(tmp) as t:
            members = []
            for m in t.getmembers():
                parts = m.name.split("/", 1) if strip else [None, m.name]
                if len(parts) < 2 or not parts[1]:
                    continue
                m.name = parts[1]
                if m.islnk() and strip:  # hard links name their target with the top folder too
                    m.linkname = m.linkname.split("/", 1)[-1]
                members.append(m)
            t.extractall(dest_dir, members=members, filter="data")
    os.remove(tmp)


def free_mb():
    if WIN:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                        ("avail", ctypes.c_ulonglong), ("tpf", ctypes.c_ulonglong), ("apf", ctypes.c_ulonglong),
                        ("tv", ctypes.c_ulonglong), ("av", ctypes.c_ulonglong), ("aev", ctypes.c_ulonglong)]

        ms = MS()
        ms.len = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms))
        return ms.avail // (1 << 20)
    if MAC:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") // (1 << 21)
    with open("/proc/meminfo") as f:
        return next(int(l.split()[1]) for l in f if l.startswith("MemAvailable:")) // 1024


def msvc_env():
    """The environment of a "x64 Native Tools" prompt (Visual Studio's vcvars64.bat), LLVM first on PATH."""
    vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
    inst = ""
    if os.path.exists(vswhere):
        inst = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                               "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                              capture_output=True, text=True).stdout.strip()
    bat = os.path.join(inst, r"VC\Auxiliary\Build\vcvars64.bat") if inst else ""
    if not bat or not os.path.exists(bat):
        fail("missing: the Visual Studio C++ build tools (the Windows SDK and C runtime clang-cl links against).\n"
             "Install them with:\n  winget install Microsoft.VisualStudio.2022.BuildTools --override "
             "\"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"")
    out = subprocess.run(f'cmd /s /c ""{bat}" >nul 2>&1 && set"', capture_output=True, text=True, errors="replace").stdout
    env = dict(l.split("=", 1) for l in out.splitlines() if "=" in l and not l.startswith("="))
    # Windows spells it "Path" (from Explorer and the launcher), shells often "PATH": one key, whatever the case.
    paths = [env.pop(k) for k in [k for k in env if k.upper() == "PATH"]]
    path = paths[0] if paths else ""
    env["PATH"] = os.pathsep.join([os.path.join(T, f"llvm-{LLVM}", "bin"), os.path.join(P, "venv", "Scripts"), path])
    return env


def find_game(game):
    """The exefs, romfs and aoc folders of a game folder (an ExeFS + RomFS dump: names in either case; the RomFS may
    also be extract/romfs), checked: the three modules are NSOs and the RomFS is the game's."""
    def sub_dir(*names):
        for n in names:
            for e in (os.listdir(game) if os.path.isdir(game) else []):
                if e.lower() == n.lower() and os.path.isdir(os.path.join(game, e)):
                    return os.path.join(game, e)
        return ""
    if not os.path.isdir(game):
        fail(f"{game}: not a folder")
    exefs = sub_dir("exefs")
    romfs = sub_dir("romfs") or (os.path.join(game, "extract", "romfs") if os.path.isdir(os.path.join(game, "extract", "romfs")) else "")
    if not exefs or not romfs:
        fail(f"{game} must hold exefs/ and romfs/ (the game's executables and files from your own dump)")
    for m in ("main", "sdk", "subsdk0"):
        f = os.path.join(exefs, m)
        if not os.path.isfile(f):
            fail(f"{exefs} has no {m}")
        with open(f, "rb") as h:
            if h.read(4) != b"NSO0":
                fail(f"{f} is not an executable (NSO): the ExeFS of a dump holds them as they are")
    if not os.path.isdir(os.path.join(romfs, "Shader")):
        fail(f"{romfs} has no Shader/ folder: not Animal Crossing: New Horizons' RomFS")
    aoc = sub_dir("aoc")
    return exefs, romfs, aoc


def is_link(p):
    return os.path.islink(p) or (WIN and os.path.isdir(p) and hasattr(os.path, "isjunction") and os.path.isjunction(p))


def is_real_dir(p):
    return os.path.isdir(p) and not is_link(p)


def unlink(p):
    """Removes a link (a symbolic link, or a directory junction on Windows), never what it points to."""
    if os.path.islink(p):
        os.unlink(p)
    elif WIN and os.path.isdir(p):
        os.rmdir(p)  # a junction: rmdir removes the junction itself


def link(target, p):
    """p made a link to the folder target: a symbolic link, or on Windows a directory junction (no rights needed)."""
    if is_link(p):
        if os.path.realpath(p) == os.path.realpath(target):
            return
        unlink(p)
    elif os.path.isdir(p):
        shutil.rmtree(p)  # an imported copy, replaced with FORCE=game (checked by the caller)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if WIN:
        import _winapi
        _winapi.CreateJunction(target, p)
    else:
        os.symlink(target, p, target_is_directory=True)


def fix_archives(romfs):
    """A mod's .sarc.zs archives as the game reads them (tools/sarc_endian.py): a zstd frame without its decompressed
    size, or little-endian headers, crashed the game at boot (a translation tool's output)."""
    if not shutil.which("zstd"):
        print("zstd not found: the mod's archives are not checked")
        return
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import sarc_endian
    fixed = 0
    for base, _, files in os.walk(romfs):
        for f in files:
            if not f.endswith(".sarc.zs"):
                continue
            path = os.path.join(base, f)
            info = subprocess.run(["zstd", "-lv", path], capture_output=True, text=True).stdout
            data = sarc_endian.read(path)
            if "Decompressed Size" in info and "XXH64" not in info and data[6:8] == b"\xfe\xff":
                continue
            subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "sarc_endian.py"), path, path + ".tmp.zs"],
                           check=True)
            os.replace(path + ".tmp.zs", path)
            fixed += 1
    if fixed:
        print(f"{fixed} archives rewritten as the game reads them (size in the zstd frame, big-endian)")


def rewrap_messages(game_romfs, romfs):
    """A translation's dialogue lines wider than the text boxes, re-flowed (tools/msg_rewrap.py), so the game does not
    break them by itself and leave a word alone on its line. Needs Pillow (to measure with the game's font)."""
    if not os.path.isdir(os.path.join(romfs, "Message")) or not shutil.which("zstd"):
        return
    if subprocess.run([sys.executable, "-c", "import PIL"], capture_output=True).returncode:
        print("Pillow not found (python3 -m pip install pillow): the translation's long lines are left as they are")
        return
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "msg_rewrap.py"), game_romfs, romfs], check=True)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--game", default="", help="the game folder: exefs/ and romfs/ (and aoc/) of your own dump")
    ap.add_argument("--mod", action="append", default=[])
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    G = os.path.abspath(a.out) if a.out else os.path.join(P, "acnh")
    os.makedirs(os.path.join(G, "logs"), exist_ok=True)
    os.makedirs(T, exist_ok=True)
    logs = os.path.join(G, "logs")

    cores = os.cpu_count() or 1
    jobs = int(os.environ.get("JOBS") or max(1, min(cores, (free_mb() - 1000) // 700)))

    # ---- tools --------------------------------------------------------------------------------------------------
    say("tools")
    py = os.path.join(P, "venv", "Scripts" if WIN else "bin", "python.exe" if WIN else "python")
    if not os.path.exists(py):
        run([sys.executable, "-m", "venv", os.path.join(P, "venv")])
    if subprocess.call([py, "-c", "import capstone, lz4, zstandard, fontTools, cmake, ninja"],
                       stderr=subprocess.DEVNULL):
        run([py, "-m", "pip", "install", "-q", "capstone", "lz4", "zstandard", "fonttools", "cmake", "ninja", "unicorn"])
    bindir = os.path.dirname(py)
    cmake = os.path.join(bindir, "cmake.exe" if WIN else "cmake")
    ninja = os.path.join(bindir, "ninja.exe" if WIN else "ninja")
    vkh = os.path.join(T, f"Vulkan-Headers-{VK_HEADERS}")
    if not os.path.isdir(vkh):
        fetch(f"https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v{VK_HEADERS}.tar.gz", vkh)
    if not os.path.exists(os.path.join(T, "imgui", "imgui.cpp")):  # the settings screen (Dear ImGui, MIT)
        fetch(f"https://github.com/ocornut/imgui/archive/refs/tags/v{IMGUI}.tar.gz", os.path.join(T, "imgui"))
    if not os.path.exists(os.path.join(T, "ryubing", "Ryujinx.sln")):
        fetch(f"https://codeberg.org/Software_arch/Ryubing/archive/{RYUBING}.tar.gz", os.path.join(T, "ryubing"))
    if WIN:
        llvm = os.path.join(T, f"llvm-{LLVM}")
        if not os.path.exists(os.path.join(llvm, "bin", "clang-cl.exe")):
            print(f"downloading LLVM {LLVM} (about 860 MB)", flush=True)
            fetch(f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM}/"
                  f"clang+llvm-{LLVM}-x86_64-pc-windows-msvc.tar.xz", llvm)
        sdl = os.path.join(T, f"SDL3-{SDL3}")
        if not os.path.isdir(os.path.join(sdl, "cmake")):
            fetch(f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL3}/SDL3-devel-{SDL3}-VC.zip", sdl)
        env = msvc_env()
        cc = ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
        # libvpx and libvorbis decode the game's movies (runtime/src/hle_movie.cpp; the flight to the island is a VP9
        # movie): static libraries built by vcpkg the first time, a few minutes. Without git the movies are black.
        vcpkg = os.path.join(T, "vcpkg")
        if not os.path.exists(os.path.join(MOVIE_LIBS, "lib", "vpx.lib")):
            if not os.path.exists(os.path.join(vcpkg, "vcpkg.exe")) and shutil.which("git"):
                if not os.path.isdir(vcpkg):
                    run(["git", "clone", "-q", "--depth", "1", "https://github.com/microsoft/vcpkg.git", vcpkg])
                run([os.path.join(vcpkg, "bootstrap-vcpkg.bat"), "-disableMetrics"], log=os.path.join(logs, "vcpkg.log"))
            if os.path.exists(os.path.join(vcpkg, "vcpkg.exe")):
                print("building libvpx and libvorbis (the game's movies; a few minutes, once)", flush=True)
                run([os.path.join(vcpkg, "vcpkg.exe"), "install", "libvpx:x64-windows-static-md",
                     "libvorbis:x64-windows-static-md", "--disable-metrics"], log=os.path.join(logs, "vcpkg.log"),
                    env=env, cwd=vcpkg)
            else:
                print("git not found: no libvpx, the game's movies will be black", flush=True)
    else:
        for tool in ("clang", "clang++", "curl"):
            if not shutil.which(tool):
                fail(f"missing: {tool} (see README, Requirements)")
        env = dict(os.environ)
        cc = []
        os.environ["CC"], os.environ["CXX"] = "clang", "clang++"
        env["CC"], env["CXX"] = "clang", "clang++"
    # .NET 10 for the shader translator: the system's when it has SDK 10, else a private copy.
    dotnet = shutil.which("dotnet")
    has10 = dotnet and any(l.startswith("10.") for l in subprocess.run([dotnet, "--list-sdks"], capture_output=True,
                                                                        text=True).stdout.splitlines())
    if not has10:
        dn = os.path.join(T, "dotnet")
        dotnet = os.path.join(dn, "dotnet.exe" if WIN else "dotnet")
        if not os.path.exists(dotnet):
            rid = {("Windows", "AMD64"): "win-x64", ("Linux", "x86_64"): "linux-x64", ("Linux", "aarch64"): "linux-arm64",
                   ("Darwin", "arm64"): "osx-arm64", ("Darwin", "x86_64"): "osx-x64"}[(platform.system(), platform.machine())]
            ext = "zip" if WIN else "tar.gz"
            fetch(f"https://builds.dotnet.microsoft.com/dotnet/Sdk/{DOTNET}/dotnet-sdk-{DOTNET}-{rid}.{ext}", dn, strip=False)
        env["DOTNET_ROOT"] = dn
        env["PATH"] = dn + os.pathsep + env["PATH"]
    env["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1"
    harness = os.path.join(P, "build", "spirv_harness", "bin", "Release", "net10.0", "spirv_harness.dll")
    if not os.path.exists(harness) or force("harness"):
        run([dotnet, "build", "tools/maxwell/spirv_harness/SpirvHarness.csproj", "-c", "Release", "-m:4"],
            log=os.path.join(logs, "harness.log"), env=env)

    # ---- game -------------------------------------------------------------------------------------------------
    say("game (the game folder: ExeFS + RomFS, add-on content)")
    exefs, romfs, aoc = os.path.join(G, "exefs"), os.path.join(G, "extract", "romfs"), os.path.join(G, "aoc")
    src_file = os.path.join(G, "game_source.txt")
    if a.game:
        game = os.path.abspath(a.game)
        g_exefs, g_romfs, g_aoc = find_game(game)
        old = open(src_file, encoding="utf-8").read().strip() if os.path.exists(src_file) else ""
        if old and old != game or (not old and is_real_dir(exefs)):
            # Another game folder (or a copy imported before the game folder existed): another dump, maybe another
            # version. What was made from it goes; the files themselves are never deleted, only links to them.
            if is_real_dir(exefs) and not force("game"):
                fail(f"{G} holds a game imported into it: give the game folder its own --out folder, or run again with "
                     "FORCE=game to use the game folder in its place (the imported copy is then deleted)")
            for d in ("analysis", "gen", "spirv", "shaders_extracted"):
                shutil.rmtree(os.path.join(G, d), ignore_errors=True)
            for f in ("version.ini", "speedfix_main.pchtxt"):
                if os.path.exists(os.path.join(G, f)):
                    os.remove(os.path.join(G, f))
        link(g_exefs, exefs)
        link(g_romfs, romfs)
        if g_aoc:
            link(g_aoc, aoc)
        elif is_link(aoc):
            unlink(aoc)
        with open(src_file, "w", encoding="utf-8") as f:
            f.write(game + "\n")
    for m in ("main", "sdk", "subsdk0"):
        if not os.path.isfile(os.path.join(exefs, m)):
            fail(f"no game in {G}: pass --game <your game folder> (exefs/ and romfs/ of your own dump)")
    if not os.path.isdir(os.path.join(romfs, "Shader")):
        fail(f"{romfs} has no Shader/ folder: not Animal Crossing: New Horizons' RomFS (pass --game again)")
    # The modules as loaded (text, ro and data at their offsets), which the shader extraction reads: from the NSOs.
    os.makedirs(os.path.join(G, "analysis"), exist_ok=True)
    for m in ("main", "sdk", "subsdk0"):
        nso, out = os.path.join(exefs, m), os.path.join(G, "analysis", m + ".bin")
        if not os.path.exists(out) or os.path.getmtime(nso) > os.path.getmtime(out) or force("game"):
            run([py, "-I", "tools/nso2bin.py", nso, out], log=os.path.join(logs, f"nso2bin_{m}.log"))
    print(f"main.bin {os.path.getsize(os.path.join(G, 'analysis', 'main.bin'))} bytes")
    if os.path.isdir(aoc):
        print(f"add-on content: {sum(1 for e in os.scandir(aoc) if e.is_dir())}")

    # ---- mods: a zip with romfs/ at its root (or one folder holding it), extracted to <folder>/mods/<name>/ ---------
    # Without --mod, a pt-BR translation package found in the game folder or in private/translations/ (the player's own
    # copy: never in this repository) is installed as "ptbr" when it is newer than the one installed.
    if not a.mod:
        found = []
        game_src = open(src_file, encoding="utf-8").read().strip() if os.path.exists(src_file) else ""
        for d in (game_src, os.path.join(ROOT, "private", "translations")):
            if d and os.path.isdir(d):
                found += [os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith(".zip") and "ptbr" in f.lower()]
        if found:
            newest = max(found, key=os.path.getmtime)
            have = os.path.join(G, "mods", "ptbr", "romfs")
            if not os.path.isdir(have) or os.path.getmtime(newest) > os.path.getmtime(have):
                a.mod.append("ptbr=" + newest)
    for m in a.mod:
        name, _, zpath = m.partition("=")
        if not name or not os.path.isfile(zpath):
            fail("--mod wants <name>=<zip>")
        say(f"mod {name} ({zpath})")
        dst = os.path.join(G, "mods", name)
        with zipfile.ZipFile(zpath) as z:
            names = [i.filename for i in z.infolist()]
            if any(n.startswith("/") or ".." in n.split("/") for n in names):
                fail("unsafe path in the zip")
            prefix = ""
            if not any(n.startswith("romfs/") for n in names):  # one wrapping folder
                tops = {n.split("/")[0] for n in names if "/romfs/" in n}
                if len(tops) != 1:
                    fail("no romfs/ folder in the zip")
                prefix = tops.pop() + "/"
            shutil.rmtree(dst, ignore_errors=True)
            for i in z.infolist():
                if i.is_dir() or not i.filename.startswith(prefix + "romfs/"):
                    continue
                out = os.path.join(dst, i.filename[len(prefix):])
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with z.open(i) as s, open(out, "wb") as d:
                    shutil.copyfileobj(s, d)
        fix_archives(os.path.join(dst, "romfs"))
        rewrap_messages(romfs, os.path.join(dst, "romfs"))

    # ---- version ------------------------------------------------------------------------------------------------
    say("version")
    # Main's build id picks the profile (games/acnh/versions.txt): the hooks file of a known build, none for the others.
    run([py, "-I", "tools/acnh_version.py", G])
    get = lambda k: subprocess.run([py, "-I", "tools/acnh_version.py", G, "--get", k], capture_output=True,
                                   text=True, cwd=ROOT).stdout.strip()
    hooks, version = get("hooks"), get("version")

    # ---- recompile ----------------------------------------------------------------------------------------------
    say("recompile (main, sdk, subsdk0)")
    # Any change to the recompiler (emit.py, emit_ext.py, ...) makes the generated code stale.
    recomp_mtime = max(os.path.getmtime(os.path.join(ROOT, "recomp", f))
                       for f in os.listdir(os.path.join(ROOT, "recomp")) if f.endswith(".py"))
    for mod, base in (("sdk", "0x4010000000"), ("subsdk0", "0x4018000000"), ("main", "0x4000000000")):
        h = []
        if mod == "main" and hooks != "-":
            h = ["--hooks", hooks]  # instructions that call the runtime first, at the addresses of this build only
        pch = os.path.join(G, "speedfix_main.pchtxt")
        if mod == "main" and version in ("3.0.2", "3.0.3") and os.path.exists(pch):
            h += ["--variant", pch]  # the native 60 FPS variant (tools/setup_60fps.sh; tmouh's patch has both builds)
        profile = " ".join(h) or "none"
        gen = os.path.join(G, "gen", mod)
        table, prof_file = os.path.join(gen, "table.cpp"), os.path.join(gen, "profile.txt")
        if os.path.exists(table) and not os.path.exists(prof_file):  # recompiled before profiles existed: 3.0.2 as is
            open(prof_file, "w").write(profile + "\n")
        stale = (not os.path.exists(table) or force("recomp")
                 or recomp_mtime > os.path.getmtime(table)
                 or open(prof_file).read().strip() != profile
                 or (hooks != "-" and mod == "main" and os.path.getmtime(os.path.join(ROOT, hooks)) > os.path.getmtime(table)))
        log = os.path.join(logs, f"recomp_{mod}.log")
        if stale:
            run([py, "-m", "recomp", os.path.join(G, "exefs", mod), gen, "--name", mod, "--base", base,
                 "--jobs", str(min(jobs, cores))] + h, log=log)
            open(prof_file, "w").write(profile + "\n")
        print(open(log, encoding="utf-8", errors="replace").read().strip().splitlines()[-1] if os.path.exists(log) else "")

    # ---- shaders ------------------------------------------------------------------------------------------------
    say("shaders (Maxwell -> SPIR-V)")
    summary = os.path.join(G, "spirv", "summary.txt")
    if not os.path.exists(summary) or force("shaders"):
        run([py, "tools/extract_shaders.py", G, "--jobs", str(cores)])
        val = ["--spirv-val", shutil.which("spirv-val")] if shutil.which("spirv-val") else ["--no-val"]
        run([dotnet, harness, "--batch", os.path.join(G, "shaders_extracted"), "--out", os.path.join(G, "spirv"),
             "--jobs", str(cores)] + val, log=os.path.join(logs, "spirv.log"), env=env)
    first = open(summary, encoding="utf-8", errors="replace").readline().strip()
    print(first)
    if first.startswith("blocks 0 "):  # nothing was extracted: the game would draw nothing
        os.remove(summary)  # the next run tries again
        fail("no shader was extracted (see the FAIL lines above and the logs in "
             f"{os.path.join(G, 'shaders_extracted')}): the build would draw nothing")

    # ---- fonts --------------------------------------------------------------------------------------------------
    say("fonts (button icons)")
    fonts = os.path.join(G, "fonts")
    os.makedirs(fonts, exist_ok=True)
    # Stand-in for the Switch's button-icon font (type 5), drawn over a host font. The other shared fonts fall back
    # to host fonts (runtime/src/hle_pl.cpp); ACNH ships its own fonts in its RomFS for almost all text.
    if not os.path.exists(os.path.join(fonts, "shared_5.ttf")):
        for f in (r"C:\Windows\Fonts\arial.ttf", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                  "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"):
            if os.path.exists(f):
                run([py, "tools/fonts/make_button_font.py", "--base", f, "--out", os.path.join(fonts, "shared_5.ttf")],
                    log=os.path.join(logs, "button_font.log"))
                break
    # ACNH draws its button prompts with its own ParkExt font (Font/ScalableFont.sarc.zs): a copy per controller
    # style and face-button layout, chosen at boot by controls.glyphs (runtime/src/glyphs.cpp). About 13 MB each.
    if not os.path.exists(os.path.join(fonts, "glyphs_xbox", "console", "romfs", "Font", "ScalableFont.sarc.zs")) or force("glyphs"):
        run([py, "tools/make_button_glyphs.py", "--romfs", os.path.join(G, "extract", "romfs"), "--out", fonts],
            log=os.path.join(logs, "glyphs.log"))

    # ---- build --------------------------------------------------------------------------------------------------
    say(f"build (-j{jobs}; the first build compiles ~5 CPU-hours of code, about {300 // jobs + 10} minutes here)")
    extra = [f"-DNX_GEN_DIR={os.path.join(G, 'gen')}"]
    if WIN:
        extra.append(f"-DCMAKE_PREFIX_PATH={os.path.join(T, f'SDL3-{SDL3}')};{MOVIE_LIBS}")
    elif MAC:
        # Started from the Finder (the launcher), PATH has no Homebrew: its usual places.
        brew_exe = shutil.which("brew") or next((b for b in ("/opt/homebrew/bin/brew", "/usr/local/bin/brew")
                                                 if os.path.exists(b)), None)
        brew = (subprocess.run([brew_exe, "--prefix"], capture_output=True, text=True).stdout.strip() if brew_exe
                else "") or "/opt/homebrew"
        extra.append(f"-DCMAKE_PREFIX_PATH={brew}")
    build = os.path.join(G, "build")
    run([cmake, "-S", ".", "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DNX_GAME=acnh",
         "-DCMAKE_BUILD_TYPE=Release", f"-DNX_VULKAN_INCLUDE={os.path.join(vkh, 'include')}"] + cc + extra,
        log=os.path.join(logs, "cmake.log"), env=env)
    run([ninja, "-C", build, f"-j{jobs}", "acnh"], env=env)
    exe = os.path.join(build, "acnh.exe" if WIN else "acnh")
    say(f"done: {exe}   (NX_DATA_ROOT={G})")


if __name__ == "__main__":
    main()
