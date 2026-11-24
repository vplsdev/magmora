"""Cycles passes for a section scene, resumable, for render_pages.py to pack.

  blender -b --factory-startup -P art/render_page_passes.py -- --scene pedal OUT_DIR [--budget 130]

Each run renders what is missing or older than the scene's sources and stops starting new renders after
`--budget` seconds (exit 3: more to do), so that every command stays short. Passes:

  plate.png            the scene, every control invisible to the camera but casting its shadow, lamps dark
  lit.png              the same with every lamp lit (lamp windows only)
  <atlas>/NN.png       a knob atlas: the knobs alone at frame NN, transparent
  <sprite>_N.png       a two-state control alone, state N, transparent
  ref.png              an ordinary render of the default state, to judge the composite against
"""
import argparse
import glob
import math
import os
import shutil
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import bpy  # noqa: E402

import build_page  # noqa: E402
import lighting  # noqa: E402

MIN_FREE_GB = 4.0      # keep this much free disk space
SAMPLES = dict(plate=512, lit=256, knob=96, sprite=128, ref=512)
FRAMES = 97


def newest_source():
    files = glob.glob(os.path.join(HERE, "pages", "*.py")) + glob.glob(os.path.join(HERE, "parts", "*.py"))
    files += [os.path.join(HERE, f) for f in ("build_page.py", "materials.py", "meshes.py", "lighting.py", "textures.py",
                                              "glyphs.py")]
    return max(os.path.getmtime(f) for f in files)


class Runner:
    def __init__(self, scene, out, budget):
        self.scene, self.out, self.budget, self.started = scene, out, budget, time.time()
        self.stale_before = newest_source()
        self.more = False

    def due(self, path):
        return not os.path.exists(path) or os.path.getmtime(path) < self.stale_before

    def render(self, path, samples, alpha=False, rect=None):
        if not self.due(path):
            return False
        if time.time() - self.started > self.budget:
            self.more = True
            return False
        free = shutil.disk_usage("/System/Volumes/Data" if os.path.isdir("/System/Volumes/Data") else "/").free / 1e9
        if free < MIN_FREE_GB:
            sys.exit(f"STOP: {free:.2f} GB free on the data volume, below {MIN_FREE_GB}")
        s = bpy.context.scene
        s.cycles.samples = samples
        s.render.film_transparent = alpha
        s.render.image_settings.color_mode = "RGBA" if alpha else "RGB"
        s.render.dither_intensity = 0.0 if alpha else 0.5
        r = s.render
        r.use_border = rect is not None
        r.use_crop_to_border = False
        if rect:
            x0, y0, x1, y1 = rect
            r.border_min_x, r.border_max_x = max(x0 / 1280, 0), min(x1 / 1280, 1)
            r.border_min_y, r.border_max_y = max(1 - y1 / 576, 0), min(1 - y0 / 576, 1)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        r.filepath = path
        t = time.time()
        bpy.ops.render.render(write_still=True)
        print(f"PASS {os.path.relpath(path, self.out)} {time.time() - t:.1f} s", flush=True)
        return True


def controls_by_id(scene):
    return {c["id"]: c for c in scene.CONTROLS}


def window_around(c, cell, pad=6):
    w, h = cell
    return (c["x"] - w / 2 - pad, c["y"] - h / 2 - pad, c["x"] + w / 2 + pad, c["y"] + h / 2 + pad)


def only(collections):
    """Camera sees only these collections' meshes; everything else still casts and reflects."""
    keep = set()
    for name in collections:
        keep |= {o.name for o in bpy.data.collections[name].all_objects}
    for o in bpy.data.objects:
        if o.type == "MESH":
            o.visible_camera = o.name in keep


def all_visible():
    for o in bpy.data.objects:
        if o.type == "MESH":
            o.visible_camera = True


def set_switch(obj, spec, on):
    if "travel" in spec:
        obj.location.z = -spec["travel"] * 0.001 if on else 0.0
    if "tilt" in spec:
        obj.rotation_euler.x = math.radians(spec["tilt"] if on else -spec["tilt"])
    if "tilt_y" in spec:   # sideways: off to the left (PRE), on to the right (POST)
        obj.rotation_euler.y = math.radians(spec["tilt_y"] if on else -spec["tilt_y"])


def switch_objects(spec):
    names = {c["id"] for c in SCENE.CONTROLS if c.get("asset") in (spec["name"],)}
    return [bpy.data.objects[n] for n in names if n in bpy.data.objects]


def write_contract(scene, out):
    """What the packer needs to know about the scene, in plain JSON (the packer runs without Blender)."""
    import json
    data = dict(scene=scene.__name__.split(".")[-1], title=scene.TITLE, controls=scene.CONTROLS,
                assets=scene.ASSETS, frames=FRAMES, defaults=scene.DEFAULTS)
    with open(os.path.join(out, "contract.json"), "w") as f:
        json.dump(data, f, indent=1)


def main():
    global SCENE
    argv = sys.argv[sys.argv.index("--") + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", required=True)
    ap.add_argument("out")
    ap.add_argument("--budget", type=float, default=130.0)
    ap.add_argument("--only", default="plate,lit,knobs,sprites,ref")
    args = ap.parse_args(argv)
    out = os.path.join(os.path.abspath(args.out), args.scene)
    os.makedirs(out, exist_ok=True)
    SCENE = scene = build_page.build(args.scene)
    build_page.set_state(scene, "default")
    lighting.render_settings(2, 128)
    s = bpy.context.scene
    s.cycles.use_animated_seed = False
    run = Runner(scene, out, args.budget)
    write_contract(scene, out)
    byid = controls_by_id(scene)
    wanted = args.only.split(",")
    # plate and lit: controls camera-invisible, mics gone
    if "plate" in wanted or "lit" in wanted:
        all_visible()
        for name in ("Knob", "Button", "Toggle"):
            for o in bpy.data.collections[name].all_objects:
                if o.type == "MESH":
                    o.visible_camera = False
        build_page.set_state(scene, "dark")
        run.render(os.path.join(out, "plate.png"), SAMPLES["plate"])
        lamps = [c for c in scene.CONTROLS if c["kind"] == "lamp"]
        if lamps and "lit" in wanted:
            for mat in bpy.data.materials:
                if mat.name.startswith("lamp_"):
                    mat.node_tree.nodes["lit"].outputs[0].default_value = 1.0
            xs = [c["x"] for c in lamps]
            ys = [c["y"] for c in lamps]
            run.render(os.path.join(out, "lit.png"), SAMPLES["lit"],
                       rect=(min(xs) - 40, min(ys) - 40, max(xs) + 40, max(ys) + 40))
        build_page.set_state(scene, "default")

    for name, spec in scene.ASSETS.items():
        spec = dict(spec, name=name)
        if spec["kind"] == "knob" and "knobs" in wanted:
            only([spec["collection"]])
            rect = window_around(byid[spec["ref"]], spec["cell"])
            for i in range(FRAMES):
                path = os.path.join(out, name, f"{i:02d}.png")
                if not run.due(path):
                    continue
                for o in bpy.data.collections[spec["collection"]].all_objects:
                    if "turn" in o:
                        o["turn"] = i / (FRAMES - 1)
                        o.update_tag()
                bpy.context.view_layer.update()
                run.render(path, SAMPLES["knob"], alpha=True, rect=rect)
        elif spec["kind"] == "switch" and "sprites" in wanted:
            only([spec["collection"]])
            rect = window_around(byid[spec["ref"]], spec["cell"])
            for state in (0, 1):
                for o in switch_objects(spec):
                    set_switch(o, spec, state)
                bpy.context.view_layer.update()
                run.render(os.path.join(out, f"{name}_{state}.png"), SAMPLES["sprite"], alpha=True, rect=rect)
            for o in switch_objects(spec):
                set_switch(o, spec, 0)
    if "ref" in wanted:
        all_visible()
        build_page.set_state(scene, "default")
        run.render(os.path.join(out, "ref.png"), SAMPLES["ref"])
    print("MORE" if run.more else "DONE", flush=True)
    sys.exit(3 if run.more else 0)


if __name__ == "__main__":
    main()
