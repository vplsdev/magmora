"""Every material in the scene, built as nodes."""
import bpy
import numpy as np

import layout as L


class Nodes:
    def __init__(self, name):
        self.mat = bpy.data.materials.new(name)
        self.mat.use_nodes = True
        self.tree = self.mat.node_tree
        self.bsdf = self.tree.nodes["Principled BSDF"]
        self.out = self.tree.nodes["Material Output"]

    def add(self, kind, inputs=None, **props):
        node = self.tree.nodes.new(kind)
        for key, value in props.items():
            setattr(node, key, value)
        for key, value in (inputs or {}).items():
            self.set(node, key, value)
        return node

    def set(self, node, key, value):
        socket = node.inputs[key]
        if isinstance(value, bpy.types.NodeSocket):
            self.tree.links.new(value, socket)
        elif isinstance(value, (tuple, list)) and socket.type == "RGBA" and len(value) == 3:
            socket.default_value = (*value, 1)
        else:
            socket.default_value = value

    def shade(self, **values):
        for key, value in values.items():
            self.set(self.bsdf, key.replace("_", " "), value)

    def position(self, scale=(1000, 1000, 1000)):
        coord = self.add("ShaderNodeNewGeometry")
        return self.add("ShaderNodeVectorMath", {0: coord.outputs["Position"], 1: scale},
                        operation="MULTIPLY").outputs["Vector"]

    def object_position(self, scale=(1000, 1000, 1000)):
        coord = self.add("ShaderNodeTexCoord")
        return self.add("ShaderNodeVectorMath", {0: coord.outputs["Object"], 1: scale},
                        operation="MULTIPLY").outputs["Vector"]

    def noise(self, vector, scale, detail=3.0, rough=0.55):
        return self.add("ShaderNodeTexNoise",
                        {"Vector": vector, "Scale": scale, "Detail": detail, "Roughness": rough}
                        ).outputs[0]

    def remap(self, value, a, b, c, d):
        return self.add("ShaderNodeMapRange",
                        {"Value": value, "From Min": a, "From Max": b, "To Min": c, "To Max": d}
                        ).outputs["Result"]

    def math(self, op, a, b=0.0):
        return self.add("ShaderNodeMath", {0: a, 1: b}, operation=op).outputs["Value"]

    def mix(self, factor, a, b):
        node = self.add("ShaderNodeMix", data_type="RGBA")
        self.tree.links.new(factor, node.inputs[0])
        for socket, value in ((node.inputs[6], a), (node.inputs[7], b)):
            if isinstance(value, bpy.types.NodeSocket):
                self.tree.links.new(value, socket)
            else:
                socket.default_value = (*value, 1)
        return node.outputs[2]

    def bump(self, height, distance_mm, strength=1.0):
        return self.add("ShaderNodeBump", {"Height": height, "Distance": distance_mm * 0.001,
                                           "Strength": strength}).outputs["Normal"]

    def attribute(self, name):
        return self.add("ShaderNodeAttribute", attribute_name=name).outputs["Fac"]


def simple(name, colour, roughness, metallic=0.0, **more):
    n = Nodes(name)
    n.shade(Base_Color=colour, Roughness=roughness, Metallic=metallic, **more)
    return n.mat


def tolex():
    n = Nodes("tolex")
    grain = n.attribute("grain")
    p = n.position()
    large = n.noise(p, 0.018, 4)
    worn = n.math("MULTIPLY", n.attribute("worn"), n.remap(n.noise(p, 0.05, 5, 0.7), 0.45, 0.7, 0, 1))
    base = n.mix(n.remap(grain, 0.2, 0.8, 0, 1), (0.0045, 0.0043, 0.0042), (0.019, 0.018, 0.0172))
    rough = n.math("ADD", n.remap(grain, 0.2, 0.8, 0.58, 0.30), n.remap(large, 0.3, 0.7, -0.03, 0.05))
    n.shade(Base_Color=base, Roughness=n.math("SUBTRACT", rough, n.math("MULTIPLY", worn, 0.06)),
            Specular_IOR_Level=0.62, Normal=n.bump(n.noise(p, 3.2, 2), 0.03, 0.5))
    return n.mat


def cap_steel():
    n = Nodes("cap_steel")
    p = n.position()
    speck = n.noise(p, 2.2, 2)
    scuff = n.attribute("scuff")
    broken = n.remap(n.noise(p, 0.9, 6, 0.8), 0.51, 0.57, 0, 1)
    worn = n.math("MULTIPLY", scuff, broken)
    n.shade(Base_Color=n.mix(worn, (0.010, 0.010, 0.011), (0.22, 0.205, 0.19)),
            Roughness=n.remap(worn, 0, 1, 0.33, 0.28),
            Metallic=n.remap(worn, 0, 1, 0.0, 1.0),
            Normal=n.bump(speck, 0.03, 0.7))
    return n.mat


def powder(name="powder", colour=(0.008, 0.008, 0.009), rough=0.42, dust=False):
    n = Nodes(name)
    p = n.position()
    base = colour
    if dust:
        spots = n.remap(n.noise(p, 0.9, 5, 0.8), 0.53, 0.70, 0, 1)
        amount = n.math("MULTIPLY", spots, n.attribute("dust"))
        base = n.mix(amount, colour, (0.07, 0.06, 0.05))
    n.shade(Base_Color=base, Roughness=n.remap(n.noise(p, 0.03, 3), 0.3, 0.7, rough - 0.06, rough + 0.06),
            Normal=n.bump(n.noise(p, 2.0, 2), 0.025, 0.6))
    return n.mat


def panel_wear(width=2400):
    """0..1 map over the plate: rubbed edges, a ring round each knob where fingers go, the input."""
    x0, _, z1 = L.screen_to_world(L.PLATE[0], L.PLATE[1])
    x1, _, z0 = L.screen_to_world(L.PLATE[2], L.PLATE[3])
    height = round(width * (z1 - z0) / (x1 - x0))
    px, pz = np.meshgrid(np.linspace(x0, x1, width), np.linspace(z0, z1, height))
    rng = np.random.default_rng(11)
    grain = np.zeros_like(px)
    for scale, weight in ((9.0, 0.5), (3.0, 0.3), (1.0, 0.2)):
        coarse = rng.random((int(height / width * (x1 - x0) / scale) + 3, int((x1 - x0) / scale) + 3))
        iy = (pz - z0) / scale
        ix = (px - x0) / scale
        i, j = ix.astype(int), iy.astype(int)
        fx, fy = ix - i, iy - j
        grain += weight * ((coarse[j, i] * (1 - fx) + coarse[j, i + 1] * fx) * (1 - fy)
                           + (coarse[j + 1, i] * (1 - fx) + coarse[j + 1, i + 1] * fx) * fy)
    edge = np.minimum.reduce((px - x0, x1 - px, pz - z0, z1 - pz))
    wear = np.exp(-edge / 1.6) * 0.9
    for _, x, _ in L.KNOBS:
        kx, _, kz = L.screen_to_world(x, L.CY)
        r = np.hypot(px - kx, pz - kz)
        wear += 0.42 * np.exp(-((r - 19.0) / 2.6) ** 2)
    jx, _, jz = L.screen_to_world(*L.JACK)
    wear += 0.5 * np.exp(-(np.hypot(px - jx, pz - jz) - 12) ** 2 / 60)
    wear = np.clip(wear * np.clip((grain - 0.5) * 4.0, 0, 1), 0, 1)
    image = bpy.data.images.new("panel_wear", width, height, alpha=False, float_buffer=True)
    rgba = np.repeat(wear[..., None], 4, 2)
    rgba[..., 3] = 1
    image.pixels.foreach_set(rgba.astype(np.float32).ravel())
    image.pack()
    image.colorspace_settings.name = "Non-Color"
    return image, (x0, x1, z0, z1)


def wear_lookup(n, image, bounds):
    """The plate's wear at the shading point, from world X and Z."""
    x0, x1, z0, z1 = bounds
    axes = n.add("ShaderNodeSeparateXYZ", {"Vector": n.add("ShaderNodeNewGeometry").outputs["Position"]}).outputs
    u = n.remap(axes["X"], x0 * 0.001, x1 * 0.001, 0, 1)
    v = n.remap(axes["Z"], z0 * 0.001, z1 * 0.001, 0, 1)
    uv = n.add("ShaderNodeCombineXYZ", {"X": u, "Y": v}).outputs[0]
    tex = n.add("ShaderNodeTexImage", {"Vector": uv}, image=image, extension="EXTEND", interpolation="Linear")
    return tex.outputs["Color"]


def anodised_panel(wear_image, bounds):
    n = Nodes("bronze_panel")
    # three grades of line: the grit, the belt, and slow banding
    hair = n.noise(n.position((1000 * 0.009, 1000, 1000 * 1.9)), 1.0, 3, 0.6)
    belt = n.noise(n.position((1000 * 0.003, 1000, 1000 * 0.75)), 1.0, 4, 0.7)
    broad = n.noise(n.position(), 0.006, 3)
    brush = n.math("ADD", n.math("MULTIPLY", hair, 0.85), n.math("MULTIPLY", belt, 0.15))
    base = n.mix(n.remap(brush, 0.36, 0.64, 0, 1), (0.052, 0.037, 0.026), (0.092, 0.069, 0.049))
    p = n.position()
    scratches = n.noise(n.position((1000 * 0.05, 1000, 1000 * 2.6)), 0.35, 1)
    lines = n.remap(n.math("ABSOLUTE", n.math("SUBTRACT", scratches, 0.5)), 0.0, 0.003, 1, 0)
    patches = n.remap(n.noise(p, 0.05, 3), 0.48, 0.56, 0, 1)
    by_input = n.remap(n.add("ShaderNodeSeparateXYZ", {"Vector": p}).outputs["X"], -262, -306, 0.1, 1)
    lines = n.math("MULTIPLY", lines, n.math("MULTIPLY", patches, by_input))
    wear = n.add("ShaderNodeSeparateColor", {"Color": wear_lookup(n, wear_image, bounds)}).outputs["Red"]
    # rubbed anodising goes paler and a little smoother, never to bare shiny metal
    rubbed = n.mix(wear, base, (0.125, 0.10, 0.075))
    rough = n.math("ADD", n.remap(brush, 0.36, 0.64, 0.27, 0.385), n.remap(broad, 0.3, 0.7, -0.03, 0.04))
    rough = n.math("SUBTRACT", rough, n.math("MULTIPLY", wear, 0.07))
    n.shade(Base_Color=n.mix(lines, rubbed, (0.24, 0.19, 0.14)), Metallic=0.9, Roughness=rough,
            Normal=n.bump(brush, 0.016, 0.8))
    return n.mat


def anodised_knob(name, colour, rough, speck=0.016):
    n = Nodes(name)
    p = n.object_position()
    grit = n.noise(p, 1.7, 2)
    n.shade(Base_Color=colour, Metallic=0.75, Roughness=n.remap(grit, 0.25, 0.75, rough - 0.05, rough + 0.05),
            Normal=n.bump(grit, speck, 0.8))
    return n.mat


def print_ink(wear_image, bounds):
    """Screen print: uneven laydown, pinholes, ragged edges, and thinning where the plate is rubbed."""
    n = Nodes("print_ink")
    p = n.position()
    tone = n.remap(n.noise(p, 0.5, 3), 0.3, 0.7, 0, 1)
    ink = n.mix(tone, (0.80, 0.75, 0.63), (0.90, 0.85, 0.73))
    pinholes = n.remap(n.noise(p, 4.5, 3, 0.7), 0.64, 0.71, 0, 1)
    wear = n.add("ShaderNodeSeparateColor", {"Color": wear_lookup(n, wear_image, bounds)}).outputs["Red"]
    thin = n.math("MULTIPLY", n.remap(n.noise(p, 1.4, 4, 0.7), 0.42, 0.62, 0, 1), wear)
    gone = n.math("MINIMUM", n.math("ADD", pinholes, thin), 0.85)
    n.shade(Base_Color=n.mix(gone, ink, (0.17, 0.135, 0.10)), Roughness=0.62)
    return n.mat


def enamel():
    n = Nodes("oxide_enamel")
    tone = n.remap(n.noise(n.position(), 0.5, 4), 0.3, 0.7, 0, 1)
    n.shade(Base_Color=n.mix(tone, (0.115, 0.014, 0.008), (0.15, 0.021, 0.012)),
            Roughness=n.remap(n.noise(n.position(), 0.2, 5, 0.7), 0.3, 0.7, 0.17, 0.30),
            Coat_Weight=0.5, Coat_Roughness=0.08)
    return n.mat


def cloth():
    """Basket weave: threads of uneven tone crossing over and under."""
    n = Nodes("grille_cloth")
    parts = n.add("ShaderNodeSeparateXYZ", {"Vector": n.position()}).outputs
    pitch = 3.14159265 / 1.7
    sx = n.math("SINE", n.math("MULTIPLY", parts["X"], pitch))
    sz = n.math("SINE", n.math("MULTIPLY", parts["Z"], pitch))
    over = n.math("GREATER_THAN", n.math("MULTIPLY", sx, sz), 0.0)
    warp = n.math("MULTIPLY", n.math("ABSOLUTE", sx), over)
    weft = n.math("MULTIPLY", n.math("ABSOLUTE", sz), n.math("SUBTRACT", 1.0, over))
    weave = n.math("ADD", warp, weft)
    slub = n.remap(n.noise(n.position((400, 1000, 400)), 1.0, 2), 0.3, 0.7, 0.45, 1.0)
    n.shade(Base_Color=n.mix(n.math("MULTIPLY", weave, slub), (0.006, 0.006, 0.006), (0.16, 0.145, 0.125)),
            Roughness=0.85, Normal=n.bump(weave, 0.35))
    return n.mat


def plaster(depth=820.0):
    """Dark limewash, out of focus. It calms towards the top of frame, where the utility bar starts."""
    n = Nodes("plaster")
    p = n.position()
    broad = n.noise(p, 0.0042, 7, 0.68)
    trowel = n.noise(n.position((1000 * 0.5, 1000, 1000 * 1.3)), 0.015, 4, 0.7)
    fine = n.noise(p, 0.07, 5, 0.65)
    mottle = n.math("ADD", n.math("MULTIPLY", broad, 0.65), n.math("MULTIPLY", trowel, 0.35))
    low, high = (L.screen_to_world(0, y, depth)[2] for y in (70, 2))
    height = n.add("ShaderNodeSeparateXYZ", {"Vector": p}).outputs["Z"]
    calm = n.remap(height, low, high, 1.0, 0.0)
    tone = n.math("ADD", n.math("MULTIPLY", n.math("SUBTRACT", mottle, 0.5), calm), 0.5)
    relief = n.math("ADD", tone, n.math("MULTIPLY", n.math("MULTIPLY", fine, 0.2), calm))
    # along the top the key's fall-off is taken out, because the bar above is one flat tone
    axes = n.add("ShaderNodeSeparateXYZ", {"Vector": p}).outputs
    left, right = (L.screen_to_world(x, 0, depth)[0] for x in (200, 1280))
    even = n.remap(axes["X"], left, right, 0.79, 1.71)
    near_top, further = (L.screen_to_world(0, y, depth)[2] for y in (40, 260))
    amount = n.remap(axes["Z"], near_top, further, 1.0, 0.3)
    gain = n.math("ADD", n.math("MULTIPLY", n.math("SUBTRACT", even, 1.0), amount), 1.0)
    colour = n.mix(n.remap(tone, 0.34, 0.68, 0, 1), (0.016, 0.014, 0.013), (0.042, 0.036, 0.031))
    colour = n.add("ShaderNodeVectorMath", {0: colour, "Scale": gain}, operation="SCALE").outputs["Vector"]
    n.shade(Base_Color=colour,
            Roughness=0.8, Normal=n.bump(relief, 9.0, 0.8))
    return n.mat


def glass(name="tube_glass", roughness=0.05):
    """Thin glass without refraction (so the inside stays lit, no caustics), with real reflections."""
    n = Nodes(name)
    clear = n.add("ShaderNodeBsdfTransparent", {"Color": (0.90, 0.89, 0.86)})
    shine = n.add("ShaderNodeBsdfGlossy", {"Roughness": roughness})
    rim = n.add("ShaderNodeLayerWeight", {"Blend": 0.30})
    amount = n.math("MINIMUM", n.math("ADD", n.math("MULTIPLY", rim.outputs["Fresnel"], 1.4), 0.07), 0.75)
    mixed = n.add("ShaderNodeMixShader", {0: amount})
    n.tree.links.new(clear.outputs[0], mixed.inputs[1])
    n.tree.links.new(shine.outputs[0], mixed.inputs[2])
    n.tree.links.new(mixed.outputs[0], n.out.inputs["Surface"])
    return n.mat


def getter():
    """The silvered flash inside the dome: a mirror that thins out towards its lower edge."""
    n = Nodes("getter")
    coord = n.add("ShaderNodeTexCoord").outputs["Object"]
    z = n.add("ShaderNodeSeparateXYZ", {"Vector": coord}).outputs["Z"]
    cover = n.remap(n.math("ADD", z, n.math("MULTIPLY", n.noise(coord, 900.0, 3), 0.004)), 0.078, 0.086, 0.0, 1.0)
    clear = n.add("ShaderNodeBsdfTransparent")
    metal = n.add("ShaderNodeBsdfMetallic", {"Base Color": (0.30, 0.29, 0.27, 1), "Roughness": 0.1})
    mixed = n.add("ShaderNodeMixShader", {0: cover})
    n.tree.links.new(clear.outputs[0], mixed.inputs[1])
    n.tree.links.new(metal.outputs[0], mixed.inputs[2])
    n.tree.links.new(mixed.outputs[0], n.out.inputs["Surface"])
    return n.mat


def lamp(name, colour, hot, off_colour, strength):
    """Coloured lens. The value node `lit` switches it between dark and glowing.

    Strength stays low enough that the view transform keeps the colour.
    """
    n = Nodes("lamp_" + name)
    lit = n.add("ShaderNodeValue", name="lit")
    lit.outputs[0].default_value = 0.0
    normal = n.add("ShaderNodeNewGeometry").outputs["Normal"]
    centre = n.add("ShaderNodeVectorMath", {0: normal, 1: (0.0, -1.0, 0.0)}, operation="DOT_PRODUCT").outputs["Value"]
    side = n.add("ShaderNodeVectorMath", {0: normal, 1: (0.62, -0.55, -0.56)}, operation="DOT_PRODUCT").outputs["Value"]
    middle = n.remap(centre, 0.45, 1.0, 0.0, 1.0)
    facet = n.remap(side, -0.1, 0.95, 0.30, 1.0)
    glow = n.math("MULTIPLY", n.math("MULTIPLY", facet, strength), lit.outputs[0])
    n.shade(Base_Color=off_colour, Roughness=0.13, Specular_IOR_Level=0.3,
            Emission_Color=n.mix(middle, colour, hot), Emission_Strength=glow)
    return n.mat


def emitter(name, colour, strength):
    n = Nodes(name)
    lit = n.add("ShaderNodeValue", name="lit")
    lit.outputs[0].default_value = 0.0
    n.shade(Base_Color=(0.02, 0.01, 0.005), Emission_Color=colour,
            Emission_Strength=n.math("MULTIPLY", lit.outputs[0], strength))
    return n.mat


def phenolic():
    """Moulded black phenolic: a hard, slightly orange-peel gloss, worn matt where thumbs land."""
    n = Nodes("rocker_phenolic")
    p = n.object_position()
    peel = n.noise(p, 1.3, 3)
    n.shade(Base_Color=(0.045, 0.042, 0.039), Roughness=n.remap(n.noise(p, 0.12, 2), 0.3, 0.7, 0.2, 0.3),
            Specular_IOR_Level=0.55, Coat_Weight=0.35, Coat_Roughness=0.1, Normal=n.bump(peel, 0.012, 0.5))
    return n.mat


def library():
    wear_image, bounds = panel_wear()
    return dict(
        tolex=tolex(),
        cap_steel=cap_steel(),
        piping=simple("piping", (0.009, 0.009, 0.009), 0.27),
        strap=powder("strap", (0.006, 0.006, 0.006), 0.43),
        rubber=simple("rubber", (0.006, 0.006, 0.006), 0.6),
        cloth=cloth(),
        wall=plaster(),
        grille=powder("grille_steel", (0.016, 0.0158, 0.016), 0.31, dust=True),
        bar=powder("grille_bar", (0.007, 0.007, 0.008), 0.3),
        trim=simple("trim", (0.07, 0.056, 0.044), 0.25, 0.9),
        panel_edge=simple("panel_edge", (0.36, 0.28, 0.20), 0.26, 1.0),
        handle_metal=simple("handle_metal", (0.075, 0.072, 0.068), 0.36, 0.9),
        thread=simple("thread", (0.06, 0.055, 0.05), 0.8),
        cable=simple("cable", (0.006, 0.006, 0.006), 0.43),
        panel=anodised_panel(wear_image, bounds),
        groove=simple("groove", (0.006, 0.005, 0.004), 0.5),
        ink=print_ink(wear_image, bounds),
        knob=anodised_knob("knob_body", (0.024, 0.0233, 0.0226), 0.31),
        knob_top=anodised_knob("knob_top", (0.040, 0.039, 0.038), 0.40, 0.022),
        knob_worn=anodised_knob("knob_knurl_crown", (0.085, 0.080, 0.075), 0.30),
        rocker=phenolic(),
        bezel_steel=simple("bezel_steel", (0.50, 0.48, 0.45), 0.24, 1.0),
        button_cap=simple("button_cap", (0.20, 0.185, 0.165), 0.33, 0.6),
        flag=simple("flag", (0.92, 0.86, 0.72), 0.45, Emission_Color=(1.0, 0.92, 0.76), Emission_Strength=0.10),
        pointer=simple("pointer", (0.88, 0.83, 0.72), 0.5),
        bright_cut=simple("bright_cut", (0.74, 0.72, 0.69), 0.20, 1.0),
        nickel=simple("nickel", (0.56, 0.53, 0.49), 0.27, 1.0),
        dark_nickel=simple("dark_nickel", (0.20, 0.19, 0.18), 0.3, 1.0),
        screw=simple("screw_black", (0.035, 0.034, 0.033), 0.3, 0.7),
        recess=simple("recess", (0.002, 0.002, 0.002), 0.7),
        plastic=simple("plastic", (0.008, 0.008, 0.008), 0.26),
        satin_plastic=simple("satin_plastic", (0.011, 0.011, 0.011), 0.40),
        enamel=enamel(),
        cream=simple("cream_enamel", (0.80, 0.70, 0.52), 0.3),
        glass=glass(),
        small_glass=glass("small_glass", 0.3),
        getter=getter(),
        plate=simple("plate", (0.16, 0.14, 0.125), 0.62, 0.3),
        mica=simple("mica", (0.22, 0.18, 0.14), 0.5),
        chassis=simple("chassis", (0.22, 0.21, 0.20), 0.42, 0.9),
        laminations=simple("laminations", (0.07, 0.064, 0.056), 0.55, 0.4),
        can=simple("shield_can", (0.42, 0.40, 0.37), 0.32, 1.0),
        interior=simple("interior", (0.05, 0.046, 0.042), 0.7),
        heater=emitter("heater", (1.0, 0.17, 0.015), 1.5),
        plate_glow=emitter("plate_glow", (1.0, 0.15, 0.015), 0.42),
        green=lamp("green", (0.02, 1.0, 0.12), (0.02, 0.9, 0.06), (0.004, 0.045, 0.008), 0.42),
        red=lamp("red", (1.0, 0.0, 0.0), (1.0, 0.008, 0.0), (0.07, 0.001, 0.001), 1.1),
        amber=lamp("amber", (1.0, 0.12, 0.0), (1.0, 0.24, 0.0), (0.12, 0.028, 0.0), 0.9),
        power=lamp("power", (0.9, 0.0, 0.0), (1.0, 0.006, 0.0), (0.09, 0.001, 0.001), 1.05),
    )
