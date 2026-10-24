#!/usr/bin/env python3
"""The Android launcher icon (android/res/mipmap-*/ic_launcher*.png): an original drawing in the game's spirit, a
white leaf over a little island in the sea under a soft green sky. No Nintendo artwork. Needs Pillow."""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # drawn large, scaled down per density


def leaf(draw, cx, cy, size, angle, fill, vein):
    """A pointed leaf (two arcs) with a curved mid-rib, side veins slanting to the tip and a short stem."""
    a = math.radians(angle)
    def rot(x, y):
        return (cx + x * math.cos(a) - y * math.sin(a), cy + x * math.sin(a) + y * math.cos(a))
    half, w = size * 0.62, size * 0.36
    pts = []
    for k in range(0, 101):  # right edge, tip (top) to base, then the left edge back
        t = k / 100
        y = -half + 2 * half * t
        x = w * math.sin(math.pi * t) ** 0.85 * (1 + 0.12 * (t - 0.5))
        pts.append(rot(x, y))
    for k in range(100, -1, -1):
        t = k / 100
        y = -half + 2 * half * t
        x = -w * math.sin(math.pi * t) ** 0.85 * (1 + 0.12 * (t - 0.5))
        pts.append(rot(x, y))
    draw.polygon(pts, fill=fill)
    if vein is None:
        return
    draw.line([rot(0, half * 1.32), rot(0, half * 0.95)], fill=fill, width=int(size * 0.06))  # stem
    rib = [rot(0.03 * size * math.sin(math.pi * t), -half * 0.82 + 1.74 * half * t) for t in [k / 20 for k in range(21)]]
    draw.line(rib, fill=vein, width=int(size * 0.035), joint="curve")
    for yy in (-0.45, -0.15, 0.15, 0.45):
        for side in (-1, 1):
            y0 = yy * half
            draw.line([rot(0, y0), rot(side * w * 0.48, y0 - half * 0.24)], fill=vein, width=int(size * 0.022))


def icon():
    img = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(img)
    for y in range(S):  # sky: soft mint to the game's green
        t = y / S
        c = (int(150 - 26 * t), int(222 - 28 * t), int(170 - 67 * t))
        d.line([(0, y), (S, y)], fill=c)
    d.ellipse([S * 0.08, S * 0.64, S * 0.92, S * 1.18], fill=(88, 190, 214))  # sea
    d.ellipse([S * 0.18, S * 0.70, S * 0.82, S * 1.05], fill=(245, 226, 160))  # beach
    d.ellipse([S * 0.24, S * 0.72, S * 0.76, S * 1.00], fill=(124, 194, 103))  # grass
    sh = Image.new("RGBA", (S, S))
    leaf(ImageDraw.Draw(sh), S * 0.52, S * 0.47, S * 0.50, 32, (0, 0, 0, 80), None)
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(S * 0.02)))
    leaf(d, S * 0.50, S * 0.44, S * 0.50, 32, (255, 255, 250), (140, 200, 120))
    return img


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "android", "res")
    big = icon()
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)
    rounded = big.copy()
    rounded.putalpha(mask)
    round_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(round_mask).ellipse([0, 0, S - 1, S - 1], fill=255)
    circle = big.copy()
    circle.putalpha(round_mask)
    for name, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(out, f"mipmap-{name}")
        os.makedirs(d, exist_ok=True)
        rounded.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
        circle.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_round.png"))
    rounded.resize((512, 512), Image.LANCZOS).save(os.path.join(out, "..", "icon_512.png"))


if __name__ == "__main__":
    main()
