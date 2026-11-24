"""The moving parts. One object per control, named by its parameter key."""
import math

import bpy
import numpy as np
from mathutils import Matrix

import layout as L
import meshes as M
from parts.common import screen_mm as mm

KNURL_TEETH = 60
BUTTON_TRAVEL = 5.8
ROCKER_THROW = 19.0
SPILL = dict(green=(0.05, 1, 0.15), red=(1, 0.02, 0.01), amber=(1, 0.25, 0.01), power=(1, 0.02, 0.01))


def mount(coll, name, xyz):
    """Frame of a control, sheared so one shared sprite matches every position along the row."""
    empty = bpy.data.objects.new(name + "_mount", None)
    empty.empty_display_size = 0.01
    coll.objects.link(empty)
    M.place(empty, xyz, M.UP_IS_FRONT)
    shear = Matrix.Identity(4)
    shear[0][2] = -xyz[0] / L.CAMERA_DIST
    empty["shear"] = shear[0][2]
    return empty, shear


def attach(obj, empty, shear):
    obj.parent = empty
    obj.matrix_parent_inverse = shear
    obj.matrix_basis = Matrix.Identity(4)


def knob_parts(b, coll):
    """Knurled base, coned grip, bright-cut chamfer, domed cap. A cut line runs over all of it."""
    m = b.mats
    r = mm(L.KNOB_D) / 2
    segs = KNURL_TEETH * 8
    shoulder, neck, top = r - 1.9, r - 3.1, r - 4.0
    profile = [(r - 1.3, 0.35), (r - 0.4, 0.5), (r, 1.0), (r, 1.8), (r, 7.0), (r, 7.7), (r - 0.2, 8.1),
               (r - 1.2, 8.7), (shoulder, 9.0), (shoulder - 0.1, 9.5), (neck, 14.8), (neck - 0.1, 15.3),
               (top, 16.2)]
    kinds = [0, 0, 0, 2, 0, 0, 2, 0, 0, 0, 0, 1]
    dome = 1.5
    rim = [(top - 0.3, 16.32)]
    rim += [(rr, 16.32 + dome * (1 - (rr / top) ** 2)) for rr in np.linspace(top - 0.8, 0.02, 20)]
    profile += rim
    kinds += [1] + [3] * (len(rim) - 1)

    def knurl(rad, z, th):
        tooth = np.abs(((th * KNURL_TEETH / (2 * math.pi)) % 1.0) - 0.5) * 2
        band = np.clip((z - 0.9) / 0.5, 0, 1) * np.clip((8.5 - z) / 0.4, 0, 1)
        return rad - 0.6 * band * np.clip((tooth - 0.18) / 0.82, 0, 1)

    body = M.lathe("knob_body", profile, [m["knob"], m["bright_cut"], m["knob_worn"], m["knob_top"]], coll,
                   segs, knurl, segment_material=kinds, sharp_deg=24)

    path = [(rr, 16.32 + dome * (1 - (rr / top) ** 2)) for rr in np.linspace(3.0, top - 0.8, 8)]
    path += [(top - 0.3, 16.36), (top, 16.2), (neck - 0.1, 15.32), (neck + 0.01, 14.8),
             (shoulder - 0.09, 9.5), (shoulder, 9.0), (r - 1.2, 8.72)]

    def line(name, half, lift, material):
        rows = []
        for rr, z in path:
            xs = np.linspace(-half, half, 5)
            rows.append(np.stack((xs, np.sqrt(np.maximum((rr + lift) ** 2 - xs ** 2, 0)),
                                  np.full(5, z + lift)), -1))
        return M.grid(name, np.array(rows), material, coll)

    return body, (line("cut", 0.86, 0.04, m["groove"]), line("pointer", 0.66, 0.075, m["pointer"]))


def knobs(b, coll):
    body, marks = knob_parts(b, coll)
    for key, x, _ in L.KNOBS:
        empty, shear = mount(coll, key, L.screen_to_world(x, L.CY))
        obj = body.copy()
        obj.name = key
        coll.objects.link(obj)
        attach(obj, empty, shear)
        obj["turn"] = 0.5
        obj.id_properties_ui("turn").update(min=0.0, max=1.0)
        driver = obj.driver_add("rotation_euler", 2).driver
        var = driver.variables.new()
        var.name = "turn"
        var.targets[0].id = obj
        var.targets[0].data_path = '["turn"]'
        driver.expression = f"(0.5 - turn) * {math.radians(L.KNOB_TRAVEL_DEG)!r}"
        for mark in marks:
            child = mark.copy()
            child.name = f"{key}_{mark.name}"
            coll.objects.link(child)
            child.parent = obj
            child.matrix_parent_inverse = Matrix.Identity(4)
            child.matrix_basis = Matrix.Identity(4)
    for obj in (body, *marks):
        bpy.data.objects.remove(obj)


def buttons(b, coll):
    """Latching switches with a mechanical flag: a dark window out, a cream one in."""
    m = b.mats
    size = mm(L.BUTTON_SIZE)
    stations = [(0, -7.5), (0, 5.2), (0.3, 5.8), (0.75, 6.15), (1.3, 6.25), (1.8, 6.22), (2.6, 6.07),
                (4.2, 5.95), (6.0, 5.92)]
    for key, x, y, _, _ in L.BUTTONS:
        empty, shear = mount(coll, key, L.screen_to_world(x, y))
        cap = M.rounded_plate(key, size, size, 2.0, stations, m["button_cap"], coll)
        attach(cap, empty, shear)
        cap["pressed"] = False
        for suffix, material, r in (("_flag_out", "recess", 1.3), ("_flag_in", "flag", 2.75)):
            profile = [(r, 5.98), (r - 0.15, 6.04), (0.02, 6.04)]
            flag = M.lathe(key + suffix, profile, m[material], coll, 48)
            flag.parent = cap


def toggle(b, coll):
    key, x, y = L.TOGGLE
    m = b.mats
    empty, shear = mount(coll, key, L.screen_to_world(x, y))
    hexagon = lambda r, z, th: np.minimum(r, 5.9 / np.cos((th + math.pi / 6) % (math.pi / 3) - math.pi / 6))
    nut = [(6.8, 0.7), (6.8, 2.5), (6.2, 3.1), (4.2, 3.1), (4.2, 4.9), (3.7, 5.3), (1.5, 5.3)]
    attach(M.lathe(key + "_nut", nut, m["nickel"], coll, 144, hexagon, sharp_deg=20), empty, shear)
    bat = [(2.2, 3.2), (2.0, 5.5), (2.0, 7.5), (2.5, 11.0), (3.2, 15.0), (3.4, 17.0), (3.2, 18.4),
           (2.1, 19.4), (0.05, 19.8)]
    lever = M.lathe(key, bat, m["nickel"], coll, 64)
    attach(lever, empty, shear)
    lever["down"] = False
    for obj in (lever, *[o for o in coll.objects if o.name.startswith(key)]):
        obj.visible_glossy = False


def rockers(b, coll):
    m = b.mats
    w, h = (mm(v) for v in L.ROCKER_SIZE)
    half_w, half_h = w / 2 - 0.2, h / 2 - 0.2

    def surface(v):
        v = np.asarray(v, float)
        edge = np.clip((np.abs(v) - 0.9) / 0.1, 0, 1)
        return 3.0 + 0.9 * np.sqrt(v * v + 0.012) - 0.9 * (1 - np.sqrt(1 - edge * edge))

    v = np.linspace(-1, 1, 41)
    y, z = v * half_h, surface(v)
    xs = np.array((-half_w, -half_w, -half_w + 0.25, -half_w + 0.85,
                   half_w - 0.85, half_w - 0.25, half_w, half_w))
    drop = np.array((0, 0.8, 0.2, 0, 0, 0.2, 0.8, 0))
    p = np.stack(np.broadcast_arrays(xs[None, :], y[:, None], z[:, None] - drop), -1).copy()
    p[:, (0, 7), 2] = -1.5
    p = np.concatenate((p[:1] * (1, 1, 0) + (0, 0, -1.5), p, p[-1:] * (1, 1, 0) + (0, 0, -1.5)))

    for key, sx, sy, _ in L.ROCKERS:
        empty, shear = mount(coll, key, L.screen_to_world(sx, sy))
        body = M.grid(key, p, m["rocker"], coll)
        body.data.set_sharp_from_angle(angle=math.radians(55))
        attach(body, empty, shear)
        body["on"] = False
        centre = half_h * 0.55
        u, w_ = np.meshgrid(np.linspace(-0.55, 0.55, 3), np.linspace(centre - 2.7, centre + 2.7, 12))
        bar = M.grid(key + "_on_mark", np.stack((u, w_, surface(w_ / half_h) + 0.07), -1), m["ink"], coll)
        angle = np.linspace(0, 2 * math.pi, 64, endpoint=False)
        radius = np.linspace(2.6, 1.75, 4)[:, None]
        u, w_ = radius * np.cos(angle), radius * np.sin(angle) - centre
        ring = M.grid(key + "_off_mark", np.stack((u, w_, surface(w_ / half_h) + 0.07), -1), m["ink"], coll,
                      closed=True)
        bar.parent = ring.parent = body


def jewel(name, material, coll, sides=12):
    """Cut lens: rings of facets, each turned half a step from the last."""
    rings = ((5.2, 1.6), (5.2, 2.6), (4.5, 4.0), (3.1, 5.2), (1.5, 5.9))
    verts = []
    for k, (r, z) in enumerate(rings):
        a = (np.arange(sides) + 0.5 * max(k - 1, 0)) * 2 * math.pi / sides
        verts += list(zip(r * np.cos(a), r * np.sin(a), np.full(sides, z)))
    faces = M.grid_quads(2, sides, closed=True).tolist()
    for k in range(1, len(rings) - 1):
        for j in range(sides):
            a, b, c, d = (k * sides + j, k * sides + (j + 1) % sides,
                          (k + 1) * sides + (j + 1) % sides, (k + 1) * sides + j)
            faces += [(a, b, d, d), (b, c, d, d)]
    last = (len(rings) - 1) * sides
    verts.append((0, 0, 6.2))
    faces += [(last + j, last + (j + 1) % sides, len(verts) - 1, len(verts) - 1) for j in range(sides)]
    return M.mesh(name, verts, faces, material, coll, smooth=False)


def lamps(b, coll):
    m = b.mats
    for cid, colour, x, y, _, _ in L.LAMPS:
        xyz = L.screen_to_world(x, y)
        if colour in ("green", "red"):
            bezel = [(3.7, 0), (3.7, 1.1), (3.35, 1.6), (2.7, 1.6), (2.5, 1.1)]
            lens = [(2.55, 0.9), (2.55, 1.8), (2.3, 2.6), (1.7, 3.2), (0.9, 3.55), (0.02, 3.65)]
            M.place(M.lathe(cid + "_bezel", bezel, m["satin_plastic"], b.static), xyz, M.UP_IS_FRONT)
            obj = M.lathe(cid, lens, m[colour], coll, 64)
        else:
            bezel = [(7.3, 0), (7.3, 1.2), (6.8, 2.2), (5.7, 2.5), (5.2, 1.9)]
            M.place(M.lathe(cid + "_bezel", bezel, m["nickel"], b.static), xyz, M.UP_IS_FRONT)
            obj = jewel(cid, m[colour], coll)
        M.place(obj, xyz, M.UP_IS_FRONT)
        small = colour in ("green", "red")
        data = bpy.data.lights.new(cid + "_spill", "POINT")
        data.color = SPILL[colour]
        data.shadow_soft_size = 0.002 if small else 0.004
        data.use_shadow = False
        data["lamp"], data["power"] = colour, 0.00022 if small else 0.0007
        light = bpy.data.objects.new(cid + "_spill", data)
        light.visible_glossy = False
        coll.objects.link(light)
        M.place(light, (xyz[0], -6.5 if small else -10.0, xyz[2]))


def set_state(values):
    """values: parameter key to 0..1 for knobs, 0 or 1 for switches."""
    objs = bpy.data.objects
    for key, *_ in L.KNOBS:
        obj = objs[key]
        obj["turn"] = float(values.get(key, 0.5))
        obj.update_tag()
    for key, *_ in L.BUTTONS:
        down = bool(values.get(key, 0))
        objs[key]["pressed"] = down
        objs[key].location.z = -BUTTON_TRAVEL * M.MM if down else 0.0
        objs[key + "_flag_in"].hide_render = not down
        objs[key + "_flag_out"].hide_render = down
    key = L.TOGGLE[0]
    down = bool(values.get(key, 0))
    objs[key]["down"] = down
    objs[key].rotation_euler.x = math.radians(25 if down else -25)
    for key, *_ in L.ROCKERS:
        on = bool(values.get(key, 0))
        objs[key]["on"] = on
        objs[key].rotation_euler.x = math.radians(ROCKER_THROW if on else -ROCKER_THROW)
    warm = float(values.get("power", 0))
    lit = {colour: float(int(values.get(source, 0)) == when) * warm
           for _, colour, _, _, source, when in L.LAMPS}
    set_lamps(lit)
    set_valves(warm)
    bpy.context.view_layer.update()


def set_lamps(lit):
    """lit: glow colour to 0..1. Lamps not named are dark."""
    for colour in L.GLOWS:
        bpy.data.materials["lamp_" + colour].node_tree.nodes["lit"].outputs[0].default_value = lit.get(colour, 0.0)
    for light in bpy.data.lights:
        if "lamp" in light:
            light.energy = light["power"] * lit.get(light["lamp"], 0.0)


def set_valves(warm):
    for name in ("heater", "plate_glow"):
        bpy.data.materials[name].node_tree.nodes["lit"].outputs[0].default_value = warm
    for light in bpy.data.lights:
        if "power" in light and "lamp" not in light:
            light.energy = light["power"] * warm


def build(b, colls):
    knobs(b, colls["Knob"])
    buttons(b, colls["Button"])
    toggle(b, colls["Toggle"])
    rockers(b, colls["Rocker"])
    lamps(b, colls["Lamps"])
