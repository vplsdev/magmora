"""Build the whole Magmora scene from nothing.

  blender -b --factory-startup -P art/build_scene.py -- [--check] [--quick]
          [--out scene.blend] [--render out.png --scale 2 --samples 256 --state hero]

Also writes art/layout.json and plugin/resources/img/layout.json from layout.py.
"""
import argparse
import os
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import bpy  # noqa: E402

import layout as L  # noqa: E402
import lighting  # noqa: E402
import materials  # noqa: E402
import meshes as M  # noqa: E402
from states import STATES  # noqa: E402
from parts import cabinet, common, controls, grille, panel, surroundings  # noqa: E402

COLLECTIONS = ("Static", "Knob", "Button", "Toggle", "Rocker", "Lamps")
def build_dir():
    private = os.environ.get("BLOCKAMP_PRIVATE")
    return os.path.join(private, "build", "art") if private else os.path.join(HERE, "build")


def build(quick=False):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    M.forget()
    colls = {name: M.collection(name) for name in COLLECTIONS}
    rig = M.collection("Rig")
    font_dir = os.path.join(REPO, "plugin", "resources", "fonts")
    fonts = dict(medium=os.path.join(font_dir, "BarlowCondensed-Medium.ttf"),
                 semibold=os.path.join(font_dir, "BarlowCondensed-SemiBold.ttf"))
    b = common.Build(materials.library(), fonts, colls["Static"],
                     os.path.join(build_dir(), "cache"), quick)
    tex = cabinet.build(b)
    grille.build(b)
    panel.build(b)
    controls.build(b, colls)
    surroundings.build(b, tex)
    lighting.camera(rig)
    lighting.lights(rig)
    lighting.world()
    view_layers()


def view_layers():
    """One layer per asset type. The default layer keeps everything, for reviews."""
    scene = bpy.context.scene
    scene.view_layers[0].name = "All"
    wanted = dict(Plate=("Static", "Lamps"), Knob=("Knob",), Button=("Button",), Toggle=("Toggle",),
                  Rocker=("Rocker",), Glow=("Static", "Lamps"))
    for name, shown in wanted.items():
        layer = scene.view_layers.new(name)
        layer.use = False
        for child in layer.layer_collection.children:
            child.exclude = child.name not in shown + ("Rig",)


def check():
    sprite_only = ("knob_light",)       # in the contract, but not objects
    wanted = {c["id"] for c in L.manifest()["controls"] if c["type"] not in sprite_only} - {"tubes"}
    problems = ["collection " + c for c in COLLECTIONS if c not in bpy.data.collections]
    problems += ["object " + o for o in sorted(wanted) if o not in bpy.data.objects]
    problems += ["turn on " + k for k, *_ in L.KNOBS
                 if k in bpy.data.objects and "turn" not in bpy.data.objects[k]]
    # the camera must map the panel plane onto the layout
    from bpy_extras.object_utils import world_to_camera_view
    scene = bpy.context.scene
    scene.render.resolution_x, scene.render.resolution_y = L.RENDER_W, L.RENDER_H
    bpy.context.view_layer.update()
    for key, x, _ in L.KNOBS:
        p = world_to_camera_view(scene, scene.camera, bpy.data.objects[key + "_mount"].matrix_world.translation)
        if abs(p.x * L.RENDER_W - x) > 0.05 or abs((1 - p.y) * L.RENDER_H - L.CY) > 0.05:
            problems.append(f"registration of {key}")
    problems += obstructed(scene)
    return problems


def obstructed(scene):
    """The lead may hide neither a legend nor anything clickable."""
    from bpy_extras.object_utils import world_to_camera_view

    def screen(obj):
        points = (world_to_camera_view(scene, scene.camera, obj.matrix_world @ v.co) for v in obj.data.vertices)
        return [(p.x * L.RENDER_W, (1 - p.y) * L.RENDER_H) for p in points]

    boxes = [(c["id"], c["x"] - c["hit"][0] / 2, c["y"] - L.BAR_H - c["hit"][1] / 2,
              c["x"] + c["hit"][0] / 2, c["y"] - L.BAR_H + c["hit"][1] / 2)
             for c in L.manifest()["controls"] if c["hit"][0]]
    for obj in scene.objects:
        if obj.type == "MESH" and obj.name.startswith(("legend_", "scale_", "rule_", "leader_")):
            xs, ys = zip(*screen(obj))
            boxes.append((obj.name, min(xs), min(ys), max(xs), max(ys)))
    found = set()
    for obj in scene.objects:
        if obj.name.startswith(("plug_", "lead")) and not obj.name.startswith("leader_"):
            points = screen(obj)
            found |= {f"{obj.name} covers {name}" for name, x0, y0, x1, y1 in boxes
                      if any(x0 <= x <= x1 and y0 <= y <= y1 for x, y in points)}
    return sorted(found)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--quick", action="store_true", help="coarse covering, for look and layout rounds")
    ap.add_argument("--render")
    ap.add_argument("--scale", type=int, default=L.SCALE)
    ap.add_argument("--samples", type=int, default=256)
    ap.add_argument("--state", choices=STATES, default="hero")
    args = ap.parse_args(argv)

    started = time.time()
    build(quick=args.quick)
    print(f"scene built in {time.time() - started:.1f} s", flush=True)
    img = os.path.join(REPO, "plugin", "resources", "img")
    os.makedirs(img, exist_ok=True)
    L.write_manifest(os.path.join(HERE, "layout.json"), os.path.join(img, "layout.json"))

    if args.check:
        problems = check()
        print("CHECK", "FAILED: " + "; ".join(problems) if problems else "OK")
        if problems:
            sys.exit(1)
    controls.set_state(STATES[args.state])
    lighting.render_settings(args.scale, args.samples)
    if args.render:
        bpy.context.scene.render.filepath = os.path.abspath(args.render)
        bpy.ops.render.render(write_still=True)
        print(f"rendered after {time.time() - started:.1f} s", flush=True)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(args.out), compress=True)


if __name__ == "__main__":
    main()
