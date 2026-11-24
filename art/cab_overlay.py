"""What the plug-in draws over the cabinet page, in Python for previews and composites (mirrors MicMarker and the
slot windows in plugin/ui/controls/Pages.h)."""
from PIL import Image, ImageDraw, ImageFilter, ImageFont
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(os.path.dirname(HERE), "plugin", "resources", "fonts")
MAX_DISTANCE = 300.0


def marker_at(cone, n, position, distance):
    """Centre and radius (canvas points) of slot n's marker on its cone."""
    cx, cy, r = cone["x"], cone["y"], cone["w"] / 2
    side = -1.0 if n == 1 else 1.0
    x = cx + side * position * r * 0.96
    y = cy + distance / MAX_DISTANCE * r * 0.86
    size = 22.0 * (1.0 - 0.35 * distance / MAX_DISTANCE)
    return x, y, size


def draw(img, page, values, names, ir=None, scale=2):
    """values: parameter values; names: what each slot's window shows; ir: slot holding the response file."""
    s = scale
    base = img.convert("RGBA")
    over = Image.new("RGBA", base.size)
    d = ImageDraw.Draw(over)
    shadow = Image.new("RGBA", base.size)
    sd = ImageDraw.Draw(shadow)
    sans = ImageFont.truetype(os.path.join(FONT, "Barlow-Medium.ttf"), int(12.5 * s))
    panel = ImageFont.truetype(os.path.join(FONT, "BarlowCondensed-SemiBold.ttf"), int(11 * s))
    small = ImageFont.truetype(os.path.join(FONT, "BarlowCondensed-SemiBold.ttf"), int(9.5 * s))
    byid = {c["id"]: c for c in page["controls"]}
    for n in (1, 2):
        cone = byid[f"cone{n}"]
        cx, cy, r = cone["x"] * s, cone["y"] * s, cone["w"] / 2 * s
        if ir == n:
            d.ellipse([cx - r * 1.12, cy - r * 1.12, cx + r * 1.12, cy + r * 1.12], fill=(8, 7, 6, 160))
            d.text((cx, cy - 10 * s), "RESPONSE FILE", font=panel, fill=(196, 180, 150, 255), anchor="mm")
            d.text((cx, cy + 10 * s), names.get("ir", ""), font=sans, fill=(238, 200, 140, 255), anchor="mm")
            continue
        if not values.get(f"mic{n}_on", 1):
            continue
        x, y, size = marker_at(cone, n, values[f"mic{n}_position"], values[f"mic{n}_distance"])
        x, y, size = x * s, y * s, size * s
        # the axis from the cap: where the marker can go
        d.line([(cx, y), (x, y)], fill=(230, 214, 186, 70), width=max(1, s))
        sd.ellipse([x - size + 2 * s, y - size + 3 * s, x + size + 2 * s, y + size + 3 * s], fill=(0, 0, 0, 150))
        d.ellipse([x - size, y - size, x + size, y + size], fill=(30, 26, 22, 230), outline=(240, 214, 160, 255),
                  width=int(2.5 * s))
        d.ellipse([x - size * 0.55, y - size * 0.55, x + size * 0.55, y + size * 0.55], outline=(236, 222, 196, 90),
                  width=max(1, s))
        d.text((x, y + 0.5 * s), str(n), font=panel, fill=(244, 232, 210, 255), anchor="mm")
        text = f"{values[f'mic{n}_position'] * 100:.0f} %   {values[f'mic{n}_distance']:.0f} mm"
        d.text((x, y + size + 11 * s), text, font=small, fill=(230, 214, 186, 200), anchor="mm")
    shadow = shadow.filter(ImageFilter.GaussianBlur(3 * s))
    for n in (1, 2):
        w = byid[f"slot{n}_window"]
        x0, y0 = (w["x"] - w["w"] / 2) * s, (w["y"] - w["h"] / 2) * s
        d.text((x0 + 6 * s, y0 + 3 * s), "SOURCE", font=small, fill=(150, 132, 104, 255))
        d.text((x0 + 6 * s, y0 + 13 * s), names.get(n, ""), font=sans, fill=(238, 200, 140, 255))
        d.text((x0 + (w["w"] - 9) * s, y0 + 20 * s), "v", font=small, fill=(238, 200, 140, 200), anchor="mm")
    out = Image.alpha_composite(base, shadow)
    return Image.alpha_composite(out, over).convert("RGB")
