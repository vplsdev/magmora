"""The macOS app icon grid (Big Sur and later): the tile, its squircle and its drop shadow.

numpy only, so the Blender scene (render_icon.py) can use the outline too; Pillow for the raster parts.
On a 1024 canvas the tile is 824 x 824, centred (100 px margin). Its continuous corners are drawn as a
superellipse of exponent 5, which matches the system's own app icon masks to within 2 to 3 px. The shadow is
y +10, blur 10, 30 % black, plus a 2 px contact shadow, all scaled with the canvas.
"""
import math

import numpy as np

CANVAS = 1024
TILE = 824
EXPONENT = 5.0


def squircle_outline(w, h, n=EXPONENT, steps=720):
    """Counter-clockwise outline of a superellipse with half-sizes w/2 and h/2, centred on the origin."""
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        pts.append((w / 2 * math.copysign(abs(c) ** (2 / n), c), h / 2 * math.copysign(abs(s) ** (2 / n), s)))
    return pts


def squircle_mask(size, ss=4):
    """Anti-aliased alpha (float, 0..1) of the tile on a size x size canvas."""
    from PIL import Image
    big = size * ss
    c = (big - 1) / 2
    r = size * TILE / CANVAS * ss / 2
    y, x = np.mgrid[0:big, 0:big].astype(np.float64)
    inside = ((np.abs(x - c) / r) ** EXPONENT + (np.abs(y - c) / r) ** EXPONENT <= 1.0).astype(np.uint8) * 255
    return np.asarray(Image.fromarray(inside).resize((size, size), Image.LANCZOS), np.float32) / 255.0


def tile(rgb):
    """RGB canvas -> RGBA with the squircle as its alpha."""
    from PIL import Image
    out = rgb.convert("RGBA")
    out.putalpha(Image.fromarray((squircle_mask(rgb.size[0]) * 255).round().astype(np.uint8)))
    return out


def with_shadow(rgba):
    """The macOS drop shadow under an RGBA canvas whose alpha is the tile."""
    from PIL import Image, ImageFilter
    k = rgba.size[0] / CANVAS
    alpha = rgba.getchannel("A")

    def shadow(opacity, drop, blur):
        layer = Image.new("RGBA", rgba.size, (0, 0, 0, 0))
        layer.putalpha(alpha.point(lambda v: int(v * opacity)))
        layer = layer.transform(layer.size, Image.AFFINE, (1, 0, 0, 0, 1, -drop * k))
        return layer.filter(ImageFilter.GaussianBlur(blur * k))

    out = Image.alpha_composite(shadow(0.30, 10, 10), shadow(0.18, 2, 2))
    return Image.alpha_composite(out, rgba)
