"""Faceplate: metal, trim, legends, rules, jack, lamp bezels, fixed bezels of the switches."""
import math

import numpy as np

import layout as L
import meshes as M
from parts.common import screen_mm as mm, screw

INK_Y = -0.06
CAP_NAME = 10.6         # cap heights, points
CAP_SECTION = 9.4
RULE_PT = 1.3
RULE_DROP = 6.0
CAP_NUMERAL = 8.9


def at(x, y, depth=0.0):
    return L.screen_to_world(x, y, depth)


def strip(b, name, x0, y0, x1, y1, material="ink", depth=INK_Y):
    ax, ay, az = at(x0, y0, depth)
    bx, _, bz = at(x1, y1, depth)
    return M.mesh(name, [(ax, ay, bz), (bx, ay, bz), (bx, ay, az), (ax, ay, az)], [(0, 1, 2, 3)],
                  b.mats[material], b.static, smooth=False)


def label(b, body, x, y, cap, font="semibold", align="CENTER", tracking=1.1, material="ink"):
    return M.text("legend_" + body, body, mm(cap), at(x, y, INK_Y), b.mats[material], b.static,
                  b.fonts[font], align, tracking)


def plate(b):
    m = b.mats
    x0, _, z1 = at(L.PANEL[0] - 6, L.PANEL[1] - 2, 4.5)
    x1, _, z0 = at(L.PANEL[2] + 6, L.PANEL[3] + 4, 4.5)
    M.box("panel_recess", (0, 6.0, (z0 + z1) / 2), (x1 - x0, 3.0, z1 - z0), m["recess"], b.static)

    x0, _, z1 = at(L.PLATE[0], L.PLATE[1])
    x1, _, z0 = at(L.PLATE[2], L.PLATE[3])
    stations = [(0, -3.4), (0, -1.5), (0.35, -0.6), (1.5, 0.0)]
    obj = M.rounded_plate("panel", x1 - x0, z1 - z0, 2.6, stations, [m["panel"], m["panel_edge"]], b.static,
                          smooth=False, segment_material=[1, 1, 1], cap_material=0)
    M.place(obj, ((x0 + x1) / 2, 0, (z0 + z1) / 2), M.UP_IS_FRONT)

    x0, _, z1 = at(L.TRIM[0] - 3, L.TRIM[1], -1.6)
    x1, _, z0 = at(L.TRIM[2] + 3, L.TRIM[3], -1.6)
    M.box("trim", (0, 1.4, (z0 + z1) / 2), (x1 - x0, 6.0, z1 - z0), m["trim"], b.static, 0.8)
    for i, (x, y) in enumerate(((88, 326), (1192, 326), (88, 450), (1192, 450))):
        cup = M.lathe(f"panel_cup{i}", [(4.0, 0), (4.0, 0.3), (3.6, 0.65), (2.7, 0.5), (2.5, 0.2)], m["nickel"], b.static)
        M.place(cup, at(x, y), M.UP_IS_FRONT)
        screw(b, f"panel_screw{i}", at(x, y, -0.25), 2.6, 1.2, 0.5 + i * 1.3, "dark_nickel")


def knob_scale(b, key, x, name):
    r0, r1 = (mm(r) for r in L.TICK_R)
    cx, y, cz = at(x, L.CY, INK_Y)
    verts, quads = [], []
    for i in range(11):
        a = math.radians(L.knob_degrees(i / 10))
        major = i % 5 == 0
        d, n = np.array((math.sin(a), math.cos(a))), np.array((math.cos(a), -math.sin(a)))
        half = mm(1.05 if major else 0.7)
        end = r1 + (mm(0.8) if major else 0)
        corners = (d * r0 - n * half, d * r0 + n * half, d * end + n * half, d * end - n * half)
        quads.append(tuple(range(len(verts), len(verts) + 4)))
        verts += [(cx + px, y, cz + pz) for px, pz in corners]
    M.mesh("scale_" + key, verts, quads, b.mats["ink"], b.static, smooth=False)
    for value, body in ((0.0, "0"), (0.5, "5"), (1.0, "10")):
        a = math.radians(L.knob_degrees(value))
        nx, ny = x + L.NUMERAL_R * math.sin(a), L.CY - L.NUMERAL_R * math.cos(a)
        label(b, body, nx + (2.0 if body == "10" else 0), ny, CAP_NUMERAL)
    label(b, name, x, L.LABEL_CY, CAP_NAME)


def group(b, name, x0, x1):
    """A printed rule with square ends, broken for the section name, in the legends' own type."""
    middle, y = (x0 + x1) / 2, L.GROUP_CY
    obj = label(b, name.upper(), middle, y, CAP_SECTION)
    half = obj["width_mm"] * L.PT_PER_MM / 2 + 5
    weight = RULE_PT / 2
    for k, (a, c) in enumerate(((x0, middle - half), (middle + half, x1))):
        strip(b, f"rule_{name}{k}", a, y - weight, c, y + weight)
        tip = a if k == 0 else c
        strip(b, f"rule_end_{name}{k}", tip - weight, y - RULE_DROP, tip + weight, y + weight)


def channel_leader(b):
    """Printed line from the channel button to the pair of lamps it switches."""
    _, bx, by, _, _ = next(item for item in L.BUTTONS if item[0] == "channel")
    _, _, lx, ly, _, _ = next(item for item in L.LAMPS if item[0] == "lamp_lead")
    turn, half = ly + 12.5, 0.7
    strip(b, "leader_up", lx - half, ly + 7.5, lx + half, turn + half)
    strip(b, "leader_across", lx + half, turn - half, bx + half, turn + half)
    strip(b, "leader_down", bx - half, turn + half, bx + half, by - L.BUTTON_SIZE / 2 - 4.5)


def hex_radius(flats):
    def fn(r, z, th):
        a = (th + math.pi / 6) % (math.pi / 3) - math.pi / 6
        return np.minimum(r, flats / np.cos(a))
    return fn


def jack(b):
    x, y = L.JACK
    xyz = at(x, y, 0)
    m = b.mats
    washer = M.lathe("jack_washer", [(10.4, 0), (10.4, 0.6), (10.0, 0.95), (6, 0.95)], m["nickel"], b.static)
    M.place(washer, xyz, M.UP_IS_FRONT)
    nut = M.lathe("jack_nut", [(9.0, 0.9), (9.0, 3.3), (8.5, 3.8), (7.6, 4.1), (5.4, 4.1)], m["nickel"],
                  b.static, 144, hex_radius(7.8), sharp_deg=20)
    M.place(nut, xyz, M.UP_IS_FRONT)
    sleeve = [(5.6, 1), (5.6, 5.4), (5.2, 5.9), (3.9, 5.9), (3.5, 5.5), (3.4, 1.2)]
    M.place(M.lathe("jack_sleeve", sleeve, m["nickel"], b.static), xyz, M.UP_IS_FRONT)
    M.place(M.lathe("jack_hole", [(3.4, 1.3), (0.1, 1.3)], m["recess"], b.static), xyz, M.UP_IS_FRONT)
    label(b, "INPUT", x, y - 34, 9.6)

    _, tx, ty = L.TOGGLE
    label(b, "HIGH", tx, ty - L.TOGGLE_LABEL_D, 9.6)
    label(b, "NORMAL", tx, ty + L.TOGGLE_LABEL_D, 9.6)
    hole = M.lathe("toggle_washer", [(8.2, 0), (8.2, 0.45), (7.8, 0.75), (3, 0.75)], m["nickel"], b.static)
    M.place(hole, at(tx, ty), M.UP_IS_FRONT)


def button_bezels(b):
    size = mm(L.BUTTON_SIZE)
    stations = [(0, 0), (0, 3.4), (0.4, 4.0), (0.8, 4.2), (1.7, 4.2), (2.05, 3.9), (2.1, 0.1)]
    for key, x, y, name, label_y in L.BUTTONS:
        ring = M.rounded_plate("bezel_" + key, size + 4.8, size + 4.8, 2.6, stations, b.mats["satin_plastic"],
                               b.static, cap_material=0)
        M.place(ring, at(x, y), M.UP_IS_FRONT)
        label(b, name, x, label_y, CAP_NAME if key == "channel" else 9.6)


def rocker_bezels(b):
    w, h = (mm(v) for v in L.ROCKER_SIZE)
    stations = [(0, 0), (0, 2.3), (0.45, 3.3), (1.2, 3.85), (2.9, 3.85), (3.6, 3.2), (3.9, 2.4), (4.0, 0.2)]
    for key, x, y, name in L.ROCKERS:
        ring = M.rounded_plate("bezel_" + key, w + 8.0, h + 8.0, 3.2, stations,
                               [b.mats["bezel_steel"], b.mats["recess"]], b.static,
                               segment_material=[0, 0, 0, 0, 0, 0, 1], cap_material=1)
        M.place(ring, at(x, y), M.UP_IS_FRONT)
        label(b, name, x, L.LABEL_CY + 7, CAP_NAME)


def build(b):
    plate(b)
    for key, x, name in L.KNOBS:
        knob_scale(b, key, x, name)
    for body, x, y in L.LAMP_NAMES:
        label(b, body, x, y, 9.3, align="LEFT", tracking=1.02)
    channel_leader(b)
    for args in L.GROUPS:
        group(b, *args)
    jack(b)
    button_bezels(b)
    rocker_bezels(b)
