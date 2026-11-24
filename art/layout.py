"""Single source of positions and sizes for the scene, the placeholders and layout.json.

Screen values are points on the render, 1280 x 576, origin top left. layout.json adds the
height of the utility bar. No bpy here: plain Python imports this file too.

The camera is level and uses lens shift, so every plane parallel to the panel maps linearly
to the screen. screen_to_world() is the exact inverse of the render.
"""
import json
import math

# ---- canvas and camera -----------------------------------------------------------------------
CANVAS_W, CANVAS_H = 1280, 620
BAR_H = 44
RENDER_W, RENDER_H = 1280, 576
SCALE = 2
BAR_COLOUR = "#37322c"          # the wall along the top edge of the render

PT_PER_MM = 1.818                # on the panel plane
LENS_MM, SENSOR_MM = 85.0, 36.0
VIEW_W_MM = RENDER_W / PT_PER_MM
CAMERA_DIST = VIEW_W_MM * LENS_MM / SENSOR_MM
CAMERA_ELEVATION_DEG = 8.5       # above the panel centre line

# ---- the head, screen points -----------------------------------------------------------------
HEAD = (44, 72, 1236, 492)       # front of the tolex frame
BAND_X, BAND_Y = 28.0, 27.0      # width of the covered frame
OPENING = (HEAD[0] + BAND_X, HEAD[1] + BAND_Y, HEAD[2] - BAND_X, HEAD[3] - BAND_Y)
PANEL = (OPENING[0], 312, OPENING[2], OPENING[3])
PLATE = (PANEL[0] + 3.5, PANEL[1] + 1.5, PANEL[2] - 3.5, PANEL[3] - 2.5)
TRIM = (OPENING[0], 305, OPENING[2], 312)
GRILLE = (OPENING[0], OPENING[1], OPENING[2], 305)
BADGE = (1016, 253, 1181, 291)

# ---- control strip ---------------------------------------------------------------------------
CY = 366.0                       # every knob centre
LABEL_CY = 422.0
GROUP_CY = 446.0
KNOB_D = 56.0
KNOB_TRAVEL_DEG = 300.0
TOGGLE_LABEL_D = 34.0             # HIGH and NORMAL, from the pivot
TICK_R = (31.5, 36.5)
NUMERAL_R = 44.0

CELLS = {                        # points; every cell lands on whole pixels at 1x and 2x
    "knob": (60, 64), "knob_light": (60, 64),
    "button": (40, 40), "toggle": (52, 88), "rocker": (52, 80), "glow": (64, 64),
}
ATLASES = {                      # columns, rows, frames
    "knob": (10, 10, 97), "knob_light": (9, 1, 9),
    "button": (2, 1, 2), "toggle": (2, 1, 2), "rocker": (2, 1, 2), "glow": (1, 1, 1),
}
# Knobs share two atlases, each rendered at one knob's position. A static per-position overlay,
# fitted over all 96 frames, puts that position's own light back.
KNOB_ATLASES = {"knob_left": "pre_lead", "knob_right": "post_lead"}
KNOB_SPLIT_X = 650
GLOWS = ("green", "red", "amber", "power")
TUBE_GLOW = (250, 99, 780, 206)  # x, y, w, h
BLEND_TOP_PT = 21                # the plate fades into the utility bar's tone above this
BUTTON_SIZE = 24.0
ROCKER_SIZE = (34.0, 52.0)

# key, centre x, printed name
KNOBS = [
    ("pre_rhythm", 225, "PRE"),
    ("pre_lead", 425, "PRE"),
    ("low", 511, "LOW"),
    ("mid", 593, "MID"),
    ("high", 675, "HIGH"),
    ("post_rhythm", 762, "RHYTHM"),
    ("post_lead", 844, "LEAD"),
    ("resonance", 931, "RESONANCE"),
    ("presence", 1013, "PRESENCE"),
]
JACK = (110, 376)
TOGGLE = ("input_jack", 160, 366)
BUTTONS = [                      # key, x, y, name, y of the name
    ("bright", 291, 334, "BRIGHT", 359.8),
    ("crunch", 291, 386, "CRUNCH", 411.5),
    ("channel", 351, 388, "CHANNEL", LABEL_CY),
]
ROCKERS = [("standby", 1090, 383, "STANDBY"), ("power", 1154, 383, "POWER")]
LAMPS = [                        # id, glow, x, y, parameter, value at which it is lit
    ("lamp_rhythm", "green", 330, 330, "channel", 0),
    ("lamp_lead", "red", 330, 350, "channel", 1),
    ("lamp_standby", "amber", 1090, 334, "standby", 1),
    ("lamp_power", "power", 1154, 334, "power", 1),
]
LAMP_NAMES = [("RHYTHM", 339, 330), ("LEAD", 339, 350)]
GROUPS = [                       # printed cradle with the section name set into it
    ("Rhythm", 187, 316), ("Lead", 386, 462), ("Tone", 474, 712),
    ("Post", 724, 882), ("Power amp", 894, 1050),
]


def camera_height():
    centre = (PANEL[1] + PANEL[3]) / 2
    return (RENDER_H / 2 - centre) / PT_PER_MM + CAMERA_DIST * math.tan(
        math.radians(CAMERA_ELEVATION_DEG))


def screen_to_world(x, y, depth=0.0):
    """Millimetres (X, Y, Z) of the point at `depth` behind the panel that lands on (x, y)."""
    k = (CAMERA_DIST + depth) / CAMERA_DIST
    z = (RENDER_H / 2 - y) / PT_PER_MM
    top = camera_height()
    return ((x - RENDER_W / 2) / PT_PER_MM * k, depth, top + (z - top) * k)


def knob_axis_offset(height_mm=16.8):
    """Points by which the top of a knob, seen from above, sits below the centre of its scale."""
    base = (RENDER_H / 2 - CY) / PT_PER_MM
    return (camera_height() - base) * height_mm / (CAMERA_DIST - height_mm) * PT_PER_MM


def knob_degrees(value):
    """Clockwise from twelve o'clock, for a value in 0..1."""
    return (value - 0.5) * KNOB_TRAVEL_DEG


def knob_atlas(x):
    return "knob_left" if x < KNOB_SPLIT_X else "knob_right"


def manifest():
    assets = {"bg": dict(file="bg.jpg", kind="plate", w=RENDER_W, h=RENDER_H)}

    def add(name, kind):
        cols, rows, frames = ATLASES[kind]
        w, h = CELLS[kind]
        assets[name] = dict(file=name + ".png", kind=kind, w=w * cols, h=h * rows,
                            cell=[w, h], columns=cols, rows=rows, frames=frames)
        return name

    def control(cid, kind, x, y, asset, hit=(0, 0), **extra):
        w, h = CELLS[kind]
        cols, rows, frames = ATLASES[kind]
        item = dict(id=cid, type=kind, x=x, y=y + BAR_H, w=w, h=h, asset=asset,
                    frames=frames, columns=cols, rows=rows, hit=list(hit), blend="over")
        item.update(extra)
        return item

    # in drawing order
    gx, gy, gw, gh = TUBE_GLOW
    assets["glow_tubes"] = dict(file="glow_tubes.jpg", kind="additive", w=gw, h=gh)
    controls = [dict(id="tubes", type="glow", x=gx + gw / 2, y=gy + gh / 2 + BAR_H, w=gw, h=gh,
                     asset="glow_tubes", frames=1, columns=1, rows=1, hit=[0, 0],
                     blend="add", source="power", on_value=1, requires="power")]
    for name in KNOB_ATLASES:
        add(name, "knob")
    controls += [control(key, "knob", x, CY, knob_atlas(x), (74, 82)) for key, x, _ in KNOBS]
    add("knob_light", "knob_light")
    controls += [control(key + "_light", "knob_light", x, CY, "knob_light", frame=i)
                 for i, (key, x, _) in enumerate(KNOBS)]
    controls += [control(key, "button", x, y, add("button_" + key, "button"), (46, 40))
                 for key, x, y, _, _ in BUTTONS]
    key, x, y = TOGGLE
    controls.append(control(key, "toggle", x, y, add("toggle", "toggle"), (42, 88)))
    controls += [control(key, "rocker", x, y, add("rocker_" + key, "rocker"), (54, 62))
                 for key, x, y, _ in ROCKERS]
    controls += [control(cid, "glow", x, y, add("glow_" + colour, "glow"),
                         source=source, on_value=on, requires="power")
                 for cid, colour, x, y, source, on in LAMPS]

    return dict(
        version=2,
        units="points; x and y are centres on the canvas; hit is the width and height of the mouse target",
        drawing="background, then controls in list order; knob_light items are static and draw `frame`; "
                "glow items show while `source` equals `on_value` and `requires` is on",
        glow_note="over: weight = intensity. add: opaque on black, and iPlug2 Add squares the weight, "
                  "so weight = sqrt(intensity)",
        canvas=dict(w=CANVAS_W, h=CANVAS_H),
        bar=dict(x=0, y=0, w=CANVAS_W, h=BAR_H, colour=BAR_COLOUR),
        background=dict(asset="bg", x=0, y=BAR_H, w=RENDER_W, h=RENDER_H),
        retina_scale=SCALE,
        knob_axis_offset=[0, round(knob_axis_offset(), 2)],
        knob_angle_deg=[-KNOB_TRAVEL_DEG / 2, KNOB_TRAVEL_DEG / 2],
        assets=assets,
        controls=controls,
    )


def write_manifest(*paths):
    text = json.dumps(manifest(), indent=2) + "\n"
    for path in paths:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
