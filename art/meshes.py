"""Geometry helpers. All inputs are millimetres; the scene is in metres."""
import math

import bpy
import numpy as np
from mathutils import Matrix

MM = 0.001
UP_IS_FRONT = Matrix.Rotation(math.pi / 2, 4, "X")      # local +Z points at the camera


def collection(name, parent=None):
    coll = bpy.data.collections.new(name)
    (parent or bpy.context.scene.collection).children.link(coll)
    return coll


def mesh(name, verts, quads, material, coll, smooth=True, attrs=None, face_material=None):
    """Mesh from arrays. A quad whose last two indices repeat is written as a triangle."""
    verts = np.asarray(verts, np.float32).reshape(-1, 3) * MM
    quads = np.asarray(quads, np.int32).reshape(-1, 4)
    tri = quads[:, 2] == quads[:, 3]
    sizes = np.where(tri, 3, 4).astype(np.int32)
    keep = np.ones(quads.shape, bool)
    keep[tri, 3] = False
    loops = quads[keep]
    starts = np.concatenate(([0], np.cumsum(sizes)[:-1])).astype(np.int32)

    me = bpy.data.meshes.new(name)
    me.vertices.add(len(verts))
    me.vertices.foreach_set("co", verts.ravel())
    me.loops.add(len(loops))
    me.loops.foreach_set("vertex_index", loops.astype(np.int32))
    me.polygons.add(len(quads))
    me.polygons.foreach_set("loop_start", starts)
    materials = material if isinstance(material, (list, tuple)) else [material]
    for mat in materials:
        me.materials.append(mat)
    me.polygons.foreach_set("use_smooth", np.full(len(quads), bool(smooth)))
    if face_material is not None:
        me.polygons.foreach_set("material_index", np.asarray(face_material, np.int32))
    for key, values in (attrs or {}).items():
        layer = me.attributes.new(key, "FLOAT", "POINT")
        layer.data.foreach_set("value", np.asarray(values, np.float32).ravel())
    me.update(calc_edges=True)
    obj = bpy.data.objects.new(name, me)
    coll.objects.link(obj)
    return obj


def grid_quads(rows, cols, closed=False):
    """Quads of a rows x cols point grid; the normal is d/dcol x d/drow."""
    i = np.arange(rows - 1)[:, None]
    j = np.arange(cols if closed else cols - 1)[None, :]
    k = (j + 1) % cols
    a, b, c, d = i * cols + j, i * cols + k, (i + 1) * cols + k, (i + 1) * cols + j
    return np.stack(np.broadcast_arrays(a, b, c, d), -1).reshape(-1, 4)


def grid(name, points, material, coll, closed=False, smooth=True, attrs=None, flip=False,
         face_material=None):
    rows, cols = points.shape[:2]
    quads = grid_quads(rows, cols, closed)
    if flip:
        quads = quads[:, ::-1]
    return mesh(name, points, quads, material, coll, smooth, attrs, face_material)


def lathe(name, profile, material, coll, segments=96, radius_fn=None, smooth=True,
          segment_material=None, sharp_deg=38):
    """Revolve (radius, z) about local Z. Profiles run from the base to the top."""
    profile = np.asarray(profile, float)
    angle = np.linspace(0, 2 * math.pi, segments, endpoint=False)
    r = np.repeat(profile[:, :1], segments, 1)
    z = np.repeat(profile[:, 1:], segments, 1)
    if radius_fn is not None:
        r = radius_fn(r, z, angle[None, :])
    points = np.stack((r * np.cos(angle), r * np.sin(angle), z), -1)
    fm = np.repeat(segment_material, segments) if segment_material is not None else None
    obj = grid(name, points, material, coll, closed=True, smooth=smooth, face_material=fm)
    if smooth and sharp_deg:
        obj.data.set_sharp_from_angle(angle=math.radians(sharp_deg))
    return obj


def place(obj, xyz, matrix=None):
    obj.matrix_world = Matrix.Translation([v * MM for v in xyz]) @ (matrix or Matrix.Identity(4))
    return obj


def box(name, centre, size, material, coll, bevel=0.0, segments=4):
    sx, sy, sz = (v / 2 for v in size)
    verts = [(x, y, z) for x in (-sx, sx) for y in (-sy, sy) for z in (-sz, sz)]
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    obj = mesh(name, verts, quads, material, coll, smooth=False)
    if bevel:
        mod = obj.modifiers.new("bevel", "BEVEL")
        mod.width = bevel * MM
        mod.segments = segments
        mod.limit_method = "NONE"
        for poly in obj.data.polygons:
            poly.use_smooth = True
        obj.modifiers.new("normals", "WEIGHTED_NORMAL").keep_sharp = True
    return place(obj, centre)


def rounded_rect(w, h, r, steps=8):
    """Counter-clockwise outline centred on the origin."""
    cx, cy = w / 2 - r, h / 2 - r
    pts = []
    for qx, qy, start in ((cx, cy, 0), (-cx, cy, 90), (-cx, -cy, 180), (cx, -cy, 270)):
        for i in range(steps + 1):
            a = math.radians(start + 90 * i / steps)
            pts.append((qx + r * math.cos(a), qy + r * math.sin(a)))
    return pts


def rounded_plate(name, w, h, r, stations, material, coll, steps=8, cap=True, smooth=True,
                  segment_material=None, cap_material=0):
    """Loft of rounded rectangles along local Z. stations = (inset, z) from base to top."""
    loops = []
    for inset, z in stations:
        outline = rounded_rect(w - 2 * inset, h - 2 * inset, max(r - inset, 0.02), steps)
        loops.append([(x, y, z) for x, y in outline])
    n = len(loops[0])
    verts = [p for loop in loops for p in loop]
    quads = grid_quads(len(loops), n, closed=True).tolist()
    fm = (np.repeat(segment_material, n).tolist() if segment_material is not None
          else [0] * len(quads))
    if cap:
        centre = len(verts)
        verts.append((0, 0, stations[-1][1]))
        first = (len(loops) - 1) * n
        for j in range(n):
            quads.append((first + j, first + (j + 1) % n, centre, centre))
            fm.append(cap_material)
    obj = mesh(name, verts, quads, material, coll, smooth, face_material=fm)
    if smooth:
        obj.data.set_sharp_from_angle(angle=math.radians(40))
    return obj


def tube(name, points, radius, material, coll, sides=24):
    """Round tube along a polyline, with a parallel-transport frame."""
    p = np.asarray(points, float)
    t = np.gradient(p, axis=0)
    t /= np.linalg.norm(t, axis=1, keepdims=True)
    seed = np.array((0.0, 0.0, 1.0)) if abs(t[0][2]) < 0.9 else np.array((1.0, 0.0, 0.0))
    a = np.cross(t[0], seed)
    angle = np.linspace(0, 2 * math.pi, sides, endpoint=False)
    radius = np.broadcast_to(radius, (len(p),))
    rings = []
    for centre, tangent, r in zip(p, t, radius):
        a = a - tangent * a.dot(tangent)
        a /= np.linalg.norm(a)
        b = np.cross(tangent, a)
        rings.append(centre + r * (np.cos(angle)[:, None] * a + np.sin(angle)[:, None] * b))
    return grid(name, np.array(rings), material, coll, closed=True)


def spline(points, steps=24):
    """Catmull-Rom through the points."""
    p = np.asarray(points, float)
    p = np.vstack((2 * p[0] - p[1], p, 2 * p[-1] - p[-2]))
    out = []
    for i in range(1, len(p) - 2):
        for s in np.linspace(0, 1, steps, endpoint=False):
            out.append(0.5 * (2 * p[i] + (p[i + 1] - p[i - 1]) * s
                              + (2 * p[i - 1] - 5 * p[i] + 4 * p[i + 1] - p[i + 2]) * s * s
                              + (3 * p[i] - p[i - 1] - 3 * p[i + 1] + p[i + 2]) * s ** 3))
    out.append(p[-2])
    return np.array(out)


def stroke_ribbon(name, path, width, height, material, coll):
    """A flat-topped stroke along a 2D path with mitred joins, standing on local z = 0."""
    p = [np.asarray(path[0], float)]
    for q in path[1:]:
        if np.linalg.norm(np.asarray(q) - p[-1]) > 1e-4:
            p.append(np.asarray(q, float))
    closed = len(p) > 2 and np.linalg.norm(p[0] - p[-1]) < 1e-4
    if closed:
        p.pop()
    n = len(p)

    def normal(i, j):
        d = p[j] - p[i]
        d /= np.linalg.norm(d)
        return np.array((-d[1], d[0]))

    verts = []
    for i in range(n):
        before = normal((i - 1) % n, i) if closed or i else normal(i, i + 1)
        after = normal(i, (i + 1) % n) if closed or i < n - 1 else before
        mid = before + after
        mid /= np.linalg.norm(mid)
        offset = mid * width / 2 / max(mid.dot(after), 0.4)
        bevel = offset * 0.86
        verts += [(*(p[i] - offset), 0), (*(p[i] - offset), height * 0.6), (*(p[i] - bevel), height),
                  (*(p[i] + bevel), height), (*(p[i] + offset), height * 0.6), (*(p[i] + offset), 0)]
    if closed:
        verts += verts[:6]
        n += 1
    quads = grid_quads(n, 6).tolist()
    if not closed:
        last = 6 * (n - 1)
        quads += [(0, 1, 2, 3), (0, 3, 4, 5), (last + 3, last + 2, last + 1, last),
                  (last + 5, last + 4, last + 3, last)]
    return mesh(name, verts, quads, material, coll, smooth=False)


_fonts = {}


def forget():
    """Handles die with the file they were loaded into; call when starting a scene from nothing."""
    _fonts.clear()
    _caps.clear()


def text(name, body, height_mm, xyz, material, coll, font, align="CENTER", tracking=1.1):
    """Printed type: cap-height `height_mm`, centred on xyz, facing the camera."""
    if font not in _fonts:
        _fonts[font] = bpy.data.fonts.load(font, check_existing=True)
    curve = bpy.data.curves.new(name, "FONT")
    curve.body = body
    curve.font = _fonts[font]
    curve.size = 1.0
    curve.space_character = tracking
    curve.resolution_u = 8
    curve.fill_mode = "FRONT"
    tmp = bpy.data.objects.new(name + "_curve", curve)
    coll.objects.link(tmp)
    bpy.context.view_layer.update()
    me = bpy.data.meshes.new_from_object(tmp)
    bpy.data.objects.remove(tmp)

    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    ref = cap_height(font)
    low, high = co[:, 0].min(), co[:, 0].max()
    anchor = {"CENTER": (low + high) / 2, "LEFT": low, "RIGHT": high}[align]
    co[:, 0] -= anchor
    co[:, 1] -= ref / 2
    co *= height_mm / ref * MM
    me.vertices.foreach_set("co", co.ravel())
    me.materials.clear()
    me.materials.append(material)
    me.update()
    obj = bpy.data.objects.new(name, me)
    coll.objects.link(obj)
    place(obj, xyz, UP_IS_FRONT)
    obj["width_mm"] = float((high - low) / ref * height_mm)
    return obj


_caps = {}


def cap_height(font):
    if font not in _caps:
        curve = bpy.data.curves.new("cap", "FONT")
        curve.body, curve.font, curve.size = "H", _fonts[font], 1.0
        tmp = bpy.data.objects.new("cap", curve)
        bpy.context.scene.collection.objects.link(tmp)
        bpy.context.view_layer.update()
        me = bpy.data.meshes.new_from_object(tmp)
        _caps[font] = max(v.co.y for v in me.vertices)
        bpy.data.objects.remove(tmp)
        bpy.data.meshes.remove(me)
    return _caps[font]
