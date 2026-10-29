"""Draws ChronoDrive.ico: a drive (a rounded disk unit with its slot and green light) with a cloud over it, in the
indigo of the web page. Sizes 16 to 256, each drawn at 4x and scaled down, so the small ones stay crisp; at 16 and 20
px only the unit and the cloud remain (no slot), so the shape still reads.
Usage: python tools/make_icon.py   (writes ChronoDrive.ico and tools/icon_preview.png)"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
INDIGO, INDIGO_DARK, WHITE, GREEN, SKY = (67, 56, 202, 255), (49, 46, 129, 255), (255, 255, 255, 255), (16, 185, 129, 255), (224, 231, 255, 255)


def cloud(d, cx, cy, w, fill, outline=None, ow=0):
    """A cloud of three bumps on a flat base, `w` wide, centred on (cx, cy)."""
    h = w * 0.62
    left, top = cx - w / 2, cy - h / 2
    parts = [
        (left + w * 0.02, top + h * 0.38, left + w * 0.46, top + h * 1.00),   # left bump
        (left + w * 0.22, top + h * 0.00, left + w * 0.72, top + h * 0.86),   # big middle bump
        (left + w * 0.52, top + h * 0.26, left + w * 0.98, top + h * 1.00),   # right bump
    ]
    base = (left + w * 0.20, top + h * 0.55, left + w * 0.80, top + h * 1.00)
    if outline:
        g = ow
        for x0, y0, x1, y1 in parts:
            d.ellipse([x0 - g, y0 - g, x1 + g, y1 + g], fill=outline)
        d.rectangle([base[0] - g, base[1] - g, base[2] + g, base[3] + g], fill=outline)
    for x0, y0, x1, y1 in parts:
        d.ellipse([x0, y0, x1, y1], fill=fill)
    d.rectangle(base, fill=fill)


def draw(size):
    S = size * 4
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = S / 64.0                      # design grid: 64 x 64
    small = size <= 20
    # the drive: a wide rounded box with a darker front edge, in the lower part of the icon
    top = 30 if not small else 28
    d.rounded_rectangle([3 * u, (top + 4) * u, 61 * u, 61 * u], radius=9 * u, fill=INDIGO_DARK)
    d.rounded_rectangle([3 * u, top * u, 61 * u, 56 * u], radius=9 * u, fill=INDIGO)
    if not small:
        d.rounded_rectangle([11 * u, 44 * u, 36 * u, 49 * u], radius=2.5 * u, fill=(255, 255, 255, 140))
        d.ellipse([45 * u, 42.5 * u, 53 * u, 50.5 * u], fill=GREEN)
    else:
        d.ellipse([44 * u, 41 * u, 54 * u, 51 * u], fill=GREEN)
    # the cloud sits on the top edge of the drive, with a white rim so it reads on the indigo and on any background
    cloud(d, 32 * u, (21 if not small else 19) * u, (40 if not small else 46) * u, SKY if not small else WHITE, outline=WHITE if not small else None, ow=2.2 * u)
    return im.resize((size, size), Image.LANCZOS)


sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = [draw(s) for s in sizes]
ico = os.path.join(HERE, "..", "ChronoDrive.ico")
imgs[-1].save(ico, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
prev = Image.new("RGBA", (sum(sizes) + 10 * len(sizes) + 10, 280), (240, 240, 244, 255))
dark = Image.new("RGBA", prev.size, (32, 33, 36, 255))
x = 5
for im, s in zip(imgs, sizes):
    prev.paste(im, (x, 140 - s // 2), im); dark.paste(im, (x, 140 - s // 2), im); x += s + 10
both = Image.new("RGBA", (prev.width, prev.height * 2)); both.paste(prev, (0, 0)); both.paste(dark, (0, prev.height))
both.save(os.path.join(HERE, "icon_preview.png"))
print("wrote", os.path.abspath(ico))
