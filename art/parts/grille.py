"""Slotted steel grille, the valves and iron behind it, and the badge."""
import math

import bpy
import numpy as np
from mathutils import Matrix, Vector

import glyphs
import layout as L
import meshes as M
import product
from parts.common import screw

PLANE = -4.2
SHEET = 1.6
HOLE_D, PITCH_X, PITCH_Z = 4.55, 6.15, 5.4        # punched rounds, staggered


def opening():
    x0, _, z1 = L.screen_to_world(L.GRILLE[0], L.GRILLE[1], PLANE)
    x1, _, z0 = L.screen_to_world(L.GRILLE[2], L.GRILLE[3], PLANE)
    return x0, x1, z0, z1


def _cell():
    """One brick of sheet round one punched hole, as rings with matching point counts."""
    n = 36
    angle = np.linspace(0, 2 * math.pi, n, endpoint=False)
    ca, sa = np.cos(angle), np.sin(angle)
    half_x, half_z = PITCH_X / 2, PITCH_Z / 2
    t = np.minimum(half_x / np.maximum(np.abs(ca), 1e-9), half_z / np.maximum(np.abs(sa), 1e-9))
    bx, bz = t * ca, t * sa
    for sx in (-1, 1):              # the brick's corners have to be vertices
        for sz in (-1, 1):
            k = int(np.argmin((bx - sx * half_x) ** 2 + (bz - sz * half_z) ** 2))
            bx[k], bz[k] = sx * half_x, sz * half_z
    r = HOLE_D / 2
    stations = ((0, SHEET), (0, 0.45), (0.06, 0.2), (0.18, 0.05), (0.36, 0.0))
    rings = [np.column_stack(((r + grow) * ca, (r + grow) * sa, np.full(n, d))) for grow, d in stations]
    rings.append(np.column_stack((bx, bz, np.zeros(n))))
    return np.array(rings)


def sheet(b):
    x0, x1, z0, z1 = opening()
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    cell = _cell()
    verts = []
    cols, rows = int((x1 - x0) / 2 / PITCH_X) + 2, int((z1 - z0) / 2 / PITCH_Z) + 2
    for row in range(-rows, rows + 1):
        for col in range(-cols, cols + 1):
            u, v = (col + 0.5 * (row % 2)) * PITCH_X, row * PITCH_Z
            x = np.clip(cx + cell[..., 0] + u, x0 - 5, x1 + 5)
            z = np.clip(cz + cell[..., 1] + v, z0 + 1.2, z1 + 5)
            across = np.clip((x - cx) / ((x1 - x0) / 2), -1, 1)
            up = np.clip((z - cz) / ((z1 - z0) / 2), -1, 1)
            y = PLANE + cell[..., 2] - 1.6 * (1 - across ** 4) * (1 - up ** 2)      # slight press
            verts.append(np.stack((x, y, z), -1).reshape(-1, 3))
    base = M.grid_quads(cell.shape[0], cell.shape[1], closed=True)
    quads = np.concatenate([base + i * len(verts[0]) for i in range(len(verts))])
    v = np.concatenate(verts)
    border = np.minimum.reduce((v[:, 0] - x0, x1 - v[:, 0], (v[:, 2] - z0) * 0.8, (z1 - v[:, 2]) * 2.0))
    dust = np.clip(1 - border / 26, 0, 1) ** 1.6
    obj = M.mesh("grille", v, quads, b.mats["grille"], b.static, attrs={"dust": dust})
    obj.data.set_sharp_from_angle(angle=math.radians(62))


def frame(b):
    x0, x1, z0, z1 = opening()
    w = 5.2
    y = PLANE - 1.6
    bar = b.mats["bar"]
    M.box("grille_bar_top", (0, y, z1 - w / 2), (x1 - x0, 2.2, w), bar, b.static, 0.7)
    M.box("grille_bar_bottom", (0, y, z0 + w / 2), (x1 - x0, 2.2, w), bar, b.static, 0.7)
    for k, x in enumerate((x0 + w / 2, x1 - w / 2)):
        M.box(f"grille_bar_side_{k}", (x, y, (z0 + z1) / 2), (w, 2.2, z1 - z0 - 2 * w + 0.4), bar, b.static, 0.7)
    for i, x in enumerate(np.linspace(x0 + 9, x1 - 9, 5)):
        for j, z in enumerate((z0 + w / 2, z1 - w / 2)):
            screw(b, f"grille_screw_{i}_{j}", (x, y - 1.1, z), 2.35, 1.2, i * 1.1 + j * 0.7)


def _point_light(b, name, xyz, power, radius, colour=(1.0, 0.33, 0.08)):
    data = bpy.data.lights.new(name, "POINT")
    data.color = colour
    data.energy = power
    data.shadow_soft_size = radius * M.MM
    data["power"] = power
    obj = bpy.data.objects.new(name, data)
    b.static.objects.link(obj)
    obj.visible_glossy = False        # light the parts, do not sparkle on the glass
    M.place(obj, xyz)


def power_tube(b, name, x, y, deck, level, turn, rise):
    """`level`, `turn` and `rise` keep the four from being copies of one another."""
    m = b.mats
    base = M.lathe(name + "_base", [(19, 0), (20, 1), (20, 5), (17.6, 5.5), (17.6, 17), (16.4, 18), (4, 18)],
                   m["satin_plastic"], b.static)
    M.place(base, (x, y, deck))
    shape = [(16.2, 17), (17, 19), (19.2, 26), (19.8, 34), (19.8, 74 + rise), (19.2, 81 + rise),
             (17.6, 86 + rise), (14, 90.5 + rise), (8.5, 93.5 + rise), (0.2, 94.6 + rise)]
    M.place(M.lathe(name + "_glass", shape, m["glass"], b.static), (x, y, deck))
    dome = [(r - 0.35, z - rise) for r, z in shape if z >= 74 + rise]
    M.place(M.lathe(name + "_getter", dome, m["getter"], b.static), (x, y, deck + rise))
    spin = Matrix.Rotation(turn, 4, "Z")

    def part(suffix, offset, size, material, bevel=0.0):
        centre = spin @ Vector(offset) + Vector((x, y, deck))
        obj = M.box(name + suffix, centre, size, material, b.static, bevel)
        M.place(obj, centre, spin)

    part("_plate", (0, 0, 52 + rise / 2), (17, 11, 38 + rise), m["plate"], 1.5)
    for side in (-1, 1):
        part(f"_fin{side}", (side * 11.3, 0, 52 + rise / 2), (6.5, 1.2, 36 + rise), m["plate"])
        part(f"_rod{side}", (side * 5.5, 0, 51), (1.0, 1.0, 62 + rise), m["plate"])
    for z in (30.5, 73.5 + rise):
        mica = M.lathe(name + "_mica", [(14, 0), (14, 0.8), (0.2, 0.8)], m["mica"], b.static, 24)
        M.place(mica, (x, y, deck + z))
    part("_slot", (0, -5.7, 49 + rise), (1.3, 0.4, 12 + 22 * (level - 0.5)), m["plate_glow"])
    part("_heater_low", (0, -2, 28), (1.9, 1.6, 5.0), m["heater"], 0.5)
    part("_heater_top", (1.2, -1, 76 + rise), (1.3, 1.3, 2 + 3 * level), m["heater"], 0.5)
    _point_light(b, name + "_light_top", (x + 3, y - 13, deck + 78 + rise), 0.013 * level, 7)
    _point_light(b, name + "_spill", (x + 6, 156, deck + 52), 0.062 * level, 22, (1.0, 0.31, 0.06))
    _point_light(b, name + "_light_low", (x - 2, y - 14, deck + 27), 0.007 * level, 7)
    _point_light(b, name + "_pool", (x, y + 28, deck + 9), 0.028 * level, 26, (1.0, 0.36, 0.10))


def preamp_tube(b, name, x, y, deck, shielded):
    m = b.mats
    M.place(M.lathe(name + "_socket", [(12, 0), (12, 6), (3, 6)], m["satin_plastic"], b.static), (x, y, deck))
    if shielded:
        shape = [(11.4, 6), (11.4, 44), (11.8, 45), (11.8, 49), (10.4, 50), (6, 50), (6, 48)]
        M.place(M.lathe(name + "_can", shape, m["can"], b.static), (x, y, deck))
        return
    shape = [(10.3, 6), (10.5, 10), (10.5, 44), (9, 49), (5, 52.5), (2, 54), (0.2, 56)]
    M.place(M.lathe(name + "_glass", shape, m["small_glass"], b.static), (x, y, deck))
    dome = [(r - 0.3, z) for r, z in shape if z >= 44]
    M.place(M.lathe(name + "_getter", dome, m["getter"], b.static), (x, y, deck + 38))
    for side in (-1, 1):
        M.box(name + f"_plate{side}", (x + side * 4, y, deck + 27), (5.6, 5, 22), m["plate"], b.static, 0.8)
    M.box(name + "_heater", (x + 3, y - 1, deck + 14.5), (1.3, 1.3, 3.2), m["heater"], b.static, 0.4)
    _point_light(b, name + "_light", (x, y - 10, deck + 15), 0.0035, 5)


def transformer(b, name, x, y, deck, w, h, d):
    m = b.mats
    M.box(name + "_core", (x, y, deck + h / 2), (w, d * 0.42, h), m["laminations"], b.static, 1.0)
    for side in (-1, 1):
        M.box(name + f"_bell{side}", (x, y + side * d * 0.33, deck + h * 0.5),
              (w * 0.78, d * 0.3, h * 0.8), m["laminations"], b.static, 9, 6)
        for j, sign in enumerate((-1, 1)):
            screw(b, name + f"_bolt{side}{j}", (x + side * (w / 2 - 7), y - d * 0.21 - 0.2,
                                               deck + h / 2 + sign * (h / 2 - 7)), 3.4, 2.5, 0.4, "nickel")


def internals(b):
    x0, x1, z0, z1 = opening()
    m = b.mats
    deck = z0 + 2.0
    M.box("chassis", (0, 100, deck - 5), (x1 - x0 + 6, 180, 10), m["chassis"], b.static, 1.0)
    # kept inside the covered box on every side
    M.box("interior_back", (0, 190, (z0 + z1) / 2), (x1 - x0 + 14, 2, z1 - z0 + 10), m["interior"], b.static)
    M.box("interior_top", (0, 98, z1 + 5), (x1 - x0 + 6, 186, 1), m["interior"], b.static)
    transformer(b, "power_transformer", x0 + 72, 70, deck, 96, 90, 82)
    transformer(b, "choke", x0 + 152, 92, deck, 46, 50, 50)
    transformer(b, "output_transformer", x1 - 78, 74, deck, 82, 74, 76)
    tubes = ((-64, 114, 1.0, 0.25, 0), (-13, 127, 0.55, -0.85, -4), (46, 117, 0.85, 1.15, 1), (101, 131, 0.68, -0.2, -5))
    for i, (x, y, level, turn, rise) in enumerate(tubes):
        power_tube(b, f"power_tube{i}", x, y, deck, level, turn, rise)
    for i, x in enumerate((-140, -107, 156, 187, 216)):
        preamp_tube(b, f"preamp_tube{i}", x, 72 + 7 * (i % 2), deck, shielded=i in (1, 3, 4))
    # light that finds its way in past the valves and edges the iron
    _point_light(b, "iron_left", (x0 + 150, 18, deck + 62), 0.0060, 18, (1.0, 0.55, 0.3))
    # a cool strip high in the cabinet, seen only in the glass, so the envelopes have a shape
    data = bpy.data.lights.new("glass_strip", "AREA")
    data.shape, data.size, data.size_y = "RECTANGLE", 0.32, 0.012
    data.energy, data.color = 1.1, (0.92, 0.94, 1.0)
    strip = bpy.data.objects.new("glass_strip", data)
    b.static.objects.link(strip)
    M.place(strip, (-20, 30, deck + 112), Matrix.Rotation(math.radians(-35), 4, "X"))
    strip.visible_camera = False
    receivers = bpy.data.collections.new("GlassReceivers")
    for obj in b.static.objects:
        # only the big envelopes: on the small domes it made hard arcs through the holes
        if obj.name.startswith("power_tube") and obj.name.endswith(("_glass", "_getter")):
            receivers.objects.link(obj)
    strip.light_linking.receiver_collection = receivers
    _point_light(b, "iron_right", (x1 - 150, 30, deck + 88), 0.006, 16, (1.0, 0.55, 0.3))
    for i, x in enumerate((-186, -164)):
        can = M.lathe(f"filter_cap{i}", [(10.5, 0), (10.5, 52), (9.5, 53.5), (0.2, 53.5)], m["can"], b.static)
        M.place(can, (x, 124, deck))


def badge(b):
    x0, y0, x1, y1 = L.BADGE
    left, y, top = L.screen_to_world(x0, y0, PLANE - 2.4)
    right, _, bottom = L.screen_to_world(x1, y1, PLANE - 2.4)
    w, h = right - left, top - bottom
    cx, cz = (left + right) / 2, (top + bottom) / 2
    m = b.mats
    stations = [(0, -1.6), (0, 1.3), (0.15, 1.7), (0.45, 1.9), (1.25, 1.9), (1.45, 1.7), (1.55, 1.5)]
    plate = M.rounded_plate("badge", w, h, 2.8, stations, [m["cream"], m["enamel"], m["nickel"]],
                            b.static, segment_material=[2, 2, 2, 0, 0, 0], cap_material=1)
    M.place(plate, (cx, y, cz), M.UP_IS_FRONT)

    strokes, width = glyphs.word(product.BADGE_TEXT)
    size = min(h * 0.47, (w - 22) / width)
    for i, stroke in enumerate(strokes):
        pts = [((px - width / 2) * size, (py - 0.5) * size) for px, py in stroke]
        obj = M.stroke_ribbon(f"badge_stroke{i}", pts, glyphs.T * size, 0.45 + i * 0.004, m["cream"], b.static)
        M.place(obj, (cx, y - 1.5, cz), M.UP_IS_FRONT)
    for k, side in enumerate((-1, 1)):
        screw(b, f"badge_screw{k}", (cx + side * (w / 2 - 5.6), y - 1.5, cz), 1.9, 1.1, 0.4 + k, "nickel")


def build(b):
    sheet(b)
    frame(b)
    internals(b)
    badge(b)
