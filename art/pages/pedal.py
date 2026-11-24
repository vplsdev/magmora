"""Cinder (working name): an original die-cast overdrive on a stage floor, seen from above the player's foot.

Colour: deep metallic hammertone green, linear RGB about (0.008, 0.030, 0.016) to (0.016, 0.056, 0.031) with
metal flake. It is not the classic pale or mint gloss of the famous green overdrive, and the
trade dress stays ours: a hammered metallic finish, three knobs in a triangle, a round stomp switch with no pad or
bezel, an inset hairline border, the CINDER word mark in our own capitals, cream print, an amber lamp.
"""
import math

import numpy as np
from mathutils import Matrix, Vector

import lighting
import meshes as M
import product
from pages import kit
from pages.view import View

# The pedal's top is the control plane; the camera hangs above it, a little towards the player.
VIEW = View(centre=(0.0, 0.0, 0.0), forward=(0.0, 0.0, -1.0), up=(0.0, 1.0, 0.0), pt_per_mm=3.7, elevation=-190.0)
TITLE = "Pedal"

BOX_W, BOX_H, BOX_D = 76.0, 124.0, 38.0   # mm
FEET = 2.5
CX, CY = 640.0, 284.0                     # screen centre of the top
KNOB_D = 17.5
KNOBS = [("pedal_drive", 568.0, 150.0, "DRIVE"), ("pedal_level", 712.0, 150.0, "LEVEL"),
         ("pedal_tone", 640.0, 232.0, "TONE")]
LED = (640.0, 118.0)
SWITCH = (640.0, 436.0)
NAME_Y, KIND_Y, MAKER_Y = 330.0, 358.0, 503.5   # the maker sits between the border line and the edge

DEFAULTS = dict(pedal_drive=0.62, pedal_tone=0.45, pedal_level=0.55, pedal_on=1)
STATES = {"off": dict(pedal_on=0)}


def lit(values):
    return {"amber": float(values.get("pedal_on", 0))}


CONTROLS = (
    [dict(id=key, kind="knob", x=x, y=y, asset="pedal_knob", label=label, hit=(78, 84)) for key, x, y, label in KNOBS]
    + [dict(id="pedal_on", kind="switch", x=SWITCH[0], y=SWITCH[1], asset="pedal_switch", hit=(84, 84)),
       dict(id="pedal_led", kind="lamp", x=LED[0], y=LED[1], asset="pedal_glow", source="pedal_on", on_value=1)]
)
ASSETS = {
    "pedal_knob": dict(kind="knob", collection="Knob", ref="pedal_tone", cell=(84, 84)),
    "pedal_switch": dict(kind="switch", collection="Button", ref="pedal_on", cell=(64, 64), travel=1.6),
    "pedal_glow": dict(kind="lamp", ref="pedal_led", cell=(64, 64)),
}


def top_point(x, y, depth=0.0):
    return VIEW.world(x, y, depth)


def enclosure(b):
    w, h = BOX_W, BOX_H
    stations = [(0.9, BOX_D), (0.0, BOX_D - 1.2), (0.0, 3.2), (0.35, 1.6), (1.0, 0.65), (1.9, 0.15), (2.8, 0.0)]
    # rounded_plate lofts along local z from base to top; the top is at local z = 0
    obj = M.rounded_plate("cinder_box", w, h, 5.5, [(i, -d) for i, d in stations], b.mats["cinder_paint"],
                          b.static, steps=10)
    me = obj.data
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3) / kit.MM
    edge = np.clip(1 - np.abs(co[:, 2] + 0.6) / 2.2, 0, 1)                      # the top's rounded edge
    corner = np.clip((np.abs(co[:, 0]) - (w / 2 - 9)) / 9, 0, 1) * np.clip((np.abs(co[:, 1]) - (h / 2 - 9)) / 9, 0, 1)
    toe = np.clip((-co[:, 1] - (h / 2 - 16)) / 16, 0, 1)                         # the end the foot hits
    bottom = np.clip(1 - np.abs(co[:, 2] + BOX_D - 1.5) / 2.5, 0, 1)
    wear = np.clip(edge * (0.25 + 0.75 * np.maximum(corner, toe)) + bottom * 0.5, 0, 1)
    layer = me.attributes.new("wear", "FLOAT", "POINT")
    layer.data.foreach_set("value", wear.astype(np.float32))
    obj.data.materials.clear()
    obj.data.materials.append(b.mats["cinder_chip"])
    obj.matrix_world = kit.frame(VIEW, CX, CY)
    # the base plate and four rubber feet
    base = M.rounded_plate("cinder_base", w - 1.0, h - 1.0, 5.0, [(0, -BOX_D - 1.0), (0, -BOX_D + 0.4)],
                           b.mats["casting"], b.static)
    base.matrix_world = kit.frame(VIEW, CX, CY)
    for sx in (-1, 1):
        for sy in (-1, 1):
            foot = M.lathe("foot", [(5.5, -BOX_D - 1.0 - FEET), (6.0, -BOX_D - 1.2), (5.8, -BOX_D - 1.0)],
                           b.mats["rubber"], b.static, 32)
            foot.matrix_world = kit.frame(VIEW, CX, CY) @ Matrix.Translation(
                Vector((sx * (w / 2 - 8), sy * (h / 2 - 8), 0)) * kit.MM)


def print_top(b):
    v = VIEW
    # an inset hairline frame, the pedal's own graphic
    x0, x1 = CX - (BOX_W / 2 - 5.5) * v.ppm, CX + (BOX_W / 2 - 5.5) * v.ppm
    y0, y1 = CY - (BOX_H / 2 - 5.5) * v.ppm, CY + (BOX_H / 2 - 5.5) * v.ppm
    t = 1.6
    for k, (a, c, d, e) in enumerate(((x0, y0, x1, y0 + t), (x0, y1 - t, x1, y1), (x0, y0, x0 + t, y1),
                                      (x1 - t, y0, x1, y1))):
        kit.strip(b, v, f"frame{k}", a, c, d, e)
    for key, x, y, label in KNOBS:
        kit.text(b, v, label, 10.5, x, y + 50.0, tracking=1.12)
        kit.ticks(b, v, "scale_" + key, x, y, 37.0, 42.0, (-150, 150), 1.2)
        kit.ticks(b, v, "scale_mid_" + key, x, y, 37.0, 40.0, (-90, -30, 30, 90), 0.8)
    kit.word(b, v, "CINDER", 26.0, CX, NAME_Y)
    kit.text(b, v, "OVERDRIVE", 9.0, CX, KIND_Y, font="medium", tracking=1.45)
    kit.text(b, v, product.BADGE_TEXT, 8.0, CX, MAKER_Y, font="medium", tracking=1.6)


def knobs(b, colls):
    mats = dict(body=b.mats["knob"], cut=b.mats["bright_cut"], crown=b.mats["knob_worn"], top=b.mats["knob_top"],
                pointer=b.mats["pointer"])
    for key, x, y, _ in KNOBS:
        e = kit.mount(colls["Knob"], key, VIEW, x, y)
        body, _ = kit.knob(b, colls["Knob"], key, KNOB_D, mats, teeth=40, height=15.0)
        kit.attach(body, e)
        kit.drive_turn(body)
        # the pot's nut and washer under the skirt edge
        nut = M.lathe(key + "_washer", [(7.4, 0), (7.4, 0.5), (4.0, 0.5)], b.mats["nickel"], b.static, 48)
        nut.matrix_world = kit.frame(VIEW, x, y)


def led(b, colls):
    x, y = LED
    m = kit.frame(VIEW, x, y)
    bezel = M.lathe("led_bezel", [(4.4, 0), (4.4, 0.8), (4.0, 1.5), (3.1, 1.6), (2.8, 1.1)], b.mats["chrome"], b.static, 64)
    bezel.matrix_world = m
    lens = M.lathe("pedal_led", [(2.85, 0.9), (2.85, 1.9), (2.5, 2.9), (1.8, 3.5), (0.9, 3.85), (0.02, 3.95)],
                   b.mats["amber"], colls["Lamps"], 48)
    lens.matrix_world = m


def footswitch(b, colls):
    x, y = SWITCH
    m = kit.frame(VIEW, x, y)
    from parts.panel import hex_radius
    washer = M.lathe("fs_washer", [(11.0, 0), (11.0, 0.7), (10.6, 1.0), (6.5, 1.0)], b.mats["nickel"], b.static, 96)
    washer.matrix_world = m
    nut = M.lathe("fs_nut", [(9.6, 0.9), (9.6, 4.0), (9.1, 4.6), (7.6, 4.8), (6.4, 4.8)], b.mats["nickel"], b.static, 144,
                  hex_radius(8.3), sharp_deg=20)
    nut.matrix_world = m
    bush = M.lathe("fs_bush", [(6.3, 4.6), (6.3, 8.5), (5.9, 9.0), (5.0, 9.0)], b.mats["nickel"], b.static, 96)
    bush.matrix_world = m
    e = kit.mount(colls["Button"], "pedal_on", VIEW, x, y)
    cap = M.lathe("pedal_on", [(4.9, 6.0), (4.9, 12.6), (4.6, 13.4), (3.6, 13.95), (1.6, 14.2), (0.02, 14.25)],
                  b.mats["chrome"], colls["Button"], 96)
    kit.attach(cap, e)
    cap["pressed"] = False


def plug(b, name, at, direction, length=34.0):
    """A straight jack plug lying along `direction` (world unit vector), its tip at `at` (world mm)."""
    d = Vector(direction).normalized()
    z = d
    x = Vector((0, 0, 1)).cross(z).normalized()
    y = z.cross(x)
    m = Matrix.Translation(Vector(at) * kit.MM) @ Matrix((x, y, z)).transposed().to_4x4()
    sleeve = M.lathe(name + "_sleeve", [(3.2, 0), (3.2, 4.0), (4.6, 4.4), (4.6, 7.0)], b.mats["nickel"], b.static, 48)
    sleeve.matrix_world = m
    body = M.lathe(name + "_body", [(5.6, 6.8), (6.4, 8.0), (6.6, 10), (6.6, length - 8), (6.2, length - 5),
                                    (4.4, length - 2), (3.6, length + 6)], b.mats["satin_plastic"], b.static, 64)
    body.matrix_world = m
    return Vector(at) + d * (length + 6)


def cables(b):
    floor = -(BOX_D + 1.0 + FEET)
    r = 3.0
    jy = VIEW.world(CX, CY - (BOX_H / 2 - 30) * VIEW.ppm)[1]
    # input on the right side, output on the left, as is usual
    for side, name in ((1, "in"), (-1, "out")):
        tip = (side * (BOX_W / 2 + 0.5), jy, -BOX_D * 0.48)
        end = plug(b, "plug_" + name, tip, (side, 0, 0))
        path = [tuple(end), (end[0] + side * 14, end[1] + 2, floor + r + 4), (end[0] + side * 40, end[1] + 18, floor + r),
                (end[0] + side * 85, end[1] + 70, floor + r), (end[0] + side * 120, end[1] + 190, floor + r),
                (end[0] + side * 150, end[1] + 420, floor + r)]
        M.tube("cable_" + name, M.spline(path, 24), r, b.mats["cable"], b.static, 24)
    # power, into the top end
    tip = (6.0, BOX_H / 2 + 0.5, -BOX_D * 0.42)
    end = plug(b, "plug_dc", tip, (0, 1, 0), 26.0)
    path = [tuple(end), (end[0] + 2, end[1] + 16, floor + 2.6), (end[0] - 20, end[1] + 70, floor + 2.2),
            (end[0] - 70, end[1] + 210, floor + 2.2)]
    M.tube("cable_dc", M.spline(path, 24), 2.2, b.mats["cable"], b.static, 20)


def floor(b):
    z = -(BOX_D + 1.0 + FEET)
    M.box("floor", (0, 200, z - 5), (3000, 3000, 10), b.mats["floor"], b.static)


def build(b, colls):
    _swap_chip_to_hammertone(b)
    enclosure(b)
    print_top(b)
    knobs(b, colls)
    led(b, colls)
    footswitch(b, colls)
    cables(b)
    floor(b)


def _swap_chip_to_hammertone(b):
    """The chipped paint uses the hammertone shader for its painted part."""
    from materials import Nodes
    n = Nodes("cinder_box_paint")
    p = n.object_position()
    dimple = n.add("ShaderNodeTexVoronoi", {"Vector": p, "Scale": 1.3}, feature="F1").outputs["Distance"]
    rough = n.noise(p, 0.08, 4, 0.6)
    paint = n.mix(n.remap(rough, 0.3, 0.7, 0, 1), (0.0075, 0.030, 0.016), (0.016, 0.056, 0.031))  # PAINT_GREEN
    wear = n.math("MULTIPLY", n.attribute("wear"), n.remap(n.noise(p, 1.3, 5, 0.75), 0.55, 0.62, 0, 1))
    flakes = n.remap(n.noise(p, 9.0, 2, 0.5), 0.74, 0.78, 0, 1)
    n.shade(Base_Color=n.mix(wear, paint, (0.50, 0.48, 0.45)),
            Metallic=n.math("MAXIMUM", wear, n.math("ADD", 0.35, n.math("MULTIPLY", flakes, 0.5))),
            Roughness=n.remap(rough, 0.3, 0.7, 0.26, 0.40), Coat_Weight=n.math("SUBTRACT", 0.35, n.math("MULTIPLY", wear, 0.35)),
            Coat_Roughness=0.22, Normal=n.bump(n.math("ADD", dimple, n.math("MULTIPLY", rough, 0.3)), 0.22, 0.45))
    b.mats["cinder_chip"] = n.mat


def lights(rig):
    a = lighting.area
    a(rig, "key", (-0.75, 0.35, 0.62), (0.0, 0.0, -0.03), (0.55, 0.4), 30, (1.0, 0.93, 0.84))
    a(rig, "fill", (0.75, -0.45, 0.6), (0.0, 0.0, -0.03), (0.4, 0.8), 5, (0.78, 0.86, 1.0))
    a(rig, "rim", (0.2, 0.9, 0.16), (0.0, 0.0, -0.02), (1.2, 0.15), 16, (1.0, 0.95, 0.9))
    a(rig, "top_soft", (-0.15, -0.25, 1.1), (0.0, 0.0, 0.0), (0.7, 0.5), 7, (1.0, 0.97, 0.93))
