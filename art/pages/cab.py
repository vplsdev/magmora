"""The cabinet page: the top two speakers of an original closed-back 4x12, grille panel off, seen square on. The
left cone carries Mic 1's marker and the right cone Mic 2's (drawn by the plug-in); under each cone, that slot's
strip. The pack folder and the alignment switch sit in a small corner between the two strips."""
import math

import numpy as np
from mathutils import Matrix, Vector

import lighting
import meshes as M
from pages import kit
from pages.view import View

PPM = 1.18
VIEW = View(centre=(0.0, 0.0, 0.0), forward=(0.0, 1.0, 0.0), up=(0.0, 0.0, 1.0), pt_per_mm=PPM, elevation=60.0)
TITLE = "Cab"

SPACING = 365.0                       # speaker centres, mm
CONE_Y = 254.0                        # screen y of both cone centres
CONE_X = (640.0 - SPACING / 2 * PPM, 640.0 + SPACING / 2 * PPM)
CAB_W, CAB_H, CAB_D = 760.0, 760.0, 360.0
SPEAKER_FROM_TOP = 196.0
CONE_R = 136.0                        # mm, the cone's radius: position 1 is its edge

STRIP_DEPTH = -1050.0                 # the strip's panel stands this far in front of the cabinet, mm
STRIP_TOP = 476.0
ROW = 522.0
KNOB_D = 22.0
BUTTON = 9.0


def _slot(n, x0):
    keys = [("position", "POSITION"), ("distance", "DISTANCE"), ("level", "LEVEL"), ("pan", "PAN")]
    knobs = [(f"mic{n}_{k}", x0 + 204.0 + i * 82.0, ROW, t) for i, (k, t) in enumerate(keys)]
    buttons = [(f"mic{n}_on", x0 + 30.0, ROW, "ON"), (f"mic{n}_polarity", x0 + 510.0, ROW, "PHASE")]
    window = (f"slot{n}_window", x0 + 56.0, ROW - 15.0, x0 + 172.0, ROW + 15.0)
    return knobs, buttons, window


SLOT_X = (8.0, 718.0)               # left edge of each slot's strip
KNOBS, BUTTONS, WINDOWS = [], [], []
for _n, _x0 in ((1, SLOT_X[0]), (2, SLOT_X[1])):
    _k, _b, _w = _slot(_n, _x0)
    KNOBS += _k
    BUTTONS += _b
    WINDOWS.append(_w)
CORNER_X = 643.0
CORNER_STEP = 50.0
BUTTONS += [("cab_pack", CORNER_X - CORNER_STEP, ROW, "PACK"), ("mic_align", CORNER_X, ROW, "ALIGN")]
KNOBS += [("mic_offset", CORNER_X + CORNER_STEP, ROW, "OFFSET")]
SMALL = {"mic_offset"}
LATCHING = {"mic1_on", "mic2_on", "mic1_polarity", "mic2_polarity", "mic_align"}
ACTIONS = {"cab_pack"}

DEFAULTS = dict(cab_on=1, mic1_on=1, mic2_on=1, mic1_position=0.3, mic2_position=0.55, mic1_distance=25.0,
                mic2_distance=60.0, mic_align=1)
STATES = {"off": dict(mic1_on=0, mic2_on=0)}


def lit(values):
    return {"amber": 1.0}


def _controls():
    out = []
    for k, x, y, t in KNOBS:
        out.append(dict(id=k, kind="knob", x=x, y=y, asset="cab_knob_small" if k in SMALL else "cab_knob", label=t,
                        hit=(46, 46) if k in SMALL else (60, 64)))
    for k, x, y, t in BUTTONS:
        out.append(dict(id=k, kind="action" if k in ACTIONS else "switch", x=x, y=y, asset="cab_button", hit=(34, 34)))
        if k in LATCHING:
            out.append(dict(id=k + "_led", kind="lamp", x=x + 12.0, y=y - 13.0, asset="cab_glow", source=k, on_value=1))
    for k, x0, y0, x1, y1 in WINDOWS:
        out.append(dict(id=k, kind="window", x=(x0 + x1) / 2, y=(y0 + y1) / 2, w=x1 - x0, h=y1 - y0))
    r = CONE_R * PPM
    for n, cx in ((1, CONE_X[0]), (2, CONE_X[1])):
        out.append(dict(id=f"cone{n}", kind="window", x=cx, y=CONE_Y, w=2 * r, h=2 * r))
    return out


CONTROLS = _controls()
ASSETS = {
    "cab_knob": dict(kind="knob", collection="Knob", ref="mic1_pan", cell=(68, 68)),
    "cab_knob_small": dict(kind="knob", collection="Knob", ref="mic_offset", cell=(44, 44)),
    "cab_button": dict(kind="switch", collection="Button", ref="mic1_polarity", cell=(36, 36), travel=4.0),
    "cab_glow": dict(kind="lamp", ref="mic1_on_led", cell=(24, 24)),
}


# ---------------------------------------------------------------------------------------------------------
def spk_world(i):
    x, _, z = VIEW.world(CONE_X[i], CONE_Y)
    return x, z


def cabinet(b):
    from parts import cabinet as C
    tex = C._grain(b.cache)
    (x1, z1), (x2, _) = spk_world(0), spk_world(1)
    cx = (x1 + x2) / 2
    top = z1 + SPEAKER_FROM_TOP
    box = C.Box("cab4x12", cx - CAB_W / 2, cx + CAB_W / 2, top - CAB_H, top, -6.0, CAB_D, 30.0, 30.0, 0.9)
    C.shell(b, tex, box)
    C.piping(b, box)
    C.corner_caps(b, box)
    baffle = M.box("baffle", (cx, 22.0, top - CAB_H / 2), (CAB_W - 40, 4, CAB_H - 40), b.mats["baffle_cloth"], b.static)
    import bpy
    holes = []
    for i in (0, 1):
        x, z = spk_world(i)
        for dz, tag in ((0.0, ""), (-(CAB_H - 2 * SPEAKER_FROM_TOP), "low")):
            speaker(b, x, z + dz, f"spk{i}{tag}")
            hole = M.lathe("hole", [(141.0, -20.0), (141.0, 20.0)], b.mats["recess"], b.static, 96, smooth=False)
            hole.matrix_world = Matrix.Translation(Vector((x, 22.0, z + dz)) * kit.MM) @ M.UP_IS_FRONT
            holes.append(hole)
    for hole in holes:
        mod = baffle.modifiers.new("hole", "BOOLEAN")
        mod.operation, mod.object, mod.solver = "DIFFERENCE", hole, "EXACT"
        hole.hide_render = True
    # The grille panel is off, but its cloth still wraps the baffle's edge: a band of it round the opening.
    frame_w = 10.0
    bx0, bx1 = cx - CAB_W / 2 + 30.0, cx + CAB_W / 2 - 30.0
    bz0, bz1 = top - CAB_H + 30.0, top - 30.0
    for name, (x, z, w, h) in {"top": ((bx0 + bx1) / 2, bz1 - frame_w / 2, bx1 - bx0, frame_w),
                               "bottom": ((bx0 + bx1) / 2, bz0 + frame_w / 2, bx1 - bx0, frame_w),
                               "left": (bx0 + frame_w / 2, (bz0 + bz1) / 2, frame_w, bz1 - bz0 - 2 * frame_w),
                               "right": (bx1 - frame_w / 2, (bz0 + bz1) / 2, frame_w, bz1 - bz0 - 2 * frame_w)}.items():
        M.box("cloth_edge_" + name, (x, 17.0, z), (w, 6.0, h), b.mats["edge_cloth"], b.static, 1.5)
    # inside the box: dark
    M.box("cab_inside", (cx, 200.0, top - CAB_H / 2), (CAB_W - 40, 4, CAB_H - 40), b.mats["recess"], b.static)
    return box


def speaker(b, x, z, name):
    m = Matrix.Translation(Vector((x, 20.0, z)) * kit.MM) @ M.UP_IS_FRONT
    # a cloth surround pressed in rolls: four corrugations across 12 mm
    corr = [(146.0 - 12.0 * t, 0.4 + 2.6 * math.sin(math.pi * t) + 1.1 * abs(math.sin(math.pi * t * 4)))
            for t in np.linspace(0, 1, 64)]
    # a curvilinear cone with three faint pressed rings
    cone = [(134.0 - (134.0 - 30.0) * t, -6.0 - 86.0 * t ** 1.25 + 0.6 * math.sin(math.pi * t * 6) * (t > 0.1))
            for t in np.linspace(0, 1, 60)]
    parts = [
        ("gasket", [(158, -1), (158, 1.6), (156.5, 2.6), (148, 2.6), (147, 1.2)], "gasket"),
        ("surround", [(147.0, 0.4)] + corr + [(134.0, -6.0)], "surround"),
        ("cone", cone, "cone"),
        ("cap", [(30.0, -92.0)] + [(30 * math.cos(t), -92.0 + 15 * math.sin(t)) for t in np.linspace(0.12, math.pi / 2, 14)],
         "dustcap"),
    ]
    for part, prof, mat in parts:
        obj = M.lathe(f"{name}_{part}", prof, b.mats[mat], b.static, 256)
        obj.matrix_world = m
    for k in range(8):
        a = k * math.pi / 4 + math.pi / 8
        bolt = M.lathe(f"{name}_bolt", [(5.0, -1), (5.0, 2.0), (4.0, 2.9), (0.1, 3.1)], b.mats["nickel"], b.static, 32)
        bolt.matrix_world = Matrix.Translation(Vector((x + 152 * math.cos(a), 20.0, z + 152 * math.sin(a))) * kit.MM) @ M.UP_IS_FRONT


def strip(b):
    """The slots' panel, below the speakers, in front of the cabinet."""
    v, d = VIEW, STRIP_DEPTH
    left, right = v.world(4, 500, d), v.world(1276, 500, d)
    top = v.world(640, STRIP_TOP, d)[2]
    bot = v.world(640, 620, d)[2]
    w, h = right[0] - left[0], top - bot
    cy = v.world(640, STRIP_TOP, d)
    plate = M.rounded_plate("strip_panel", w, h, 2.5, [(0, -40.0), (0, -1.2), (0.5, -0.3), (1.4, 0.0)],
                            b.mats["eq_panel"], b.static, smooth=False)
    plate.matrix_world = Matrix.Translation(Vector((cy[0], cy[1], top - h / 2)) * kit.MM) @ M.UP_IS_FRONT
    for n, x0 in ((1, SLOT_X[0]), (2, SLOT_X[1])):
        kit.text(b, v, f"MIC {n}", 9.4, x0 + 14, STRIP_TOP + 19.0, align="LEFT", tracking=1.14, depth=d - 0.06)
    kit.strip(b, v, "rule", 566.0, STRIP_TOP + 10, 567.0, 572, depth=d - 0.06)
    kit.strip(b, v, "rule", 719.0, STRIP_TOP + 10, 720.0, 572, depth=d - 0.06)
    kit.text(b, v, "CABINET", 8.4, CORNER_X, STRIP_TOP + 19.0, tracking=1.14, depth=d - 0.06)
    for k, x, y, t in KNOBS:
        small = k in SMALL
        corner = abs(x - CORNER_X) < 60
        kit.text(b, v, t, 6.6 if corner else 7.6, x, y + (23.0 if small else 39.0), font="medium", tracking=1.0 if corner else 1.04, depth=d - 0.06)
        kit.ticks(b, v, "scale_" + k, x, y, 19.0 if small else 28.5, 21.5 if small else 32.0,
                  (-150.0, 0.0, 150.0) if k.endswith("pan") else (-150.0, 150.0), 0.7, depth=d - 0.06)
    for k, x, y, t in BUTTONS:
        corner = abs(x - CORNER_X) < 60
        kit.text(b, v, t, 6.6 if corner else 7.6, x, y + 23.0, font="medium", tracking=1.0 if corner else 1.04, depth=d - 0.06)
        bezel = M.rounded_plate(k + "_bezel", BUTTON + 3.6, BUTTON + 3.6, 2.0,
                                [(0, 0), (0, 2.6), (0.4, 3.2), (0.8, 3.4), (1.5, 3.4), (1.8, 3.1), (1.9, 0.1)],
                                b.mats["satin_plastic"], b.static)
        bezel.matrix_world = kit.frame(v, x, y, d)
        if k in LATCHING:
            led = M.lathe(k + "_led", [(1.5, 0.0), (1.5, 0.6), (1.1, 1.0), (0.02, 1.1)], b.mats["amber"], b.static, 32)
            led.matrix_world = kit.frame(v, x + 12.0, y - 13.0, d)
    for k, x0, y0, x1, y1 in WINDOWS:
        a, c = v.world(x0, y0, d), v.world(x1, y1, d)
        win = M.rounded_plate(k, abs(c[0] - a[0]), abs(c[2] - a[2]), 1.2, [(0, -2.0), (0, 0.4)], b.mats["display_glass"], b.static)
        win.matrix_world = kit.frame(v, (x0 + x1) / 2, (y0 + y1) / 2, d)


def controls(b, colls):
    mats = dict(body=b.mats["knob"], cut=b.mats["bright_cut"], crown=b.mats["knob_worn"], top=b.mats["knob_top"],
                pointer=b.mats["pointer"])
    d = STRIP_DEPTH
    for k, x, y, _ in KNOBS:
        e = kit.mount(colls["Knob"], k, VIEW, x, y, d)
        body, _ = kit.knob(b, colls["Knob"], k, 14.0 if k in SMALL else KNOB_D, mats, teeth=40, height=12.0 if k in SMALL else 15.0)
        kit.attach(body, e)
        kit.drive_turn(body)
    for k, x, y, _ in BUTTONS:
        e = kit.mount(colls["Button"], k, VIEW, x, y, d)
        cap = M.rounded_plate(k, BUTTON, BUTTON, 1.5, [(0, -5.0), (0, 4.4), (0.3, 5.0), (0.7, 5.3), (1.3, 5.4),
                                                       (2.4, 5.25), (4.5, 5.18)], b.mats["button_cap"], colls["Button"])
        kit.attach(cap, e)
        cap["pressed"] = False


def room(b):
    M.box("wall", (0, 1600, 0), (6000, 4, 4000), b.mats["wall"], b.static)


def build(b, colls):
    from materials import Nodes, simple
    del Nodes
    b.mats["cone"] = _cone()
    b.mats["dustcap"] = _dustcap()
    b.mats["surround"] = simple("surround", (0.016, 0.0145, 0.013), 0.42, Coat_Weight=0.3, Coat_Roughness=0.35)
    b.mats["gasket"] = simple("gasket", (0.012, 0.012, 0.012), 0.6)
    b.mats["baffle_felt"] = _felt()
    b.mats["baffle_cloth"] = _baffle_cloth()
    b.mats["edge_cloth"] = _edge_cloth()
    cabinet(b)
    strip(b)
    controls(b, colls)
    room(b)


def _felt():
    from materials import Nodes
    n = Nodes("baffle_felt")
    p = n.position()
    fuzz = n.noise(p, 2.5, 6, 0.75)
    grain = n.noise(n.position((1000 * 0.08, 1000, 1000 * 2.5)), 1.0, 4, 0.7)   # the board under the felt
    n.shade(Base_Color=n.mix(n.remap(fuzz, 0.3, 0.7, 0, 1), (0.012, 0.0115, 0.011), (0.026, 0.024, 0.022)),
            Roughness=0.95, Sheen_Weight=1.0, Sheen_Roughness=0.35,
            Normal=n.bump(n.math("ADD", fuzz, n.math("MULTIPLY", grain, 0.6)), 0.5, 0.7))
    return n.mat


def _edge_cloth():
    """Our grille cloth: a fine twill of charcoal and warm bronze-grey threads (no basketweave, no plate)."""
    from materials import Nodes
    n = Nodes("edge_cloth")
    parts = n.add("ShaderNodeSeparateXYZ", {"Vector": n.position()}).outputs
    diag = n.math("ADD", parts["X"], n.math("MULTIPLY", parts["Z"], 0.6))
    twill = n.math("ABSOLUTE", n.math("SINE", n.math("MULTIPLY", diag, 3.1416 / 0.9)))
    fleck = n.remap(n.noise(n.position(), 3.0, 3, 0.6), 0.62, 0.7, 0, 1)
    base = n.mix(n.remap(twill, 0.2, 1.0, 0, 1), (0.010, 0.0095, 0.009), (0.032, 0.028, 0.024))
    n.shade(Base_Color=n.mix(fleck, base, (0.09, 0.07, 0.05)), Roughness=0.85, Sheen_Weight=0.7,
            Normal=n.bump(twill, 0.25, 0.6))
    return n.mat


def _baffle_cloth():
    """Our grille cloth, stretched over the baffle: an open twill of charcoal threads with a warm bronze-grey weft,
    coarse enough to read at this distance. Original: not a basketweave, not a check, no metallic stripe."""
    from materials import Nodes
    n = Nodes("baffle_cloth")
    pos = n.position()
    xyz = n.add("ShaderNodeSeparateXYZ", {"Vector": pos}).outputs
    pitch = 2.4                                     # mm per thread
    u = n.math("DIVIDE", n.math("ADD", xyz["X"], 3000.0), pitch)   # kept positive: FLOOR and MODULO
    w = n.math("DIVIDE", n.math("ADD", xyz["Z"], 3000.0), pitch)
    diag = n.math("FLOOR", n.math("ADD", u, w))     # the twill line steps one thread per row
    warp_up = n.math("MODULO", diag, 3.0)
    over = n.math("LESS_THAN", warp_up, 2.0)        # 2/1 twill: warp over two, under one
    fu = n.math("FRACT", u)
    fw = n.math("FRACT", w)
    warp = n.math("SINE", n.math("MULTIPLY", fu, 3.1416))
    weft = n.math("SINE", n.math("MULTIPLY", fw, 3.1416))
    height = n.mix(over, (0.0, 0.0, 0.0), (1.0, 1.0, 1.0))
    h = n.add("ShaderNodeSeparateColor", {"Color": height}).outputs["Red"]
    surface = n.math("ADD", n.math("MULTIPLY", h, warp), n.math("MULTIPLY", n.math("SUBTRACT", 1.0, h), weft))
    slub = n.remap(n.noise(n.position((300, 1000, 300)), 1.0, 3), 0.3, 0.7, 0.85, 1.15)
    warp_col, weft_col = (0.012, 0.011, 0.010), (0.050, 0.040, 0.030)
    col = n.mix(h, weft_col, warp_col)
    col = n.add("ShaderNodeVectorMath", {0: col, "Scale": n.math("MULTIPLY", slub, n.remap(surface, 0, 1, 0.55, 1.0))},
                operation="SCALE").outputs["Vector"]
    n.shade(Base_Color=col, Roughness=0.85, Sheen_Weight=0.15, Sheen_Roughness=0.4,
            Normal=n.bump(surface, 0.45, 0.9))
    return n.mat


def _cone():
    """Pressed paper: long fibres running out from the centre, a faint mottle, a warm grey-brown, matt."""
    from materials import Nodes
    n = Nodes("cone")
    p = n.object_position()
    xyz = n.add("ShaderNodeSeparateXYZ", {"Vector": p}).outputs
    ang = n.add("ShaderNodeMath", {0: xyz["Y"], 1: xyz["X"]}, operation="ARCTAN2").outputs[0]
    rad = n.add("ShaderNodeVectorMath", {0: p}, operation="LENGTH").outputs["Value"]
    polar = n.add("ShaderNodeCombineXYZ", {"X": n.math("MULTIPLY", ang, 160.0), "Y": n.math("MULTIPLY", rad, 0.9)}).outputs[0]
    fibre = n.noise(polar, 1.0, 6, 0.75)                      # streaks along the radius
    mottle = n.noise(p, 0.08, 5, 0.7)
    grain = n.noise(p, 3.0, 4, 0.6)
    tone = n.math("ADD", n.math("MULTIPLY", fibre, 0.6), n.math("MULTIPLY", mottle, 0.4))
    n.shade(Base_Color=n.mix(n.remap(tone, 0.3, 0.7, 0, 1), (0.020, 0.0175, 0.015), (0.045, 0.039, 0.033)),
            Roughness=n.remap(grain, 0.3, 0.7, 0.78, 0.9),
            Normal=n.bump(n.math("ADD", n.math("MULTIPLY", fibre, 0.8), n.math("MULTIPLY", grain, 0.4)), 0.12, 0.7))
    return n.mat

def _dustcap():
    """A felt dust cap: dense fibre, strong sheen at grazing angles, near black."""
    from materials import Nodes
    n = Nodes("dustcap")
    p = n.object_position()
    fuzz = n.noise(p, 9.0, 6, 0.8)
    n.shade(Base_Color=n.mix(n.remap(fuzz, 0.3, 0.7, 0, 1), (0.008, 0.0075, 0.007), (0.018, 0.017, 0.016)),
            Roughness=0.95, Sheen_Weight=0.35, Sheen_Roughness=0.35, Sheen_Tint=(0.75, 0.7, 0.62),
            Normal=n.bump(fuzz, 0.08, 0.8))
    return n.mat

DOF = False


def lights(rig):
    import bpy
    a = lighting.area
    sx0, sz = (c * kit.MM for c in spk_world(0))
    # a warm key, high on the left, spread so it falls away across the cabinet towards the lower right
    key = a(rig, "key", (-0.95, -1.15, sz + 0.95), (0.15, 0.0, sz - 0.1), (0.55, 0.45), 7, (1.0, 0.86, 0.68))
    key.data.spread = math.radians(55)
    a(rig, "fill", (1.4, -1.6, 0.2), (0.0, 0.0, 0.05), (0.5, 0.9), 4, (0.72, 0.82, 1.0))
    a(rig, "rim", (0.1, -0.2, 1.4), (0.0, 0.1, 0.2), (1.4, 0.15), 8, (1.0, 0.92, 0.82))
    a(rig, "strip", (-0.2, -1.9, 0.2), (0.0, -1.05, -0.32), (1.4, 0.3), 14, (1.0, 0.92, 0.80))
    sides = [o for o in bpy.data.objects if o.name.startswith(("cab4x12_shell", "cab4x12_corner", "cab4x12_piping"))]
    for side in (-1, 1):
        k = a(rig, f"tolex_kicker{side}", (side * 1.3, -0.9, sz + 0.35), (side * 0.35, 0.05, sz - 0.1), (0.3, 0.9), 9,
              (1.0, 0.9, 0.76) if side < 0 else (0.85, 0.88, 0.95))
        lighting.only_lights(k, sides, f"Tolex{side}")
    # the cones: a soft, warm light close in from the upper left, seen only by the speakers
    for i in (0, 1):
        x, z = (c * kit.MM for c in spk_world(i))
        reveal = a(rig, f"cone_light{i}", (x - 0.35, -0.45, z + 0.32), (x, 0.04, z), (0.4, 0.4), 3.0, (1.0, 0.88, 0.72))
        lighting.only_lights(reveal, [o for o in bpy.data.objects if o.name.startswith(f"spk{i}_")], f"Cone{i}")
