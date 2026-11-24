"""Flat-shaded stand-ins for every file in the asset contract, at the exact final sizes.

  python art/make_placeholders.py [--out plugin/resources/img] [--force]

Runs under any python3 (see deps.py). Without --force it will not replace assets that already exist.
"""
import argparse
import math
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import deps  # noqa: E402

deps.ensure()

from PIL import Image, ImageDraw, ImageFilter, ImageFont  # noqa: E402

import image_io  # noqa: E402
import layout as L  # noqa: E402
import product  # noqa: E402

K = L.SCALE
BLACK, EDGE, METAL, INK, KNOB = (20, 19, 18), (44, 42, 40), (70, 56, 45), (224, 214, 194), (28, 27, 26)
GLOW = dict(green=(20, 210, 60), red=(230, 25, 15), amber=(235, 130, 10), power=(230, 20, 12))


def box(*values):
    return tuple(round(v * K) for v in values)


def font(size, weight="SemiBold"):
    path = os.path.join(REPO, "plugin", "resources", "fonts", f"BarlowCondensed-{weight}.ttf")
    return ImageFont.truetype(path, round(size / 0.7 * K))


def background():
    im = Image.new("RGB", box(L.RENDER_W, L.RENDER_H), (30, 27, 25))
    d = ImageDraw.Draw(im)

    def text(body, x, y, size=10.6, anchor="mm", weight="SemiBold", fill=INK):
        d.text(box(x, y), body, font=font(size, weight), fill=fill, anchor=anchor)

    d.rounded_rectangle(box(18, 506, 1262, 600), 24 * K, BLACK, EDGE, 3 * K)
    d.rounded_rectangle(box(*L.HEAD), 22 * K, BLACK, EDGE, 3 * K)
    d.rectangle(box(*L.GRILLE), fill=(9, 9, 9))
    for gy in range(int(L.GRILLE[1]) + 8, int(L.GRILLE[3]) - 4, 10):
        for gx in range(int(L.GRILLE[0]) + 7 + gy // 10 % 2 * 5, int(L.GRILLE[2]) - 4, 11):
            d.ellipse(box(gx - 4, gy - 4, gx + 4, gy + 4), outline=EDGE, width=K)
    d.rounded_rectangle(box(*L.PLATE), 4 * K, METAL)
    d.rounded_rectangle(box(*L.BADGE), 4 * K, (120, 38, 24), INK, 2 * K)
    text(product.BADGE_TEXT, (L.BADGE[0] + L.BADGE[2]) / 2, (L.BADGE[1] + L.BADGE[3]) / 2, 18)
    text("PLACEHOLDER", L.RENDER_W / 2, 200, 28, fill=EDGE)

    for _, x, name in L.KNOBS:
        for i in range(11):
            a = math.radians(L.knob_degrees(i / 10))
            r0, r1 = L.TICK_R
            d.line(box(x + r0 * math.sin(a), L.CY - r0 * math.cos(a),
                       x + r1 * math.sin(a), L.CY - r1 * math.cos(a)), INK, 2 * K)
        for value, body in ((0, "0"), (0.5, "5"), (1, "10")):
            a = math.radians(L.knob_degrees(value))
            text(body, x + L.NUMERAL_R * math.sin(a), L.CY - L.NUMERAL_R * math.cos(a), 8.9)
        text(name, x, L.LABEL_CY)
    for _, x, y, name, label_y in L.BUTTONS:
        half = L.BUTTON_SIZE / 2 + 4
        d.rounded_rectangle(box(x - half, y - half, x + half, y + half), 5 * K, BLACK)
        text(name, x, label_y, 9.6)
    for _, x, y, name in L.ROCKERS:
        w, h = (v / 2 + 5 for v in L.ROCKER_SIZE)
        d.rounded_rectangle(box(x - w, y - h, x + w, y + h), 5 * K, BLACK)
        text(name, x, L.LABEL_CY + 7)
    for _, colour, x, y, _, _ in L.LAMPS:
        r = 5 if colour in ("green", "red") else 10
        d.ellipse(box(x - r, y - r, x + r, y + r), fill=tuple(v // 4 for v in GLOW[colour]))
    for body, x, y in L.LAMP_NAMES:
        text(body, x, y, 9.3, "lm")
    x, y = L.JACK
    d.ellipse(box(x - 17, y - 17, x + 17, y + 17), fill=EDGE)
    text("INPUT", x, y - 34, 9.6)
    _, x, y = L.TOGGLE
    text("HIGH", x, y - L.TOGGLE_LABEL_D, 9.6)
    text("NORMAL", x, y + L.TOGGLE_LABEL_D, 9.6)
    for name, x0, x1 in L.GROUPS:
        y, middle = L.GROUP_CY, (x0 + x1) / 2
        half = d.textlength(name.upper(), font=font(9.4)) / K / 2 + 5
        d.line([box(x0, y - 6), box(x0, y), box(middle - half, y)], INK, round(1.3 * K))
        d.line([box(middle + half, y), box(x1, y), box(x1, y - 6)], INK, round(1.3 * K))
        text(name.upper(), middle, y, 9.4)
    return im


def cell(kind):
    w, h = L.CELLS[kind]
    im = Image.new("RGBA", box(w, h))
    return im, ImageDraw.Draw(im), w / 2, h / 2


def knob(value):
    over = 4                    # drawn large, because ImageDraw rounds to whole pixels
    w, h = (round(v * K) for v in L.CELLS["knob"])
    im = Image.new("RGBA", (w * over, h * over))
    d = ImageDraw.Draw(im)
    k = K * over
    cx, cy = w * over / 2, h * over / 2
    r = L.KNOB_D / 2 * k
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=KNOB + (255,))
    cy += L.knob_axis_offset() * k
    r = 20 * k
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(46, 44, 42, 255))
    a = math.radians(L.knob_degrees(value))
    ux, uy = math.sin(a), -math.cos(a)
    near, far, half = 5 * k, 21 * k, 1.2 * k
    d.polygon([(cx + ux * r_ - uy * s * half, cy + uy * r_ + ux * s * half)
               for r_, s in ((near, -1), (far, -1), (far, 1), (near, 1))], fill=INK + (255,))
    return im.resize((w, h), Image.LANCZOS)


def button(down):
    im, d, cx, cy = cell("button")
    half = L.BUTTON_SIZE / 2 - (1.5 if down else 0)
    fill = (62, 57, 52, 255) if down else (124, 116, 106, 255)
    d.rounded_rectangle(box(cx - half, cy - half, cx + half, cy + half), 3 * K, fill)
    d.ellipse(box(cx - 2, cy - 2, cx + 2, cy + 2), fill=BLACK + (255,))
    return im


def toggle(down):
    im, d, cx, cy = cell("toggle")
    d.ellipse(box(cx - 11, cy - 11, cx + 11, cy + 11), fill=(110, 104, 96, 255))
    tip = cy + (17 if down else -17)
    d.line(box(cx, cy, cx, tip), (190, 184, 174, 255), 6 * K)
    d.ellipse(box(cx - 5, tip - 5, cx + 5, tip + 5), fill=(190, 184, 174, 255))
    return im


def rocker(on):
    im, d, cx, cy = cell("rocker")
    w, h = (v / 2 for v in L.ROCKER_SIZE)
    d.rounded_rectangle(box(cx - w, cy - h, cx + w, cy + h), 3 * K, KNOB + (255,))
    lit = (cy - h + 2, cy) if on else (cy, cy + h - 2)
    d.rectangle(box(cx - w + 2, lit[0], cx + w - 2, lit[1]), fill=(58, 54, 50, 255))
    d.line(box(cx, cy - 19, cx, cy - 9), INK + (255,), 2 * K)
    d.ellipse(box(cx - 5, cy + 9, cx + 5, cy + 19), outline=INK + (255,), width=2 * K)
    return im


def glow(colour):
    im, d, cx, cy = cell("glow")
    r = 4 if colour in ("green", "red") else 9
    d.ellipse(box(cx - r, cy - r, cx + r, cy + r), fill=GLOW[colour] + (255,))
    return im


def tubes():
    gx, _, w, h = L.TUBE_GLOW
    im = Image.new("RGB", box(w, h))
    d = ImageDraw.Draw(im)
    for x in (527, 620, 722, 814):
        d.ellipse(box(x - gx - 24, 35, x - gx + 24, h - 45), fill=(62, 22, 4))
    return im.filter(ImageFilter.GaussianBlur(12 * K))


def top_fade():
    mask = Image.new("L", box(L.RENDER_W, L.RENDER_H))
    d = ImageDraw.Draw(mask)
    height = L.BLEND_TOP_PT * K
    for y in range(height):
        d.line((0, y, mask.width, y), fill=round(255 * (1 - y / height)))
    return mask


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "plugin", "resources", "img"))
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    assets = L.manifest()["assets"]
    existing = [a["file"] for a in assets.values() if os.path.exists(os.path.join(args.out, a["file"]))]
    if existing and not args.force:
        sys.exit(f"{len(existing)} assets already exist in {args.out}; pass --force to replace them")

    draw = dict(knob=knob, button=button, toggle=toggle, rocker=rocker)
    for name, asset in assets.items():
        kind = asset["kind"]
        if kind == "plate":
            image = Image.composite(Image.new("RGB", box(L.RENDER_W, L.RENDER_H), L.BAR_COLOUR),
                                    background(), top_fade())
            image_io.save_opaque(args.out, asset["file"], image, K)
        elif kind == "additive":
            image_io.save_opaque(args.out, asset["file"], tubes(), K)
        else:
            count = asset["frames"]
            if kind == "glow":
                frames = [glow(name[len("glow_"):])]
            elif kind == "knob_light":
                frames = [cell(kind)[0] for _ in range(count)]
            else:
                frames = [draw[kind](i / (count - 1)) for i in range(count)]
            image_io.save_atlas(args.out, asset["file"], frames, asset["columns"], K)
    targets = [os.path.join(args.out, "layout.json")]
    if os.path.samefile(args.out, os.path.join(REPO, "plugin", "resources", "img")):
        targets.append(os.path.join(HERE, "layout.json"))
    L.write_manifest(*targets)
    print("placeholders written to", args.out)


if __name__ == "__main__":
    main()
