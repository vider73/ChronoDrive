"""Draws ChronoDrive.ico: a drive (a rounded disk unit with its slot and green light) with two sync arrows over it,
in the indigo of the web page. Sizes 16 to 256, each drawn at 4x and scaled down, so the small ones stay crisp.
Usage: python tools/make_icon.py   (writes ChronoDrive.ico and tools/icon_preview.png)"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
INDIGO, INDIGO_DARK, WHITE, GREEN = (67, 56, 202, 255), (49, 46, 129, 255), (255, 255, 255, 255), (16, 185, 129, 255)


def draw(size):
    S = size * 4
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = S / 64.0                     # design grid: 64 x 64
    # the drive body: a wide rounded box, with a darker base (the drive's front edge)
    d.rounded_rectangle([4 * u, 20 * u, 60 * u, 56 * u], radius=9 * u, fill=INDIGO_DARK)
    d.rounded_rectangle([4 * u, 16 * u, 60 * u, 50 * u], radius=9 * u, fill=INDIGO)
    # the slot and the light
    d.rounded_rectangle([12 * u, 38 * u, 38 * u, 43 * u], radius=2.5 * u, fill=(255, 255, 255, 140))
    d.ellipse([46 * u, 36.5 * u, 54 * u, 44.5 * u], fill=GREEN)
    if size >= 24:
        # two sync arrows (a circle of two arcs with heads) on the top of the drive
        c, r, w = (32 * u, 27 * u), 7.5 * u, max(2.2 * u, 3)
        d.arc([c[0] - r, c[1] - r, c[0] + r, c[1] + r], start=200, end=340, fill=WHITE, width=int(w))
        d.arc([c[0] - r, c[1] - r, c[0] + r, c[1] + r], start=20, end=160, fill=WHITE, width=int(w))
        h = 3.6 * u
        d.polygon([(c[0] + r - h, c[1] - 1.8 * u), (c[0] + r + h, c[1] - 1.8 * u), (c[0] + r, c[1] + 2.6 * u)], fill=WHITE)
        d.polygon([(c[0] - r - h, c[1] + 1.8 * u), (c[0] - r + h, c[1] + 1.8 * u), (c[0] - r, c[1] - 2.6 * u)], fill=WHITE)
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = [draw(s) for s in sizes]
ico = os.path.join(HERE, "..", "ChronoDrive.ico")
imgs[-1].save(ico, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
prev = Image.new("RGBA", (sum(sizes) + 10 * len(sizes), 260), (240, 240, 244, 255))
x = 5
for im, s in zip(imgs, sizes):
    prev.paste(im, (x, 130 - s // 2), im); x += s + 10
prev.save(os.path.join(HERE, "icon_preview.png"))
print("wrote", os.path.abspath(ico))
