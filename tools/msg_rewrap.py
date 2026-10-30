#!/usr/bin/env python3
"""Re-wraps a translation's dialogue lines that are wider than the game's text boxes.

The game breaks a line that does not fit by itself, which leaves a word alone on a line: the translation's own line
breaks were placed for a narrower text. This measures each line with the game's dialogue font (nintendoP_Seurat-B,
from the game's own romfs) and, when one is wider than the box, re-flows it with the lines of the same sentence after
it: as many lines as before when they fit, balanced, one more when they do not. Lines that fit are left exactly as they
were; control tags (colours, pauses, expressions) stay glued to the words they touch.

The box widths are calibrated on the game's own text: its dialogue lines stop at about 2020 font units (size 100),
its letters' at about 1910.
Only the dialogue archives are touched (Talk*, Mail, Tutorials); menus and names have other widths. One exception: the
tips of the phone's Island Life 101 app (System, LifeSupport/), whose bubbles hold the game's own 3 lines (up to ~3390
units wide) and do not grow: a tip is re-flowed into as few lines as fit there, or its text spills over the next one.

usage: tools/msg_rewrap.py <game romfs> <mod romfs> [--dry-run]     (needs Pillow)"""
import glob
import io
import os
import struct
import subprocess
import sys

# The width of the boxes per archive: dialogue, and letters (narrower paper).
LIMITS = (("Talk", 2020), ("Tutorials_", 2020), ("Mail_", 1910))
# Messages re-flowed into the fewest lines: (archive prefix, message file prefix, width).
REFLOW = (("System_", "LifeSupport/", 3300),)
FONT_KEY = struct.pack(">I", 0x49621806)


def read_zs(path):
    return subprocess.run(["zstd", "-d", "-q", "-c", path], capture_output=True, check=True).stdout


def write_zs(path, data):
    raw = path + ".raw"  # from a file: zstd records the content size only when it knows it
    open(raw, "wb").write(data)
    try:
        subprocess.run(["zstd", "-19", "-q", "-f", "--no-check", raw, "-o", path], check=True)
    finally:
        os.remove(raw)


def sarc_files(d):
    e = "<" if d[6:8] == b"\xff\xfe" else ">"
    hlen, _, _, doff, _ = struct.unpack_from(e + "HHIIH", d, 4)
    _, sl, n, _ = struct.unpack_from(e + "4sHHI", d, hlen)
    nodes = [struct.unpack_from(e + "IIII", d, hlen + sl + 16 * i) for i in range(n)]
    sfnt = hlen + sl + 16 * n
    out = []
    for i, (h, attr, s, t) in enumerate(nodes):
        name = d[sfnt + 8 + (attr & 0xFFFF) * 4:].split(b"\0")[0].decode()
        out.append((name, hlen + sl + 16 * i, doff + s, doff + t))
    return e, doff, out


def sarc_replace(d, new):
    """The archive with some files' contents replaced; files keep their order and the original's alignment."""
    e, doff, files = sarc_files(d)
    align = 0x2000  # the largest power of two the original starts all keep
    for _, _, s, _ in files:
        while (s - doff) % align:
            align //= 2
    align = max(align, 8)
    head = bytearray(d[:doff])
    body = bytearray()
    for name, node, s, t in files:
        data = new.get(name, d[s:t])
        while len(body) % align:
            body.append(0)
        start = len(body)
        body += data
        struct.pack_into(e + "II", head, node + 8, start, len(body))
    out = head + body
    struct.pack_into(e + "I", out, 8, len(out))
    return bytes(out)


def msbt_strings(b):
    e = "<" if b[8:10] == b"\xff\xfe" else ">"
    nsec = struct.unpack_from(e + "H", b, 14)[0]
    pos = 32
    for _ in range(nsec):
        size = struct.unpack_from(e + "I", b, pos + 4)[0]
        if b[pos:pos + 4] == b"TXT2":
            base = pos + 16
            n = struct.unpack_from(e + "I", b, base)[0]
            offs = [struct.unpack_from(e + "I", b, base + 4 + 4 * k)[0] for k in range(n)] + [size]
            return e, pos, size, [b[base + offs[k]:base + offs[k + 1]] for k in range(n)]
        pos = (pos + 16 + size + 15) & ~15
    return e, None, 0, []


def msbt_replace(b, strings):
    e, pos, size, _ = msbt_strings(b)
    n = len(strings)
    table, blob, off = b"", b"", 4 + 4 * n
    for s in strings:
        table += struct.pack(e + "I", off + len(blob))
        blob += s
    sec = struct.pack(e + "I", n) + table + blob
    end = (pos + 16 + size + 15) & ~15
    pad = (-len(sec)) % 16
    head = bytearray(b[pos:pos + 16])
    struct.pack_into(e + "I", head, 4, len(sec))
    out = bytearray(b[:pos]) + head + sec + b"\xab" * pad + b[end:]
    struct.pack_into(e + "I", out, 18, len(out))
    return bytes(out)


class Text:
    """A TXT2 string (UTF-16) as lines of items: a character (str) or a control tag (tuple of the code units)."""

    def __init__(self, raw, e):
        self.e = e
        u = struct.unpack(e + "%dH" % (len(raw) // 2), raw)
        self.lines, cur, i = [], [], 0
        self.parts = [self.lines]  # pages: lists of lines, split at the explicit page break tag (0, 4)
        while i < len(u):
            c = u[i]
            if c == 0x0E and i + 3 < len(u):
                n = 4 + u[i + 3] // 2
                tag = tuple(u[i:i + n])
                cur.append(tag)
                i += n
                if tag[1:3] == (0, 4):
                    self.lines.append(cur)
                    cur = []
                    self.lines = []
                    self.parts.append(self.lines)
                continue
            if c == 0x0F:
                cur.append(tuple(u[i:i + 3]))
                i += 3
                continue
            if c == 10:
                self.lines.append(cur)
                cur = []
            else:
                cur.append(chr(c))
            i += 1
        self.lines.append(cur)

    def encode(self):
        u = []
        for lines in self.parts:
            for k, line in enumerate(lines):
                if k:
                    u.append(10)
                for it in line:
                    u.extend(it if isinstance(it, tuple) else (ord(it),))
        return struct.pack(self.e + "%dH" % len(u), *u)


def words(line):
    """The line split at spaces; tags stay inside the word they touch."""
    out, cur = [], []
    for it in line:
        if it == " ":
            out.append(cur)
            cur = []
        else:
            cur.append(it)
    out.append(cur)
    return out


def join(ws):
    line = []
    for k, w in enumerate(ws):
        if k:
            line.append(" ")
        line += w
    return line


def visible(w):
    return "".join(it for it in w if isinstance(it, str) and it != "\0")


def tag_text(tag):
    """What a tag shows on screen, as far as its width goes: the ellipsis tag "...", the longest of the words a
    gender or plural choice picks from (in its parameters), a placeholder for names, items and numbers."""
    group, kind, params = tag[1], tag[2], tag[4:]
    if group == 110 and kind == 0:
        return "..."
    if group == 50 and params:
        alts = "".join(chr(c) if c >= 32 and c < 0xE000 else "\n" for c in params).split("\n")
        return max(alts, key=len)
    if group == 90:
        return "000"
    if group in (50, 110, 115, 125, 135, 140, 150):
        return "Nnnnnn"
    return ""


class Measure:
    def __init__(self, font):
        self.font, self.adv = font, {}

    def text(self, s):
        total = 0.0
        for ch in s:
            a = self.adv.get(ch)
            if a is None:
                a = self.adv[ch] = self.font.getlength(ch)
            total += a
        return total

    def __call__(self, items):
        """The width of a line or word: its characters and what its tags show, without the spaces at its ends."""
        s = "".join(it if isinstance(it, str) else tag_text(it) if it[0] == 0x0E else "" for it in items)
        return self.text(s.replace("\0", "").strip())


def ends_sentence(line):
    end = [it for it in line if it != " " and it != "\0" and not (isinstance(it, tuple) and it[1] in (40, 10))]
    if end and isinstance(end[-1], tuple) and end[-1][1:3] == (110, 0):  # an ellipsis
        return True
    return visible(line).rstrip().endswith((".", "!", "?", "\u2026", ":"))


def balance(ws, n, width, space, limit):
    """The words in exactly n lines, each fitting in the limit when it can, the widest line as narrow as possible."""
    w = [width(x) for x in ws]

    def span(i, j):
        return sum(w[i:j]) + space * (j - i - 1)

    inf = float("inf")
    best = {(0, 0): (0.0, None)}
    for line in range(1, n + 1):
        for j in range(line, len(ws) + 1):
            for i in range(line - 1, j):
                prev = best.get((line - 1, i))
                wide = span(i, j)
                if prev is None or (wide > limit and j - i > 1):
                    continue
                cost = max(prev[0], wide)
                if cost < best.get((line, j), (inf,))[0]:
                    best[(line, j)] = (cost, i)
    if (n, len(ws)) not in best:
        return None
    lines, j = [], len(ws)
    for line in range(n, 0, -1):
        i = best[(line, j)][1]
        lines.insert(0, join(ws[i:j]))
        j = i
    return lines


def rewrap(text, width, limit):
    """Lines wider than the box re-flowed with the lines of the same sentence after them: the same number of lines
    when they fit, one more line at a time when they do not."""
    space = width.text(" ")
    changed = False
    for lines in text.parts:
        k = 0
        while k < len(lines):
            if width(lines[k]) <= limit:
                k += 1
                continue
            end = k + 1  # the sentence goes on in the next lines, up to its end or the page's
            while end < len(lines) and end - k < 4 and not ends_sentence(lines[end - 1]) \
                    and visible(lines[end]).strip():
                end += 1
            ws = [x for line in lines[k:end] for x in words(line) if x]
            out = None
            for n in range(end - k, end - k + 4):
                out = balance(ws, n, width, space, limit)
                if out:
                    break
            if not out:
                k += 1
                continue
            lines[k:end] = out
            changed = True
            k += len(out)
    return changed


def reflow(text, width, limit):
    """Each page's words in the fewest lines that fit, balanced; pages already that short are left as they were."""
    space = width.text(" ")
    changed = False
    for p, lines in enumerate(text.parts):
        ws = [x for line in lines for x in words(line) if x]
        for n in range(1, len(lines)):
            out = balance(ws, n, width, space, limit)
            if out:
                text.parts[p][:] = out
                changed = True
                break
    return changed


def font(game_romfs):
    d = read_zs(os.path.join(game_romfs, "Font", "ScalableFont.sarc.zs"))
    for name, _, s, t in sarc_files(d)[2]:
        if name.startswith("nintendoP_Seurat-B"):  # 8-byte header, then the font XORed with a 32-bit key
            return bytes(b ^ FONT_KEY[i % 4] for i, b in enumerate(d[s + 8:t]))
    raise SystemExit("dialogue font not found in " + game_romfs)


def main():
    from PIL import ImageFont
    game, mod = sys.argv[1], sys.argv[2]
    dry = "--dry-run" in sys.argv
    width = Measure(ImageFont.truetype(io.BytesIO(font(game)), 100))

    for path in sorted(glob.glob(os.path.join(mod, "Message", "*.sarc.zs"))):
        base = os.path.basename(path)
        limit = next((w for prefix, w in LIMITS if base.startswith(prefix)), None)
        flow = [(m, w) for prefix, m, w in REFLOW if base.startswith(prefix)]
        if not limit and not flow:
            continue
        d = read_zs(path)
        new, count = {}, 0
        for name, _, s, t in sarc_files(d)[2]:
            b = d[s:t]
            if b[:8] != b"MsgStdBn":
                continue
            fit = next((w for m, w in flow if name.startswith(m)), None)
            if not limit and not fit:
                continue
            e, pos, _, strings = msbt_strings(b)
            out, hit = [], False
            for raw in strings:
                text = Text(raw, e)
                if reflow(text, width, fit) if fit else rewrap(text, width, limit):
                    out.append(text.encode())
                    count += 1
                    hit = True
                else:
                    out.append(raw)
            if hit:
                new[name] = msbt_replace(b, out)
        print(f"{os.path.basename(path)}: {count} messages re-wrapped")
        if new and not dry:
            write_zs(path, sarc_replace(d, new))


if __name__ == "__main__":
    main()
