"""What the EQ's screen shows, drawn in Python for previews and composites. The plug-in draws the same thing in
plugin/ui/controls/Pages.h (EqScreen): the analogue prototypes of dsp/src/eq/Design.cpp, summed in dB."""
import math

from PIL import Image, ImageDraw, ImageFilter

LO_HZ, HI_HZ, RANGE_DB = 20.0, 20000.0, 18.0
BW2, BW4 = 0.70710678118654752, (0.54119610014619698, 1.30656296487637652)


def prototype(kind, gain_db, q):
    q = max(q, 1e-3)
    a = 10 ** (gain_db / 40)
    r = math.sqrt(a)
    return {"hp": (1, 0, 0, 1, 1 / q, 1), "lp": (0, 0, 1, 1, 1 / q, 1), "bell": (1, a / q, 1, 1, 1 / (a * q), 1),
            "lowshelf": (a, a * r / q, a * a, a, r / q, 1), "highshelf": (a * a, a * r / q, a, 1, r / q, a)}[kind]


def mag_db(kind, hz, gain_db, q, f):
    n2, n1, n0, d2, d1, d0 = prototype(kind, gain_db, q)
    x = complex(0, f / hz)
    h = (n2 * x * x + n1 * x + n0) / (d2 * x * x + d1 * x + d0)
    return 20 * math.log10(max(abs(h), 1e-9))


def sections(v):
    out = []
    for i in range(int(v["eq_hp_slope"])):
        out.append(("hp", v["eq_hp_freq"], 0, BW2 if v["eq_hp_slope"] == 1 else BW4[i]))
    for i in range(int(v["eq_lp_slope"])):
        out.append(("lp", v["eq_lp_freq"], 0, BW2 if v["eq_lp_slope"] == 1 else BW4[i]))
    for b in (1, 2, 3, 4):
        kind = "bell"
        if b == 1 and v["eq_b1_type"]:
            kind = "lowshelf"
        if b == 4 and v["eq_b4_type"]:
            kind = "highshelf"
        out.append((kind, v[f"eq_b{b}_freq"], v[f"eq_b{b}_gain"], v[f"eq_b{b}_q"]))
    return out


def draw(img, rect, v, colours, scale=2, on=True, glass=None):
    """The display behind the glass, then the glass's own reflections (the plate's pixels there) added over it."""
    x0, y0, x1, y1 = (c * scale for c in rect)
    w, h = x1 - x0, y1 - y0
    W, H = int(w), int(h)
    import numpy as np
    # a backlit panel: brightest in the middle, falling off to the edges, a faint warm cast; a fine pixel lattice
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    fall = (1 - ((xx / W - 0.5) * 1.6) ** 2) * (1 - ((yy / H - 0.5) * 1.9) ** 2)
    glow = np.clip(fall, 0, 1) ** 0.7
    panel = np.stack([10 + 16 * glow, 10 + 14 * glow, 9 + 11 * glow], -1)
    lattice = ((xx % (1.5 * scale) < 0.6 * scale) | (yy % (1.5 * scale) < 0.6 * scale)).astype(np.float32)
    panel *= (1 - 0.18 * lattice)[..., None]
    screen = Image.fromarray(np.clip(panel, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    layer = Image.new("RGBA", (W, H))
    d = ImageDraw.Draw(layer)
    fx = lambda f: (math.log(f / LO_HZ) / math.log(HI_HZ / LO_HZ)) * w
    fy = lambda db: h / 2 - db / RANGE_DB * (h / 2 - 10 * scale)
    grid = (170, 160, 140, 34)
    for f in (50, 100, 200, 500, 1000, 2000, 5000, 10000):
        d.line([(fx(f), 6 * scale), (fx(f), h - 6 * scale)], fill=grid, width=1)
    for db in (-12, -6, 6, 12):
        d.line([(6 * scale, fy(db)), (w - 6 * scale, fy(db))], fill=grid, width=1)
    d.line([(6 * scale, fy(0)), (w - 6 * scale, fy(0))], fill=(190, 178, 156, 70), width=1)
    try:
        from PIL import ImageFont
        import os
        font = ImageFont.truetype(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                               "plugin", "resources", "fonts", "BarlowCondensed-Medium.ttf"), int(8.5 * scale))
        for f, t in ((100, "100"), (1000, "1k"), (10000, "10k")):
            d.text((fx(f) + 3 * scale, h - 14 * scale), t, font=font, fill=(190, 176, 150, 120))
        for db, t in ((12, "+12"), (-12, "-12")):
            d.text((7 * scale, fy(db) - 11 * scale), t, font=font, fill=(190, 176, 150, 110))
    except OSError:
        pass
    secs = sections(v)
    pts = []
    for i in range(0, W + 1, 2):
        f = LO_HZ * (HI_HZ / LO_HZ) ** (i / w)
        db = sum(mag_db(k, hz, g, q, f) for k, hz, g, q in secs) if on else 0.0
        pts.append((i, min(max(fy(db), 2), h - 2)))
    ink = (255, 214, 150, 255) if on else (130, 120, 104, 200)
    fill = Image.new("RGBA", (W, H))
    ImageDraw.Draw(fill).polygon(pts + [(w, fy(0)), (0, fy(0))], fill=(255, 190, 110, 30))
    bloom = Image.new("RGBA", (W, H))
    ImageDraw.Draw(bloom).line(pts, fill=ink[:3] + (170,), width=int(5 * scale))
    bloom = bloom.filter(ImageFilter.GaussianBlur(4 * scale))
    d.line(pts, fill=ink, width=int(1.5 * scale))
    nodes = Image.new("RGBA", (W, H))
    nd = ImageDraw.Draw(nodes)
    for b in (1, 2, 3, 4):
        hz, g = v[f"eq_b{b}_freq"], v[f"eq_b{b}_gain"]
        cx, cy = fx(hz), fy(sum(mag_db(k, h2, g2, q2, hz) for k, h2, g2, q2 in secs) if on else 0)
        col = tuple(int(255 * c ** (1 / 2.2)) for c in colours[f"b{b}"])
        r = 5.5 * scale
        nd.ellipse([cx - r * 2.2, cy - r * 2.2, cx + r * 2.2, cy + r * 2.2], fill=col + (60,))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col + (255,))
        d.ellipse([cx - r * 0.45, cy - r * 0.45, cx + r * 0.45, cy + r * 0.45], fill=(255, 250, 240, 200))
    nodes = nodes.filter(ImageFilter.GaussianBlur(3 * scale))
    for key, f in (("hp", v["eq_hp_freq"]), ("lp", v["eq_lp_freq"])):
        if (v["eq_hp_slope"] if key == "hp" else v["eq_lp_slope"]):
            col = tuple(int(255 * c ** (1 / 2.2)) for c in colours[key])
            cx, cy = fx(f), fy(sum(mag_db(k, h2, g2, q2, f) for k, h2, g2, q2 in secs) if on else 0)
            r = 4.5 * scale
            d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=col + (230,), width=int(1.5 * scale))
    for over in (fill, bloom, nodes, layer):
        screen = Image.alpha_composite(screen, over)
    out = img.convert("RGB").copy()
    box = (int(x0), int(y0), int(x0) + W, int(y0) + H)
    content = np.asarray(screen.convert("RGB")).astype(np.float32)
    reflection = np.asarray(img.convert("RGB").crop(box)).astype(np.float32) if glass is None else glass
    out.paste(Image.fromarray(np.clip(content + reflection, 0, 255).astype(np.uint8)), box[:2])
    return out


DEMO = dict(eq_hp_freq=80.0, eq_hp_slope=1, eq_lp_freq=11000.0, eq_lp_slope=1, eq_b1_freq=110.0, eq_b1_gain=2.0,
            eq_b1_q=0.8, eq_b1_type=0, eq_b2_freq=420.0, eq_b2_gain=-4.0, eq_b2_q=1.2, eq_b3_freq=1700.0,
            eq_b3_gain=3.0, eq_b3_q=1.0, eq_b4_freq=6000.0, eq_b4_gain=-2.5, eq_b4_q=2.5, eq_b4_type=0)
