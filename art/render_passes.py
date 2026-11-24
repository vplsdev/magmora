"""The Cycles passes that render_assets.py packs. Runs inside Blender.

  blender -b --factory-startup -P art/render_passes.py -- OUT_DIR [--only static,knobs,refs] [--state NAME ...]
          [--since EPOCH] [--budget SECONDS]

static  plate.png         controls out of the picture, lamps dark, valves cold. Knobs still throw
                          their shadow, so every position keeps its own.
        warm.png          the same with warm valves
        bare.png          the plate round the switches and lamps, without the print
        switch_{0,1}.png  the same with all switches released / thrown, and their shadows
        mask_{0,1}.png    the switches alone, for their geometric alpha
        lamp_<id>.png     over switch_1, one lamp lit
knobs   knobs/NN.png      all nine knobs alone at frame NN, lit and reflecting as in the scene
refs    ref_<state>.png   ordinary full renders, to judge the composite against
"""
import argparse
import glob
import os
import shutil
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import bpy  # noqa: E402

import build_scene  # noqa: E402
import layout as L  # noqa: E402
import lighting  # noqa: E402
from parts import controls  # noqa: E402
from states import STATES, on_frames  # noqa: E402

SAMPLES = dict(plate=768, switch=512, lamp=384, knob=256, ref=768)
SWITCHES = ("Button", "Toggle", "Rocker")
KNOB_WINDOW = (L.KNOBS[0][1] - 36, L.CY - 36, L.KNOBS[-1][1] + 36, L.CY + 40)
CONTROL_WINDOW = (128, 290, 1194, 432)   # switches and lamps
PRINT = ("legend_", "scale_", "rule_", "leader_")
SCENE_SOURCES = ("build_scene.py", "layout.py", "lighting.py", "materials.py", "meshes.py", "textures.py",
                 "glyphs.py", "product.py", "states.py")
MIN_FREE_GB = 2.5                       # keep this much free disk space

def window(rect=None):
    """Render only this rectangle (points); the rest of the frame stays empty."""
    r = bpy.context.scene.render
    r.use_border = rect is not None
    r.use_crop_to_border = False
    if rect:
        x0, y0, x1, y1 = rect
        r.border_min_x, r.border_max_x = x0 / L.RENDER_W, x1 / L.RENDER_W
        r.border_min_y, r.border_max_y = 1 - y1 / L.RENDER_H, 1 - y0 / L.RENDER_H


# --since EPOCH re-renders every pass older than that moment (a change the scene sources cannot show, such as the
# product name read from plugin/config.h); --budget stops starting new renders after that many seconds (exit 3).
RUN = dict(since=0.0, budget=float("inf"), started=time.time(), more=False)


def render(path, samples, alpha=False):
    if RUN["since"] and os.path.exists(path) and os.path.getmtime(path) >= RUN["since"]:
        return
    if time.time() - RUN["started"] > RUN["budget"]:
        RUN["more"] = True
        return
    s = bpy.context.scene
    s.render.dither_intensity = 0.0 if alpha else 0.5
    s.cycles.samples = samples
    s.render.film_transparent = alpha
    s.render.image_settings.color_mode = "RGBA" if alpha else "RGB"
    s.render.filepath = path
    free = shutil.disk_usage("/").free / 1e9
    if free < MIN_FREE_GB:
        sys.exit(f"STOP: internal disk has {free:.2f} GB free, below {MIN_FREE_GB} GB (A7)")
    started = time.time()
    bpy.ops.render.render(write_still=True)
    print(f"PASS {os.path.basename(path)} {time.time() - started:.1f} s", flush=True)


def objects(*names):
    return [o for name in names for o in bpy.data.collections[name].all_objects if o.type == "MESH"]


def switches(value):
    return dict({key: value for key, *_ in L.BUTTONS}, **{L.TOGGLE[0]: value},
                **{key: value for key, *_ in L.ROCKERS})


def reset():
    build_scene.build()
    lighting.render_settings(L.SCALE, SAMPLES["plate"])


def static(out):
    reset()
    colls = bpy.data.collections
    controls.set_state(switches(0))
    controls.set_lamps({})
    controls.set_valves(0.0)
    for obj in objects("Knob"):
        obj.visible_camera = False
    for name in SWITCHES:
        colls[name].hide_render = True
    render(os.path.join(out, "plate.png"), SAMPLES["plate"])
    controls.set_valves(1.0)
    render(os.path.join(out, "warm.png"), SAMPLES["plate"])

    # Switch and lamp passes leave the print out, so that no sprite can carry a copy of it.
    for obj in bpy.data.objects:
        if obj.name.startswith(PRINT):
            obj.hide_render = True
    window(CONTROL_WINDOW)
    controls.set_valves(0.0)
    render(os.path.join(out, "bare.png"), SAMPLES["switch"])
    for name in SWITCHES:
        colls[name].hide_render = False
    for value in (0, 1):
        controls.set_state(switches(value))
        controls.set_lamps({})
        controls.set_valves(0.0)
        render(os.path.join(out, f"switch_{value}.png"), SAMPLES["switch"])
    for cid, colour, *_ in L.LAMPS:
        controls.set_lamps({colour: 1.0})
        render(os.path.join(out, f"lamp_{cid}.png"), SAMPLES["lamp"])
    controls.set_lamps({})

    for obj in objects("Static", "Lamps"):
        obj.visible_camera = False
    for value in (0, 1):
        controls.set_state(switches(value))
        controls.set_lamps({})
        controls.set_valves(0.0)
        render(os.path.join(out, f"mask_{value}.png"), 64, alpha=True)


def knobs(out):
    reset()
    folder = os.path.join(out, "knobs")
    os.makedirs(folder, exist_ok=True)
    controls.set_state(switches(0))
    controls.set_lamps({})
    controls.set_valves(0.0)
    for obj in objects("Static", "Lamps", *SWITCHES):
        obj.visible_camera = False
    window(KNOB_WINDOW)
    bpy.context.scene.render.use_persistent_data = True
    frames = L.ATLASES["knob"][2]
    scene = max(os.path.getmtime(f) for f in glob.glob(os.path.join(HERE, "*.py")) + glob.glob(os.path.join(HERE, "parts", "*.py"))
                if os.path.basename(f) in SCENE_SOURCES or "parts" in f)
    for i in range(frames):
        path = os.path.join(folder, f"{i:02d}.png")
        if os.path.exists(path) and os.path.getmtime(path) > max(scene, RUN["since"]):
            continue                    # resumable: a capped run that stopped keeps its frames
        for key, *_ in L.KNOBS:
            obj = bpy.data.objects[key]
            obj["turn"] = i / (frames - 1)
            obj.update_tag()
        bpy.context.view_layer.update()
        render(path, SAMPLES["knob"], alpha=True)


def refs(out, states):
    for state in states:
        reset()
        controls.set_state(on_frames(STATES[state]))
        render(os.path.join(out, f"ref_{state}.png"), SAMPLES["ref"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--only", default="static,knobs,refs")
    ap.add_argument("--state", nargs="*", default=list(STATES))
    ap.add_argument("--since", type=float, default=0.0, help="re-render passes older than this time (seconds since epoch)")
    ap.add_argument("--budget", type=float, default=float("inf"), help="seconds after which no new render starts (exit 3)")
    args = ap.parse_args(sys.argv[sys.argv.index("--") + 1:])
    RUN.update(since=args.since, budget=args.budget)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    wanted = args.only.split(",")
    if "static" in wanted:
        static(out)
    if "knobs" in wanted:
        knobs(out)
    if "refs" in wanted:
        refs(out, args.state)
    print("MORE" if RUN["more"] else "DONE", flush=True)
    sys.exit(3 if RUN["more"] else 0)


if __name__ == "__main__":
    main()
