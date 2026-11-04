#!/usr/bin/env python3
"""Build runtime/src/ui/ui_cjk_font.cpp: a Noto Sans subset Dear ImGui's stb_truetype can draw.

Android 15+ system CJK is a CFF2 collection, which stb_truetype rejects. Noto Sans SC/KR from the
Sans 2.004 language packages are CFF and do load. The subset is the menu strings plus ImGui's
simplified-Chinese and Japanese ranges. Hangul not in the SC face comes from Noto Sans KR, limited
to syllables the menu actually uses (the full Hangul block would bloat the phone atlas).

Both faces are SIL OFL 1.1 (see the copyright in the generated file). Pass --sc and --kr if the
zips are already downloaded; otherwise they are fetched from the Noto CJK release.
"""
import argparse
import io
import re
import urllib.request
import zipfile
from pathlib import Path

from fontTools.subset import Options, Subsetter
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[2]
# tools/setup_acnh.py downloads Dear ImGui here. The glyph-range tables are read from it.
IMGUI_CANDIDATES = [
    ROOT / "private" / "third_party" / "imgui" / "imgui_draw.cpp",
]
SC_URL = "https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/18_NotoSansSC.zip"
KR_URL = "https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/17_NotoSansKR.zip"


def imgui_draw() -> Path:
    for p in IMGUI_CANDIDATES:
        if p.is_file():
            return p
    raise SystemExit("imgui_draw.cpp not found; download Dear ImGui 1.91.9 first")


def quoted_strings(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    out = []
    i = 0
    while True:
        a = text.find('"', i)
        if a < 0:
            break
        j = a + 1
        chars = []
        while j < len(text):
            c = text[j]
            if c == "\\":
                n = text[j + 1]
                chars.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(n, n))
                j += 2
                continue
            if c == '"':
                out.append("".join(chars))
                i = j + 1
                break
            chars.append(c)
            j += 1
        else:
            break
    return out


def accumulative(src: str, marker: str) -> list[int]:
    i = src.find(marker)
    if i < 0:
        raise SystemExit(f"missing {marker}")
    brace = src.find("{", i)
    end = src.find("};", brace)
    return [int(n) for n in re.findall(r"-?\d+", src[brace + 1 : end])]


def unpack(base: int, offsets: list[int]) -> set[int]:
    cps = set()
    cp = base
    for off in offsets:
        cp += off
        cps.add(cp)
    return cps


def wanted() -> tuple[set[int], set[int]]:
    cps: set[int] = set()
    for rel in ("runtime/src/ui/i18n.cpp", "runtime/src/ui/i18n_launcher.inc"):
        for s in quoted_strings(ROOT / rel):
            cps.update(ord(ch) for ch in s)
    cps.update(range(0x2000, 0x206F + 1))
    cps.update(range(0x3000, 0x30FF + 1))
    cps.update(range(0x31F0, 0x31FF + 1))
    cps.update(range(0xFF00, 0xFFEF + 1))
    cps.add(0xFFFD)
    src = imgui_draw().read_text(encoding="utf-8")
    # Both tables are named the same; take them in source order (simplified, then Japanese).
    tables = list(re.finditer(r"static const short accumulative_offsets_from_0x4E00\[\] =", src))
    if len(tables) < 2:
        raise SystemExit("expected two ImGui accumulative CJK tables")
    for m in tables[:2]:
        cps.update(unpack(0x4E00, accumulative(src[m.start() :], "accumulative_offsets_from_0x4E00")))
    hangul = {cp for cp in cps if 0x1100 <= cp <= 0x11FF or 0x3130 <= cp <= 0x318F or 0xAC00 <= cp <= 0xD7A3}
    return cps, hangul


def read_regular(zip_path: Path) -> bytes:
    with zipfile.ZipFile(zip_path) as z:
        name = next(n for n in z.namelist() if n.endswith("Regular.otf"))
        return z.read(name)


def subset(data: bytes, cps: set[int]) -> bytes:
    font = TTFont(io.BytesIO(data))
    have = set(font.getBestCmap())
    use = cps & have
    opt = Options()
    opt.drop_tables = ["GSUB", "GPOS", "GDEF", "BASE", "DSIG", "vhea", "vmtx", "VORG"]
    opt.layout_features = []
    opt.hinting = False
    opt.desubroutinize = False
    opt.notdef_outline = True
    opt.name_IDs = [0, 1, 2, 3, 4, 5, 6]
    opt.glyph_names = False
    sub = Subsetter(options=opt)
    sub.populate(unicodes=use)
    sub.subset(font)
    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue()


def emit(name: str, data: bytes) -> str:
    lines = [f"extern const unsigned char {name}[] = {{"]
    for i in range(0, len(data), 20):
        chunk = data[i : i + 20]
        lines.append("    " + ",".join(str(b) for b in chunk) + ",")
    lines.append("};")
    lines.append(f"extern const int {name}Size = int(sizeof({name}));")
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    cache = ROOT / "private" / "fonts"
    ap.add_argument("--sc", type=Path, default=cache / "NotoSansSC.zip")
    ap.add_argument("--kr", type=Path, default=cache / "NotoSansKR.zip")
    a = ap.parse_args()
    for path, url in ((a.sc, SC_URL), (a.kr, KR_URL)):
        if not path.is_file():
            path.parent.mkdir(parents=True, exist_ok=True)
            print(f"downloading {url}")
            urllib.request.urlretrieve(url, path)
    cps, hangul = wanted()
    sc = subset(read_regular(a.sc), cps)
    kr = subset(read_regular(a.kr), hangul)
    print(f"SC subset {len(sc)} bytes, KR subset {len(kr)} bytes, codepoints {len(cps)}, hangul {len(hangul)}")
    missing_note = []
    sc_cmap = set(TTFont(io.BytesIO(sc)).getBestCmap())
    kr_cmap = set(TTFont(io.BytesIO(kr)).getBestCmap()) if kr else set()
    for rel in ("runtime/src/ui/i18n.cpp", "runtime/src/ui/i18n_launcher.inc"):
        for s in quoted_strings(ROOT / rel):
            for ch in s:
                cp = ord(ch)
                if cp < 0x80:
                    continue
                if cp not in sc_cmap and cp not in kr_cmap:
                    missing_note.append(ch)
    if missing_note:
        uniq = "".join(sorted(set(missing_note)))
        print(f"menu characters absent from both subsets ({len(set(missing_note))}): {uniq[:200]}")
    out = ROOT / "runtime" / "src" / "ui" / "ui_cjk_font.cpp"
    header = """// Generated by tools/fonts/make_ui_cjk_font.py. Do not edit.
// Noto Sans SC and Noto Sans KR, subset for the settings menu.
// Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.
// Copyright 2012 Google LLC. Licensed under the SIL Open Font License 1.1.
// The license is at https://scripts.sil.org/OFL and in the Noto CJK release this was subset from.
#include <cstddef>

namespace nx::ui {

"""
    out.write_text(header + emit("kUiCjkTtf", sc) + "\n" + emit("kUiHangulTtf", kr) + "\n}  // namespace nx::ui\n", encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
