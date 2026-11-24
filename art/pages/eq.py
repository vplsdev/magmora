"""An original four-band parametric equaliser, 4U, in a road-case rack, lit like the head.

A lit screen across the top shows the response (drawn live by the plug-in behind the glass); below it, one
column per band, colour-coded to match the band's node on the screen.
"""
import math

from mathutils import Matrix, Vector

import lighting
import meshes as M
import product
from pages import kit
from pages.view import View

VIEW = View(centre=(0.0, 0.0, 0.0), forward=(0.0, 1.0, 0.0), up=(0.0, 0.0, 1.0), pt_per_mm=2.45, elevation=170.0)
TITLE = "EQ"

U = 44.45
UNIT_TOP, UNIT_BOT = 84.0, 84.0 + 4 * U * 2.45
EAR = 15.9
PANEL_L, PANEL_R = 640.0 - 482.6 / 2 * 2.45, 640.0 + 482.6 / 2 * 2.45
KNOB_D = 21.5
ROW1, ROW2 = 340.0, 444.0
SCREEN = (196.0, 104.0, 1084.0, 270.0)          # x0, y0, x1, y1: the glass the plug-in draws the response behind
COLS = {"hp": 262.0, "b1": 410.0, "b2": 560.0, "b3": 710.0, "b4": 860.0, "lp": 1008.0}
LEFT_X = 136.0
BRAND_X = 1140.0
# band colours: printed on the panel, and the same on the screen's nodes (pages.json carries them)
COLOURS = {"hp": (0.78, 0.74, 0.66), "b1": (0.88, 0.36, 0.16), "b2": (0.90, 0.68, 0.22),
           "b3": (0.22, 0.70, 0.62), "b4": (0.42, 0.55, 0.92), "lp": (0.78, 0.74, 0.66)}
NAMES = {"hp": "HIGH-PASS", "b1": "LOW", "b2": "LOW MID", "b3": "HIGH MID", "b4": "HIGH", "lp": "LOW-PASS"}

DEFAULTS = dict(eq_on=1, eq_position=0, eq_hp_slope=1, eq_lp_slope=0, eq_b1_type=0, eq_b4_type=0)
STATES = {"off": dict(eq_on=0)}


def lit(values):
    return {"amber": float(values.get("eq_on", 0))}


PAIR = 37.0
KNOBS = [("eq_hp_freq", COLS["hp"], ROW1, "FREQ", "hp"), ("eq_lp_freq", COLS["lp"], ROW1, "FREQ", "lp")]
for _b in (1, 2, 3, 4):
    _x = COLS[f"b{_b}"]
    KNOBS += [(f"eq_b{_b}_freq", _x - PAIR, ROW1, "FREQ", f"b{_b}"), (f"eq_b{_b}_gain", _x + PAIR, ROW1, "GAIN", f"b{_b}"),
              (f"eq_b{_b}_q", _x if _b in (2, 3) else _x - PAIR, ROW2, "Q", f"b{_b}")]
SELECTORS = [("eq_hp_slope", COLS["hp"], ROW2, "SLOPE", "hp"), ("eq_lp_slope", COLS["lp"], ROW2, "SLOPE", "lp")]
SLOPE_ANGLES = (-60.0, 0.0, 60.0)
SLOPE_TEXT = ("OFF", "12", "24")
TOGGLES = [("eq_b1_type", COLS["b1"] + PAIR, ROW2, ("BELL", "SHELF")), ("eq_b4_type", COLS["b4"] + PAIR, ROW2, ("BELL", "SHELF"))]
ORDER = ("eq_position", LEFT_X, ROW2)
BUTTON = ("eq_on", LEFT_X, ROW1)
LED = (LEFT_X, ROW1 - 34)

CONTROLS = (
    [dict(id=k, kind="knob", x=x, y=y, asset="eq_knob", label=t, hit=(66, 72), band=c) for k, x, y, t, c in KNOBS]
    + [dict(id=k, kind="selector", x=COLS[c], y=y, asset="eq_knob", label=t, hit=(66, 72),
            detents=[round((a / 300.0 + 0.5) * 96) for a in SLOPE_ANGLES]) for k, _, y, t, c in SELECTORS]
    + [dict(id=k, kind="switch", x=x, y=y, asset="eq_toggle", hit=(40, 64)) for k, x, y, _ in TOGGLES]
    + [dict(id=ORDER[0], kind="switch", x=ORDER[1], y=ORDER[2], asset="eq_toggle_h", hit=(64, 40)),
       dict(id=BUTTON[0], kind="switch", x=BUTTON[1], y=BUTTON[2], asset="eq_button", hit=(50, 50)),
       dict(id="eq_led", kind="lamp", x=LED[0], y=LED[1], asset="eq_glow", source="eq_on", on_value=1),
       dict(id="eq_screen", kind="window", x=(SCREEN[0] + SCREEN[2]) / 2, y=(SCREEN[1] + SCREEN[3]) / 2,
            w=SCREEN[2] - SCREEN[0], h=SCREEN[3] - SCREEN[1],
            colours={k: [round(c, 3) for c in v] for k, v in COLOURS.items()})]
)
ASSETS = {
    "eq_knob": dict(kind="knob", collection="Knob", ref="eq_b2_q", cell=(68, 68)),
    "eq_toggle": dict(kind="switch", collection="Toggle", ref="eq_b1_type", cell=(40, 64), tilt=25.0, only=["eq_b1_type", "eq_b4_type"]),
    "eq_toggle_h": dict(kind="switch", collection="Toggle", ref="eq_position", cell=(64, 40), tilt_y=25.0, only=["eq_position"]),
    "eq_button": dict(kind="switch", collection="Button", ref="eq_on", cell=(48, 48), travel=4.5),
    "eq_glow": dict(kind="lamp", ref="eq_led", cell=(48, 48)),
    "eq_glass": dict(kind="glass", window="eq_screen"),   # the display glass's reflections, added over the display
}


def angle(t):
    return (t - 0.5) * 300.0


def unit(b):
    v = VIEW
    top, bot = v.world(640, UNIT_TOP)[2], v.world(640, UNIT_BOT)[2]
    left, right = v.world(PANEL_L, 300)[0], v.world(PANEL_R, 300)[0]
    M.box("eq_panel", ((left + right) / 2, 3.0, (top + bot) / 2), (right - left, 6.0, top - bot - 0.6),
          b.mats["eq_panel"], b.static, 2.2)
    M.box("eq_chassis", ((left + right) / 2, 156, (top + bot) / 2 + 2), (430, 300, top - bot - 6), b.mats["rack_black"], b.static, 1.0)
    # the screen: a bezel standing proud, then the glass set back into it
    x0, y0, x1, y1 = SCREEN
    a, c = v.plane(x0, y0), v.plane(x1, y1)
    w, h = c[0] - a[0], a[1] - c[1]
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    t = 7.0
    for name, (bx, by, bw, bh) in {"top": (0, h / 2 + t / 2, w + 2 * t, t), "bottom": (0, -h / 2 - t / 2, w + 2 * t, t),
                                   "left": (-w / 2 - t / 2, 0, t, h), "right": (w / 2 + t / 2, 0, t, h)}.items():
        bar = M.box("screen_bezel_" + name, (0, 0, 0), (bw, bh, 4.0), b.mats["bezel_satin"], b.static, 1.6)
        bar.matrix_world = kit.frame(v, cx, cy) @ Matrix.Translation(Vector((bx, by, 1.6)) * kit.MM)
    # an opening in the faceplate for the display
    cut = M.box("screen_cut", (0, 0, 0), (w + 0.2, h + 0.2, 30.0), b.mats["recess"], b.static)
    cut.matrix_world = kit.frame(v, cx, cy)
    cut.hide_render = True
    import bpy
    panel = bpy.data.objects["eq_panel"]
    mod = panel.modifiers.new("screen", "BOOLEAN")
    mod.operation, mod.object, mod.solver = "DIFFERENCE", cut, "EXACT"
    panel.modifiers.move(len(panel.modifiers) - 1, 0)
    # the display sits back in a well: its walls catch the light
    well = M.rounded_plate("screen_well", w + 0.4, h + 0.4, 1.0, [(0, -6.0), (0, 0.2)], b.mats["recess"], b.static, cap=False)
    well.matrix_world = kit.frame(v, cx, cy)
    glass = M.rounded_plate("eq_screen", w, h, 1.0, [(0, -4.0), (0, -3.4)], b.mats["display_glass"], b.static)
    glass.matrix_world = kit.frame(v, (x0 + x1) / 2, (y0 + y1) / 2)
    from parts.common import screw
    for side in (-1, 1):
        x = (left + right) / 2 + side * (482.6 / 2 - EAR / 2)
        for z in (top - 6.35, bot + 6.35):
            slot = M.rounded_plate("ear_slot", 10.0, 6.6, 3.3, [(0, -6.2), (0, 0.05)], b.mats["recess"], b.static)
            slot.matrix_world = Matrix.Translation(Vector((x, -0.06, z)) * kit.MM) @ M.UP_IS_FRONT
            screw(b, "rack_screw", (x + side * 1.2, -1.6, z), 3.6, 1.6, 0.4 + z * 0.01, "nickel")
            washer = M.lathe("rack_washer", [(5.6, 0), (5.6, 0.9), (4.2, 1.1)], b.mats["black_plastic"], b.static, 48)
            washer.matrix_world = Matrix.Translation(Vector((x + side * 1.2, -0.06, z)) * kit.MM) @ M.UP_IS_FRONT


def handles(b):
    """Two bent-bar rack handles on the ears, standing off the panel on bosses: real gear, and depth."""
    v = VIEW
    top, bot = v.world(640, UNIT_TOP)[2], v.world(640, UNIT_BOT)[2]
    for side in (-1, 1):
        x = side * (482.6 / 2 - EAR / 2 - 1.0)
        z0, z1 = bot + 22.0, top - 22.0
        path = [(x, -1.0, z0), (x, -24.0, z0 + 2.0), (x, -30.0, z0 + 14.0), (x, -30.0, z1 - 14.0), (x, -24.0, z1 - 2.0),
                (x, -1.0, z1)]
        M.tube(f"handle{side}", M.spline(path, 16), 4.8, b.mats["handle_chrome"], b.static, 32)
        for z in (z0, z1):
            boss = M.lathe("handle_boss", [(7.5, 0.0), (7.5, 2.0), (6.5, 3.0), (5.0, 3.2)], b.mats["handle_chrome"], b.static, 48)
            boss.matrix_world = Matrix.Translation(Vector((x, -0.2, z)) * kit.MM) @ M.UP_IS_FRONT


def blank(b, name, top_u, units, vented):
    eq_top = VIEW.world(640, UNIT_TOP)[2]
    top = eq_top + top_u * U
    bot = top - units * U
    M.box(name, (0, 1.0, (top + bot) / 2), (482.6, 2.0, units * U - 0.6), b.mats["rack_black"], b.static, 0.6)
    if vented:
        for i in range(-7, 8):
            for row in range(units):
                slot = M.rounded_plate("vent", 4.4, 26.0, 2.2, [(0, -3.0), (0.3, -0.1), (0.6, 0.0)], b.mats["recess"], b.static)
                slot.matrix_world = Matrix.Translation(Vector((i * 21.0, -0.05, top - U * (row + 0.5))) * kit.MM) @ M.UP_IS_FRONT
    from parts.common import screw
    for side in (-1, 1):
        for row in range(units):
            for dz in (6.35, U - 6.35):
                z = top - row * U - dz
                screw(b, name + "_screw", (side * (482.6 / 2 - EAR / 2), -2.0, z), 3.6, 1.6, z * 0.07, "nickel")


def case(b):
    eq_top = VIEW.world(640, UNIT_TOP)[2]
    top, bot = eq_top + 3 * U, eq_top - 7 * U
    for side in (-1, 1):
        x = side * (482.6 / 2 - EAR / 2)
        M.box("rail", (x, 4.0, (top + bot) / 2), (EAR + 2, 4.0, top - bot), b.mats["rack_rail"], b.static, 0.4)
        wall_x = side * (482.6 / 2 + 11)
        M.box(f"case_wall{side}", (wall_x, 230, (top + bot) / 2), (18, 520, top - bot + 80), b.mats["tolex"], b.static, 1.5)
        M.box(f"case_edge{side}", (wall_x, -12, (top + bot) / 2), (22, 26, top - bot + 80), b.mats["rack_rail"], b.static, 1.2)
    M.box("case_back", (0, 520, (top + bot) / 2), (560, 4, top - bot + 80), b.mats["interior"], b.static)


def band_inks(b):
    from materials import simple
    for key, col in COLOURS.items():
        b.mats["ink_" + key] = simple("ink_" + key, col, 0.55)


def print_panel(b):
    v = VIEW
    for key in COLS:
        kit.text(b, v, NAMES[key], 9.4, COLS[key], ROW1 - 48.0, material="ink_" + key, tracking=1.14)
    for k, x, y, t, c in KNOBS:
        kit.text(b, v, t, 8.4, x, y + 38.0, font="medium", tracking=1.1)
        ends = (-150.0, 150.0) + ((0.0,) if t == "GAIN" else ())
        kit.ticks(b, v, "scale_" + k, x, y, 30.0, 34.0, ends, 0.8)
        kit.ring(b, v, "ring_" + k, x, y, 27.4, 28.6, material="ink_" + c, a0=-150, a1=150)
    for k, x, y, t, c in SELECTORS:
        kit.text(b, v, t, 8.4, x, y + 38.0, font="medium", tracking=1.1)
        kit.ticks(b, v, "scale_" + k, x, y, 30.0, 34.0, SLOPE_ANGLES, 0.9)
        for a, txt in zip(SLOPE_ANGLES, SLOPE_TEXT):
            kit.text(b, v, txt, 7.4, x + 42.0 * math.sin(math.radians(a)), y - 42.0 * math.cos(math.radians(a)),
                     font="medium", tracking=1.0)
        kit.ring(b, v, "ring_" + k, x, y, 27.4, 28.6, material="ink_" + c, a0=-150, a1=150)
    for k, x, y, (up, down) in TOGGLES:
        kit.text(b, v, up, 7.6, x, y - 25.0, font="medium", tracking=1.08)
        kit.text(b, v, down, 7.6, x, y + 25.0, font="medium", tracking=1.08)
    k, x, y = ORDER
    kit.text(b, v, "ORDER", 9.4, x, y - 34.0, tracking=1.14)
    kit.text(b, v, "PRE", 7.6, x - 36.0, y, font="medium", tracking=1.08)
    kit.text(b, v, "POST", 7.6, x + 38.0, y, font="medium", tracking=1.08)
    kit.text(b, v, "CAB", 7.0, x, y + 24.0, font="medium", tracking=1.2)
    kit.text(b, v, "IN", 9.4, BUTTON[1], BUTTON[2] + 32.0, tracking=1.14)
    # thin rules between the columns
    for xr in (196.0, 336.0, 485.0, 635.0, 785.0, 935.0, 1084.0):
        kit.strip(b, v, "rule", xr - 0.45, ROW1 - 58.0, xr + 0.45, UNIT_BOT - 18.0)
    kit.word(b, v, product.BADGE_TEXT, 13.0, BRAND_X, ROW1 - 6.0)
    kit.text(b, v, "PARAMETRIC", 7.0, BRAND_X, ROW1 + 16.0, font="medium", tracking=1.4)
    kit.text(b, v, "EQUALISER", 7.0, BRAND_X, ROW1 + 28.0, font="medium", tracking=1.4)
    kit.text(b, v, "4 BANDS  HP  LP", 6.6, BRAND_X, ROW2, font="medium", tracking=1.3)


def knobs(b, colls):
    mats = dict(body=b.mats["knob"], cut=b.mats["bright_cut"], crown=b.mats["knob_worn"], top=b.mats["knob_top"],
                pointer=b.mats["pointer"])
    for k, x, y, *_ in KNOBS:
        e = kit.mount(colls["Knob"], k, VIEW, x, y)
        body, _ = kit.knob(b, colls["Knob"], k, KNOB_D, mats, teeth=44, height=16.0)
        kit.attach(body, e)
        kit.drive_turn(body)
    for k, _, y, _, c in SELECTORS:
        e = kit.mount(colls["Knob"], k, VIEW, COLS[c], y)
        body, _ = kit.knob(b, colls["Knob"], k, KNOB_D, mats, teeth=44, height=16.0)
        kit.attach(body, e)
        kit.drive_turn(body)


def toggle(b, colls, k, x, y, sideways=False):
    from parts.panel import hex_radius
    e = kit.mount(colls["Toggle"], k, VIEW, x, y)
    nut = M.lathe(k + "_nut", [(5.4, 0.6), (5.4, 2.2), (4.9, 2.7), (3.4, 2.7), (3.4, 4.3), (3.0, 4.6), (1.2, 4.6)],
                  b.mats["nickel"], colls["Toggle"], 120, hex_radius(4.7), sharp_deg=20)
    kit.attach(nut, e)
    bat = M.lathe(k, [(1.8, 2.8), (1.65, 5.0), (1.65, 6.5), (2.1, 9.5), (2.7, 13.0), (2.85, 14.4), (2.6, 15.6),
                      (1.8, 16.4), (0.05, 16.7)], b.mats["nickel"], colls["Toggle"], 64)
    kit.attach(bat, e)
    bat["down"] = False


def button(b, colls):
    k, x, y = BUTTON
    size = 9.5
    bezel = M.rounded_plate("eq_on_bezel", size + 4.4, size + 4.4, 2.2,
                            [(0, 0), (0, 3.0), (0.4, 3.6), (0.8, 3.8), (1.6, 3.8), (2.0, 3.5), (2.1, 0.1)],
                            b.mats["satin_plastic"], b.static)
    bezel.matrix_world = kit.frame(VIEW, x, y)
    e = kit.mount(colls["Button"], k, VIEW, x, y)
    cap = M.rounded_plate(k, size, size, 1.6, [(0, -5.0), (0, 4.6), (0.3, 5.2), (0.7, 5.5), (1.3, 5.6), (2.4, 5.45),
                                               (4.75, 5.38)], b.mats["button_cap"], colls["Button"])
    kit.attach(cap, e)
    cap["pressed"] = False


def led(b, colls):
    x, y = LED
    m = kit.frame(VIEW, x, y)
    bezel = M.lathe("eq_led_bezel", [(3.6, 0), (3.6, 1.0), (3.2, 1.5), (2.6, 1.5), (2.4, 1.0)], b.mats["satin_plastic"], b.static)
    bezel.matrix_world = m
    lens = M.lathe("eq_led", [(2.45, 0.8), (2.45, 1.7), (2.2, 2.5), (1.6, 3.0), (0.8, 3.3), (0.02, 3.4)], b.mats["amber"],
                   colls["Lamps"], 48)
    lens.matrix_world = m


def build(b, colls):
    band_inks(b)
    from materials import simple
    b.mats["handle_chrome"] = simple("handle_chrome", (0.62, 0.60, 0.57), 0.16, 1.0)
    b.mats["bezel_satin"] = simple("bezel_satin", (0.028, 0.027, 0.026), 0.42, 0.6)
    unit(b)
    handles(b)
    blank(b, "vent_panel", 1, 1, True)
    blank(b, "blank_panel", -4, 2, False)
    case(b)
    print_panel(b)
    knobs(b, colls)
    for k, x, y, _ in TOGGLES:
        toggle(b, colls, k, x, y)
    toggle(b, colls, *ORDER, sideways=True)
    button(b, colls)
    led(b, colls)
    M.box("wall", (0, 1200, 0), (4000, 4, 3000), b.mats["wall"], b.static)


def apply(values):
    """Toggles at their default positions for the plate's shadows; the order switch lies sideways."""
    import bpy
    for k, *_ in TOGGLES:
        bpy.data.objects[k].rotation_euler.x = math.radians(-25)
    bpy.data.objects[ORDER[0]].rotation_euler.y = math.radians(-25)


def lights(rig):
    a = lighting.area
    a(rig, "key", (-0.62, -1.05, 0.82), (-0.05, 0, 0), (0.85, 0.6), 26, (1.0, 0.93, 0.84))
    a(rig, "fill", (1.1, -1.0, 0.15), (0.1, 0, 0), (0.45, 0.9), 6, (0.76, 0.86, 1.0))
    a(rig, "top_strip", (0.05, -0.3, 0.95), (0, -0.02, 0), (1.5, 0.12), 6, (1.0, 0.97, 0.93))
    a(rig, "sheen", (-0.38, -1.4, -0.40), (-0.02, 0, -0.07), (1.5, 0.3), 6.0, (1.0, 0.92, 0.82))
    a(rig, "bounce", (0.0, -1.0, -0.65), (0, 0, -0.05), (1.8, 0.3), 2.2, (1.0, 0.9, 0.8))
    # a long soft card where the glass mirrors it, so the display reads as glass
    card = a(rig, "glass_card", (-0.45, -2.5, 0.06), (-0.05, 0, 0.12), (1.2, 0.5), 1.6, (0.95, 0.97, 1.0))
    card.data.use_shadow = False
