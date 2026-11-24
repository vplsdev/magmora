"""Blender helpers shared by the section scenes."""
import math

import bpy
import numpy as np
from mathutils import Matrix, Vector

import meshes as M

MM = 0.001


def frame(view, x, y, depth=0.0, sheared=False):
    """Matrix (in mm-to-m world units) whose local x, y run along the screen and local z points at the camera."""
    r, u, f = (Vector(v) for v in (view.right, view.up, view.forward))
    rot = Matrix((r, u, -f)).transposed().to_4x4()
    m = Matrix.Translation(Vector(view.world(x, y, depth)) * MM) @ rot
    if sheared:
        sx, sy = view.shear(x, y)
        s = Matrix.Identity(4)
        s[0][2], s[1][2] = sx, sy
        m = m @ s
    return m


def put(obj, matrix):
    obj.matrix_world = matrix
    return obj


def text(b, view, body, cap_pt, x, y, material="ink", font="semibold", align="CENTER", tracking=1.1, depth=-0.06):
    """Printed type on the control plane: cap height in points, anchored at (x, y)."""
    obj = M.text("legend_" + body, body, cap_pt / view.ppm, (0, 0, 0), b.mats[material], b.static,
                 b.fonts[font], align, tracking)
    obj.matrix_world = frame(view, x, y, depth)
    obj["width_pt"] = obj["width_mm"] * view.ppm
    return obj


def word(b, view, body, cap_pt, x, y, material="ink", depth=-0.06, height_mm=0.3):
    """Our own badge capitals (art/glyphs.py) as raised strokes, centred on (x, y)."""
    import glyphs
    strokes, width = glyphs.word(body)
    size = cap_pt / view.ppm
    objs = []
    for i, stroke in enumerate(strokes):
        pts = [((px - width / 2) * size, (py - 0.5) * size) for px, py in stroke]
        o = M.stroke_ribbon(f"word_{body}_{i}", pts, glyphs.T * size, height_mm, b.mats[material], b.static)
        o.matrix_world = frame(view, x, y, depth)
        objs.append(o)
    return width * size * view.ppm


def strip(b, view, name, x0, y0, x1, y1, material="ink", depth=-0.06, coll=None):
    """A flat rectangle on the plane, in screen points."""
    p = [view.world(x0, y1, depth), view.world(x1, y1, depth), view.world(x1, y0, depth), view.world(x0, y0, depth)]
    return M.mesh(name, p, [(0, 1, 2, 3)], b.mats[material], coll or b.static, smooth=False)


def ring(b, view, name, x, y, r0_pt, r1_pt, material="ink", depth=-0.06, a0=0.0, a1=360.0, steps=96):
    th = np.radians(np.linspace(a0, a1, steps))
    pts = []
    for r in (r0_pt, r1_pt):
        pts.append([view.world(x + r * math.sin(t), y - r * math.cos(t), depth) for t in th])
    return M.grid(name, np.array(pts), b.mats[material], b.static, smooth=False)


def ticks(b, view, name, x, y, r0_pt, r1_pt, angles_deg, half_w_pt=0.8, material="ink", depth=-0.06):
    verts, quads = [], []
    for a in angles_deg:
        t = math.radians(a)
        d = (math.sin(t), -math.cos(t))
        n = (math.cos(t), math.sin(t))
        for r, s in ((r0_pt, -1), (r1_pt, -1), (r1_pt, 1), (r0_pt, 1)):
            verts.append(view.world(x + d[0] * r + s * n[0] * half_w_pt, y + d[1] * r + s * n[1] * half_w_pt, depth))
        quads.append(tuple(range(len(verts) - 4, len(verts))))
    return M.mesh(name, verts, quads, b.mats[material], b.static, smooth=False)


def knob(b, coll, name, diameter, mats, teeth=48, height=17.0, skirt=False):
    """A machined knob centred on its local origin, local z toward the camera. Returns (body, pointer)."""
    r = diameter / 2
    segs = teeth * 8
    shoulder, top = r - 1.4, r - 2.6
    prof = [(r - 1.0, 0.3), (r - 0.3, 0.45), (r, 0.9), (r, height * 0.48), (r - 0.25, height * 0.52),
            (shoulder, height * 0.56), (shoulder - 0.1, height * 0.6), (top + 0.35, height * 0.93),
            (top, height * 0.96)]
    kinds = [0, 0, 2, 0, 0, 0, 0, 1]
    dome = height * 0.06
    rim = [(top - 0.25, height + 0.0)]
    rim += [(rr, height + dome * (1 - (rr / top) ** 2)) for rr in np.linspace(top - 0.7, 0.02, 16)]
    prof += rim
    kinds += [1] + [3] * (len(rim) - 1)

    def knurl(rad, z, th):
        tooth = np.abs(((th * teeth / (2 * math.pi)) % 1.0) - 0.5) * 2
        band = np.clip((z - 0.8) / 0.5, 0, 1) * np.clip((height * 0.48 - z) / 0.4, 0, 1)
        return rad - 0.5 * band * np.clip((tooth - 0.18) / 0.82, 0, 1)

    body = M.lathe(name, prof, [mats["body"], mats["cut"], mats["crown"], mats["top"]], coll, segs, knurl,
                   segment_material=kinds, sharp_deg=24)
    path = [(rr, height + dome * (1 - (rr / top) ** 2)) for rr in np.linspace(top * 0.25, top - 0.7, 6)]
    path += [(top - 0.2, height + 0.03), (top + 0.35, height * 0.93), (shoulder - 0.1, height * 0.6)]
    rows = []
    half = max(diameter * 0.03, 0.5)
    for rr, z in path:
        xs = np.linspace(-half, half, 5)
        rows.append(np.stack((xs, np.sqrt(np.maximum((rr + 0.08) ** 2 - xs ** 2, 0)), np.full(5, z + 0.08)), -1))
    pointer = M.grid(name + "_pointer", np.array(rows), mats["pointer"], coll)
    pointer.parent = body
    return body, pointer


def drive_turn(obj, travel_deg=300.0):
    obj["turn"] = 0.5
    obj.id_properties_ui("turn").update(min=0.0, max=1.0)
    d = obj.driver_add("rotation_euler", 2).driver
    v = d.variables.new()
    v.name = "turn"
    v.targets[0].id = obj
    v.targets[0].data_path = '["turn"]'
    d.expression = f"(0.5 - turn) * {math.radians(travel_deg)!r}"


def mount(coll, name, view, x, y, depth=0.0):
    """An empty at (x, y). The shear that cancels perspective there is kept on it, for attach() to apply: Blender
    cannot hold a shear in an object's own matrix (it is decomposed into location, rotation and scale)."""
    e = bpy.data.objects.new(name + "_mount", None)
    e.empty_display_size = 0.01
    coll.objects.link(e)
    e.matrix_world = frame(view, x, y, depth)
    e["shear"] = view.shear(x, y)
    return e


def attach(obj, parent):
    """Parent with the mount's shear as the parent-inverse matrix, which does keep it."""
    shear = Matrix.Identity(4)
    if "shear" in parent:
        shear[0][2], shear[1][2] = parent["shear"]
    obj.parent = parent
    obj.matrix_parent_inverse = shear
    obj.matrix_basis = Matrix.Identity(4)
    return obj


def bpy_empty(coll, name, xyz_mm):
    e = bpy.data.objects.new(name, None)
    e.empty_display_size = 0.02
    coll.objects.link(e)
    e.location = Vector(xyz_mm) * MM
    return e
