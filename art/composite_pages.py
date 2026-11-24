"""Assemble a section page from the shipped files the way the plug-in draws it, and diff it with a full render.

  python3 art/composite_pages.py OUT_DIR [--scene pedal ...] [--state default|off]

Values come from the scene's defaults (what ref.png shows). Uses composite.Window's NanoVG rules.
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

import layout as L  # noqa: E402
from composite import Window  # noqa: E402
from render_assets import blend_top  # noqa: E402


def draw(folder, page, values, scale=2):
    win = Window.__new__(Window)
    win.folder, win.cache = folder, {}
    win.layout = dict(assets={k: dict(file=a["file"]) for k, a in page["assets"].items()})
    s = scale
    out = np.empty((620 * s, 1280 * s, 3), np.float32)
    out[:] = [int(L.BAR_COLOUR[i:i + 2], 16) for i in (1, 3, 5)]
    bg = [k for k in page["assets"] if k.endswith("_bg")][0]
    Window.blit(out, win.texture(bg, scale), (0, L.BAR_H, 1280, 576), (0, 0), s, scale)
    for c in page["controls"]:
        kind = c["kind"]
        if kind == "window":
            continue
        a = page["assets"][c["asset"]]
        w, h = a["cell"]
        frame = 0
        weight = 1.0
        if kind in ("knob", "selector"):
            frame = math.floor(values.get(c["id"], 0.5) * (a["frames"] - 1) + 0.5)
        elif kind in ("switch", "action"):
            frame = int(values.get(c["id"], 0))
        elif kind == "lamp":
            weight = 1.0 if int(values.get(c["source"], 0)) == c.get("on_value", 1) else 0.0
            if not weight:
                continue
        if c.get("disabled"):
            weight *= 0.45
        src = (frame % a["columns"] * w, frame // a["columns"] * h)
        Window.blit(out, win.texture(c["asset"], scale), (c["x"] - w / 2, c["y"] - h / 2, w, h), src, s, scale,
                    "over", weight)
    return Image.fromarray((out + 0.5).clip(0, 255).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--scene", nargs="*", default=["pedal", "eq", "cab"])
    ap.add_argument("--passes", default=os.path.join(deps.build_dir(), "pagepasses"))
    ap.add_argument("--assets", default=os.path.join(REPO, "plugin", "resources", "img"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.assets, "pages.json")) as f:
        doc = json.load(f)
    for scene in args.scene:
        page = doc["pages"][scene]
        with open(os.path.join(args.passes, scene, "contract.json")) as f:
            contract = json.load(f)
        values = dict(contract.get("defaults", {}))
        shot = draw(args.assets, page, values)
        if scene == "eq":
            import eq_screen
            win = [c for c in page["controls"] if c["id"] == "eq_screen"][0]
            rect = (win["x"] - win["w"] / 2, win["y"] - win["h"] / 2, win["x"] + win["w"] / 2, win["y"] + win["h"] / 2)
            shot = eq_screen.draw(shot, rect, dict(eq_screen.DEMO, **{k: v for k, v in values.items() if k in eq_screen.DEMO}),
                                  win["colours"])
        if scene == "cab":
            import cab_overlay
            shot = cab_overlay.draw(shot, page, values, {1: "Dynamic A", 2: "Ribbon B"})
        shot.save(os.path.join(args.out, f"composite_{scene}_2x.png"))
        (cab_overlay.draw(draw(args.assets, page, values, scale=1), page, values, {1: "Dynamic A", 2: "Ribbon B"}, scale=1)
         if scene == "cab" else draw(args.assets, page, values, scale=1)).save(os.path.join(args.out, f"composite_{scene}_1x.png"))
        ref = os.path.join(args.passes, scene, "ref.png")
        if os.path.exists(ref):
            truth = blend_top(np.asarray(Image.open(ref).convert("RGB")).astype(np.float32))
            got = np.asarray(shot).astype(np.float32)[L.BAR_H * 2:]
            d = np.abs(got - truth).mean(2)
            Image.fromarray(np.clip(d * 6, 0, 255).astype(np.uint8)).save(os.path.join(args.out, f"diff_{scene}_x6.png"))
            print(f"{scene:6} composite vs render: mean {d.mean():.2f}  p99 {np.percentile(d, 99):.1f}  max {d.max():.0f}")


if __name__ == "__main__":
    main()
