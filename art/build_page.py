"""Build one section scene (pedal, eq, cab) from nothing, as build_scene.py does for the head.

  blender -b --factory-startup -P art/build_page.py -- --scene pedal [--check] [--render out.png --scale 2
          --samples 128] [--state default|off] [--out scene.blend]

The positions of every control come from the scene module (art/pages/<scene>.py), which also writes
its part of plugin/resources/img/pages.json, so the render and the UI cannot drift apart.
"""
import argparse
import importlib
import os
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import bpy  # noqa: E402
from mathutils import Matrix, Vector  # noqa: E402

import lighting  # noqa: E402
import materials  # noqa: E402
import meshes as M  # noqa: E402
from pages import looks  # noqa: E402
from parts import common  # noqa: E402

SCENES = ("pedal", "eq", "cab")
COLLECTIONS = ("Static", "Knob", "Button", "Toggle", "Lamps", "Mic")


def build_dir():
    private = os.environ.get("BLOCKAMP_PRIVATE")
    return os.path.join(private, "build", "art") if private else os.path.join(HERE, "build")


def camera(view, rig):
    data = bpy.data.cameras.new("Camera")
    data.lens, data.sensor_width, data.sensor_fit = lighting.L.LENS_MM, lighting.L.SENSOR_MM, "HORIZONTAL"
    data.shift_y = -view.elevation / view.view_w
    data.clip_start, data.clip_end = 0.05, 30
    data.dof.use_dof = True
    data.dof.focus_distance = view.dist * M.MM
    data.dof.aperture_fstop = 8.0
    obj = bpy.data.objects.new("Camera", data)
    rig.objects.link(obj)
    r, u, f = (Vector(v) for v in (view.right, view.up, view.forward))
    obj.matrix_world = Matrix.Translation(Vector(view.camera_position()) * M.MM) @ Matrix((r, u, -f)).transposed().to_4x4()
    bpy.context.scene.camera = obj


def build(name, quick=False):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    M.forget()
    colls = {c: M.collection(c) for c in COLLECTIONS}
    rig = M.collection("Rig")
    fonts_dir = os.path.join(REPO, "plugin", "resources", "fonts")
    fonts = dict(medium=os.path.join(fonts_dir, "BarlowCondensed-Medium.ttf"),
                 semibold=os.path.join(fonts_dir, "BarlowCondensed-SemiBold.ttf"))
    mats = looks.add(materials.library())
    b = common.Build(mats, fonts, colls["Static"], os.path.join(build_dir(), "cache"), quick)
    scene = importlib.import_module("pages." + name)
    scene.build(b, colls)
    camera(scene.VIEW, rig)
    bpy.context.scene.camera.data.dof.use_dof = getattr(scene, "DOF", True)
    scene.lights(rig)
    lighting.world()
    return scene


def set_state(scene, state):
    """state: 'default' (on, knobs as stored), 'off' (block off)."""
    values = dict(getattr(scene, "DEFAULTS", {}))
    values.update(getattr(scene, "STATES", {}).get(state, {}))
    dark = state == "dark"
    for obj in bpy.data.objects:
        if "turn" in obj:
            obj["turn"] = float(values.get(obj.name, 0.5))
            obj.update_tag()
    lit = {} if dark else getattr(scene, "lit", lambda v: {})(values)
    for colour in ("green", "red", "amber", "power"):
        mat = bpy.data.materials.get("lamp_" + colour)
        if mat:
            mat.node_tree.nodes["lit"].outputs[0].default_value = float(lit.get(colour, 0.0))
    if hasattr(scene, "apply"):
        scene.apply(values)
    bpy.context.view_layer.update()


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", choices=SCENES, required=True)
    ap.add_argument("--render")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--samples", type=int, default=128)
    ap.add_argument("--state", default="default")
    ap.add_argument("--out")
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args(argv)
    started = time.time()
    scene = build(args.scene, args.quick)
    print(f"scene built in {time.time() - started:.1f} s", flush=True)
    set_state(scene, args.state)
    lighting.render_settings(args.scale, args.samples)
    if args.render:
        bpy.context.scene.render.filepath = os.path.abspath(args.render)
        bpy.ops.render.render(write_still=True)
        print(f"rendered after {time.time() - started:.1f} s", flush=True)
    if args.out:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(args.out), compress=True)


if __name__ == "__main__":
    main()
