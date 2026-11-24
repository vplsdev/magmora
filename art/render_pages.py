"""Render and pack the section pages (pedal, EQ, cabinet) into plugin/resources/img and pages.json.

  python3 art/render_pages.py [--scene pedal ...] [--pack-only]

Rendering goes through art/render_page_passes.py in short, capped Blender runs that resume where the
last one stopped. Packing reads the passes and the scene's contract.json and writes, per page: a plate
(<page>_bg.jpg), knob atlases, two-state sprites, lamp sprites solved against the plate, and the EQ display's
glass (its reflections, which the plug-in adds over the live display). Runs under any python3 (see deps.py).
"""
import argparse
import json
import os
import subprocess
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
from render_assets import blend_top, changed, solve  # noqa: E402

K = 2
SCENES = ("pedal", "eq", "cab")
ORDER = ("pedal", "eq", "cab")


def load(path, mode="RGB"):
    return np.asarray(Image.open(path).convert(mode)).astype(np.float32)


def cell_slice(x, y, w, h):
    left, top = round((x - w / 2) * K), round((y - h / 2) * K)
    return np.s_[top:top + h * K, left:left + w * K]


def pack(scene, src, out):
    with open(os.path.join(src, "contract.json")) as f:
        c = json.load(f)
    byid = {e["id"]: e for e in c["controls"]}
    page = dict(title=c["title"], background=f"{scene}_bg.jpg", assets={}, controls=[])
    plate = load(os.path.join(src, "plate.png"))
    image_io.save_opaque(out, f"{scene}_bg.jpg", Image.fromarray((blend_top(plate) + 0.5).astype(np.uint8)), K)
    page["assets"][f"{scene}_bg"] = dict(file=f"{scene}_bg.jpg", w=L.RENDER_W, h=L.RENDER_H)
    for name, spec in c["assets"].items():
        kind = spec["kind"]
        if kind == "knob":
            w, h = spec["cell"]
            ref = byid[spec["ref"]]
            sl = cell_slice(ref["x"], ref["y"], w, h)
            frames = [Image.fromarray(np.asarray(Image.open(os.path.join(src, name, f"{i:02d}.png")).convert("RGBA"))[sl])
                      for i in range(c["frames"])]
            image_io.save_atlas(out, name + ".png", frames, 10, K)
            page["assets"][name] = dict(file=name + ".png", w=w * 10, h=h * 10, cell=[w, h], columns=10,
                                        frames=c["frames"])
        elif kind == "switch":
            w, h = spec["cell"]
            ref = byid[spec["ref"]]
            sl = cell_slice(ref["x"], ref["y"], w, h)
            frames = [Image.fromarray(np.asarray(Image.open(os.path.join(src, f"{name}_{i}.png")).convert("RGBA"))[sl])
                      for i in (0, 1)]
            image_io.save_atlas(out, name + ".png", frames, 2, K)
            page["assets"][name] = dict(file=name + ".png", w=w * 2, h=h, cell=[w, h], columns=2, frames=2)
        elif kind == "lamp":
            w, h = spec["cell"]
            ref = byid[spec["ref"]]
            sl = cell_slice(ref["x"], ref["y"], w, h)
            lit = load(os.path.join(src, "lit.png"))[sl]
            base = plate[sl]
            weight = changed(base, lit)[..., None]
            sprite = solve(base, base + (lit - base) * weight)
            image_io.save_atlas(out, name + ".png", [sprite], 1, K)
            page["assets"][name] = dict(file=name + ".png", w=w, h=h, cell=[w, h], columns=1, frames=1)
        elif kind == "glass":
            win = byid[spec["window"]]
            x0, y0 = round((win["x"] - win["w"] / 2) * K), round((win["y"] - win["h"] / 2) * K)
            w, h = int(win["w"]), int(win["h"])
            crop = Image.fromarray((plate[y0:y0 + h * K, x0:x0 + w * K] + 0.5).astype(np.uint8))
            image_io.save_opaque(out, name + ".jpg", crop, K)
            page["assets"][name] = dict(file=name + ".jpg", w=w, h=h, window=spec["window"])
    for e in c["controls"]:
        item = dict(e)
        item["y"] = e["y"] + L.BAR_H if "y" in e else None
        if e["kind"] in ("knob", "selector", "switch", "action", "lamp"):
            a = page["assets"][e["asset"]]
            item.update(w=a["cell"][0], h=a["cell"][1], frames=a["frames"], columns=a["columns"])
        if item["y"] is None:
            item.pop("y")
        item.pop("label", None)
        page["controls"].append(item)
    return page


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", nargs="*", default=list(SCENES))
    ap.add_argument("--passes", default=os.path.join(deps.build_dir(), "pagepasses"))
    ap.add_argument("--out", default=os.path.join(REPO, "plugin", "resources", "img"))
    ap.add_argument("--pack-only", action="store_true")
    args = ap.parse_args()
    if not args.pack_only:
        for scene in args.scene:
            while True:
                done = subprocess.run([os.environ.get("BLENDER", "blender"), "-t", "3", "-b", "--factory-startup",
                                       "-P", os.path.join(HERE, "render_page_passes.py"), "--", "--scene", scene,
                                       args.passes], timeout=170, stdout=subprocess.DEVNULL)
                if done.returncode != 3:
                    break
            if done.returncode:
                sys.exit(f"render_page_passes failed for {scene} ({done.returncode})")
    path = os.path.join(args.out, "pages.json")
    pages = {}
    if os.path.exists(path):
        with open(path) as f:
            pages = json.load(f).get("pages", {})
    for scene in args.scene:
        pages[scene] = pack(scene, os.path.join(args.passes, scene), args.out)
    doc = dict(version=1, order=[p for p in ORDER if p in pages],
               units="points on the 1280 x 620 canvas; x and y are centres",
               bar_h=L.BAR_H, pages=pages)
    with open(path + ".tmp", "w") as f:
        json.dump(doc, f, indent=1)
    os.replace(path + ".tmp", path)
    print("pages written:", ", ".join(doc["order"]))


if __name__ == "__main__":
    main()
