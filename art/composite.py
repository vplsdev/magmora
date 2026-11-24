"""Assemble the window from the shipped assets the way the plugin does, and diff it with full renders.

  python3 art/composite.py OUT_DIR [--refs DIR] [--assets plugin/resources/img]

Only layout.json and the packed files are read, with NanoVG's rules: straight-alpha textures,
bilinear sampling without mipmaps, premultiplied source-over, weight squared for Add.
"""
import argparse
import json
import math
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import deps  # noqa: E402

deps.ensure()

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import image_io  # noqa: E402
import layout as L  # noqa: E402
from render_assets import blend_top  # noqa: E402
from states import STATES, on_frames  # noqa: E402


class Window:
    def __init__(self, folder):
        self.folder = folder
        with open(os.path.join(folder, "layout.json"), encoding="utf-8") as f:
            self.layout = json.load(f)
        self.cache = {}

    def texture(self, asset, scale):
        if (asset, scale) not in self.cache:
            name = image_io.names(self.folder, self.layout["assets"][asset]["file"], scale)
            self.cache[asset, scale] = Image.open(name).convert("RGBA")
        return self.cache[asset, scale]

    def draw(self, values, scale=2, zoom=1.0, intensity=1.0):
        """`scale` picks the texture set (the screen's), `zoom` is the plugin's resize."""
        m, s = self.layout, scale * zoom
        out = np.empty((round(m["canvas"]["h"] * s), round(m["canvas"]["w"] * s), 3), np.float32)
        out[:] = [int(m["bar"]["colour"][i:i + 2], 16) for i in (1, 3, 5)]
        b = m["background"]
        self.blit(out, self.texture(b["asset"], scale), (b["x"], b["y"], b["w"], b["h"]), (0, 0), s, scale)
        for c in m["controls"]:
            frame, weight = 0, 1.0
            if "frame" in c:
                frame = c["frame"]
            elif c["type"] == "glow":
                on = int(values[c["source"]]) == c["on_value"] and bool(values[c["requires"]])
                weight = intensity if on else 0.0
            else:
                frame = math.floor(float(values[c["id"]]) * (c["frames"] - 1) + 0.5)
            if not weight:
                continue
            source = (frame % c["columns"] * c["w"], frame // c["columns"] * c["h"])
            dest = (c["x"] - c["w"] / 2, c["y"] - c["h"] / 2, c["w"], c["h"])
            self.blit(out, self.texture(c["asset"], scale), dest, source, s, scale, c.get("blend"), weight)
        return Image.fromarray((out + 0.5).clip(0, 255).astype(np.uint8))

    @staticmethod
    def blit(out, texture, dest, source, s, scale, blend="over", weight=1.0):
        x, y, w, h = dest
        x0, y0 = math.floor(x * s + 0.5), math.floor(y * s + 0.5)
        x1, y1 = math.floor((x + w) * s + 0.5), math.floor((y + h) * s + 0.5)
        step = scale / s
        affine = (step, 0, source[0] * scale + (x0 / s - x) * scale,
                  0, step, source[1] * scale + (y0 / s - y) * scale)
        # straight RGBA is filtered channel by channel, as the GPU does
        texels = np.dstack([np.asarray(band.transform((x1 - x0, y1 - y0), Image.Transform.AFFINE, affine,
                                                      Image.Resampling.BILINEAR))
                            for band in texture.split()]).astype(np.float32)
        a = texels[..., 3:] / 255.0 * weight
        under = out[y0:y1, x0:x1]
        if blend == "add":
            under[:] = np.minimum(texels[..., :3] * a * a + under, 255)
        else:
            under[:] = texels[..., :3] * a + under * (1 - a)


def regions(layout):
    top = layout["bar"]["h"]
    out = [("whole", (0, 0, L.RENDER_W, L.RENDER_H)), ("grille", L.GRILLE), ("plate", L.PLATE)]
    for c in layout["controls"]:
        if c["type"] == "knob_light" or c["id"] == "tubes":
            continue
        x, y, w, h = c["x"], c["y"] - top, c["w"], c["h"]
        if c["type"] == "glow":
            w, h = 22, 22
        out.append((c["id"], (x - w / 2, y - h / 2, x + w / 2, y + h / 2)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--refs", default=os.path.join(deps.build_dir(), "passes"))
    ap.add_argument("--assets", default=os.path.join(REPO, "plugin", "resources", "img"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    window = Window(args.assets)
    top = window.layout["bar"]["h"] * L.SCALE
    report = {}
    for state, values in STATES.items():
        shot = window.draw(values)
        shot.save(os.path.join(args.out, f"composite_{state}_2x.png"))
        if state in ("hero", "off", "standby", "rhythm"):
            window.draw(values, scale=1).save(os.path.join(args.out, f"composite_{state}_1x.png"))
            window.draw(values, zoom=0.75).save(os.path.join(args.out, f"composite_{state}_75_2x.png"))
            window.draw(values, scale=1, zoom=0.75).save(os.path.join(args.out, f"composite_{state}_75_1x.png"))
        path = os.path.join(args.refs, f"ref_{state}.png")
        if not os.path.exists(path):
            continue
        truth = blend_top(np.asarray(Image.open(path).convert("RGB")).astype(np.float32))
        drawn = np.asarray(window.draw(on_frames(values))).astype(np.float32)[top:]
        diff = np.abs(drawn - truth).mean(2)
        table = {}
        for name, box in regions(window.layout):
            x0, y0, x1, y1 = (round(v * L.SCALE) for v in box)
            d = diff[y0:y1, x0:x1]
            table[name] = dict(mean=round(float(d.mean()), 2), p99=round(float(np.percentile(d, 99)), 1))
        report[state] = table
        Image.fromarray(np.clip(diff * 6, 0, 255).astype(np.uint8)).save(
            os.path.join(args.out, f"diff_{state}_x6.png"))
        for group, names in (("whole", ["whole"]), ("grille", ["grille"]), ("plate", ["plate"]),
                             ("worst knob", [k for k, *_ in L.KNOBS]),
                             ("worst switch", [k for k, *_ in L.BUTTONS] + [L.TOGGLE[0]] + [r[0] for r in L.ROCKERS]),
                             ("worst lamp", [item[0] for item in L.LAMPS])):
            worst = max(names, key=lambda n: table[n]["mean"])
            print(f"{state:8} {group:13} {worst:13} mean {table[worst]['mean']:6.2f}  p99 {table[worst]['p99']:6.1f}")
    with open(os.path.join(args.out, "mismatch.json"), "w", encoding="utf-8") as f:
        json.dump(dict(unit="levels of 255, mean of RGB, composite against full render at 2x",
                       states=report), f, indent=1)


if __name__ == "__main__":
    main()
