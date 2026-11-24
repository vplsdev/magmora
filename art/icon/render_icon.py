"""The app icon's Blender scene: the head's badge made into the icon tile. Runs inside Blender.

  blender -b --factory-startup -P art/icon/render_icon.py -- --out badge.png [--size 2048] [--samples 384]

art/icon/make_icon.py runs this and turns the render into the shipped files. The tile is a squircle of
oxide-red enamel in a rolled nickel bezel with a thin cream line, the way the badge on the head is made
(art/parts/grille.py badge()). On it sits the badge's own M (art/glyphs.py) as a raised cream stroke, and
the badge's two screws. The render is RGBA, transparent round the tile, and its front face spans the icon
grid's tile exactly.
"""
import argparse
import math
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.dirname(HERE)
REPO = os.path.dirname(ART)
sys.path[:0] = [ART, HERE]

import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Matrix  # noqa: E402

import deps  # noqa: E402
import glyphs  # noqa: E402
import lighting  # noqa: E402
import materials  # noqa: E402
import meshes as M  # noqa: E402
from icon_grid import CANVAS, TILE, squircle_outline  # noqa: E402
from parts import common  # noqa: E402

P = 76.0                       # tile width, mm: the M is then about the size of a large badge's
FRAME = P * CANVAS / TILE      # what the camera sees across, mm
LENS, SENSOR = 85.0, 36.0
ELEVATION_DEG = 9.0            # a little above, as the head's camera is


def squircle_loft(name, w, h, stations, mats, coll, segment_material, cap_material):
    """Loft of squircles along local Z, capped flat at the last one. stations = (inset, z), base to top."""
    loops = [[(x, y, z) for x, y in squircle_outline(w - 2 * inset, h - 2 * inset, steps=480)]
             for inset, z in stations]
    n = len(loops[0])
    verts = [p for loop in loops for p in loop]
    quads = M.grid_quads(len(loops), n, closed=True).tolist()
    fm = np.repeat(segment_material, n).tolist()
    centre = len(verts)
    verts.append((0, 0, stations[-1][1]))
    first = (len(loops) - 1) * n
    for j in range(n):
        quads.append((first + j, first + (j + 1) % n, centre, centre))
        fm.append(cap_material)
    obj = M.mesh(name, verts, quads, mats, coll, True, face_material=fm)
    obj.data.set_sharp_from_angle(angle=math.radians(40))
    return obj


def facing(obj):
    """Local +Z towards the camera, centred on the tile's front plane."""
    obj.matrix_world = M.UP_IS_FRONT.copy()
    return obj


def enamel():
    """Oxide-red stove enamel: deep, satin, a faint orange peel. No mirror sheen."""
    n = materials.Nodes("icon_enamel")
    p = n.position()
    tone = n.remap(n.noise(p, 0.12, 3), 0.3, 0.7, 0, 1)
    n.shade(Base_Color=n.mix(tone, (0.085, 0.010, 0.006), (0.100, 0.013, 0.007)),
            Roughness=n.remap(n.noise(p, 0.3, 4, 0.6), 0.3, 0.7, 0.30, 0.40),
            Coat_Weight=0.3, Coat_Roughness=0.16, Normal=n.bump(n.noise(p, 1.4, 2), 0.012, 0.35))
    return n.mat


def badge(b):
    m = b.mats
    # nickel bezel rolled over, a cream line, the enamel field a touch below it
    stations = [(0, -5), (0, -1.0), (0.3, -0.25), (0.8, 0.1), (1.4, 0.18), (1.9, 0.05), (2.15, -0.1),
                (2.2, 0.02), (2.3, 0.12), (3.0, 0.12), (3.1, 0.04), (3.2, 0.0)]
    nickel = materials.simple("icon_nickel", (0.40, 0.38, 0.35), 0.34, 1.0)
    facing(squircle_loft("badge", P, P, stations, [m["cream"], enamel(), nickel], b.static,
                         [2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0], 1))

    strokes, width = glyphs.word("M")
    size = P * 0.44
    for i, stroke in enumerate(strokes):
        pts = [((px - width / 2) * size, (py - 0.5) * size) for px, py in stroke]
        facing(M.stroke_ribbon(f"m_stroke{i}", pts, glyphs.T * size, 0.9, m["cream"], b.static))
    for k, side in enumerate((-1, 1)):
        common.screw(b, f"screw{k}", (side * (P / 2 - 8.2), 0.0, 0.0), 2.1, 1.1, 0.4 + k * 1.1, "dark_nickel")


def camera():
    """Square on to the tile (so it maps linearly to the image), raised and shifted back down."""
    dist = FRAME * LENS / SENSOR
    data = bpy.data.cameras.new("Camera")
    data.lens, data.sensor_width, data.sensor_fit = LENS, SENSOR, "HORIZONTAL"
    rise = dist * math.tan(math.radians(ELEVATION_DEG))
    data.shift_y = -rise / FRAME
    data.clip_start, data.clip_end = 0.01, 5
    obj = bpy.data.objects.new("Camera", data)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = (0, -dist * M.MM, rise * M.MM)
    obj.rotation_euler = (math.pi / 2, 0, 0)
    bpy.context.scene.camera = obj


def lights(coll):
    """The head's studio layout (art/lighting.py), scaled down to a tile a few centimetres across."""
    s = 0.12
    area = lighting.area
    area(coll, "key", (-0.62 * s, -1.05 * s, 0.82 * s), (0, 0, 0), (0.85 * s, 0.6 * s), 18 * s * s * 1.6,
         (1.0, 0.93, 0.84))
    area(coll, "fill", (1.1 * s, -1.0 * s, 0.15 * s), (0, 0, 0), (0.45 * s, 0.9 * s), 4.6 * s * s * 1.4,
         (0.76, 0.86, 1.0))
    area(coll, "top", (0.05 * s, -0.4 * s, 0.95 * s), (0, 0, 0), (1.5 * s, 0.12 * s), 4 * s * s * 1.5,
         (1.0, 0.97, 0.93))
    area(coll, "card", (-0.3 * s, -1.4 * s, -0.55 * s), (0, 0, -0.01), (1.5 * s, 0.3 * s), 3.3 * s * s * 0.7,
         (1.0, 0.92, 0.82))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--samples", type=int, default=384)
    args = ap.parse_args(argv)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    M.forget()
    coll = M.collection("Icon")
    fonts_dir = os.path.join(REPO, "plugin", "resources", "fonts")
    fonts = dict(medium=os.path.join(fonts_dir, "BarlowCondensed-Medium.ttf"),
                 semibold=os.path.join(fonts_dir, "BarlowCondensed-SemiBold.ttf"))
    b = common.Build(materials.library(), fonts, coll, os.path.join(deps.build_dir(), "cache"))
    badge(b)
    camera()
    lights(M.collection("Rig"))
    lighting.world()
    bpy.context.scene.world.node_tree.nodes["Background"].inputs[1].default_value = 0.02
    lighting.render_settings(1, args.samples)
    s = bpy.context.scene
    s.render.resolution_x = s.render.resolution_y = args.size
    s.render.film_transparent = True
    s.render.image_settings.color_mode = "RGBA"
    s.render.image_settings.color_depth = "16"
    s.render.filepath = os.path.abspath(args.out)
    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
