"""Materials of the section scenes that the head does not already have."""
from materials import Nodes


def hammertone(name, dark, light, flake=(0.55, 0.42, 0.33)):
    """Hammered enamel: soft dimples, a sheen that breaks up, fine metal flake in the highlights."""
    n = Nodes(name)
    p = n.object_position()
    dimple = n.add("ShaderNodeTexVoronoi", {"Vector": p, "Scale": 0.55}, feature="F1").outputs["Distance"]
    rough = n.noise(p, 0.08, 4, 0.6)
    colour = n.mix(n.remap(rough, 0.3, 0.7, 0, 1), dark, light)
    flakes = n.remap(n.noise(p, 9.0, 2, 0.5), 0.70, 0.76, 0, 1)
    n.shade(Base_Color=colour, Roughness=n.remap(rough, 0.3, 0.7, 0.26, 0.40), Coat_Weight=0.35,
            Coat_Roughness=0.22, Metallic=n.math("MULTIPLY", flakes, 0.6),
            Normal=n.bump(n.math("ADD", dimple, n.math("MULTIPLY", rough, 0.3)), 0.35, 0.55))
    return n.mat


def chipped(name, paint, metal=(0.55, 0.53, 0.5), wear_attr="wear"):
    """Paint that is worn through to the casting where the attribute says so."""
    n = Nodes(name)
    p = n.object_position()
    wear = n.math("MULTIPLY", n.attribute(wear_attr), n.remap(n.noise(p, 1.3, 5, 0.75), 0.52, 0.6, 0, 1))
    n.shade(Base_Color=n.mix(wear, paint, metal), Metallic=wear, Roughness=n.remap(wear, 0, 1, 0.35, 0.3))
    return n.mat


def wood_floor():
    """Dark stained stage boards: planks of uneven tone, long grain, satin worn duller in places."""
    n = Nodes("stage_floor")
    p = n.position()
    along = n.add("ShaderNodeVectorRotate", {"Vector": p, "Angle": 1.5707963}, rotation_type="Z_AXIS").outputs[0]
    planks = n.add("ShaderNodeTexBrick", {"Vector": along, "Color1": (1, 1, 1), "Color2": (0.55, 0.55, 0.55),
                                          "Mortar": (0, 0, 0), "Scale": 1.0, "Mortar Size": 0.8,
                                          "Mortar Smooth": 0.3, "Brick Width": 6000.0, "Row Height": 96.0,
                                          "Bias": 0.0}, offset=0.37)
    tone = n.add("ShaderNodeSeparateColor", {"Color": planks.outputs["Color"]}).outputs["Red"]
    grain = n.noise(n.position((1000 * 2.2, 1000 * 0.06, 1000)), 1.0, 6, 0.65)
    rings = n.math("SINE", n.math("MULTIPLY", n.add("ShaderNodeSeparateXYZ", {"Vector": n.position((1000 * 0.5, 1000 * 0.02, 1000))}).outputs["X"], 6.0))
    figure = n.math("ADD", n.math("MULTIPLY", grain, 0.7), n.math("MULTIPLY", rings, 0.08))
    base = n.mix(n.remap(figure, 0.3, 0.75, 0, 1), (0.018, 0.009, 0.004), (0.046, 0.024, 0.011))
    base = n.mix(n.remap(tone, 0.55, 1.0, 0.25, 0.0), base, (0.010, 0.006, 0.004))
    scuff = n.remap(n.noise(p, 0.004, 5, 0.7), 0.45, 0.7, 0, 1)
    gap = n.math("LESS_THAN", n.add("ShaderNodeSeparateColor", {"Color": planks.outputs["Color"]}).outputs["Red"], 0.05)
    n.shade(Base_Color=n.mix(gap, base, (0.002, 0.0015, 0.001)),
            Roughness=n.remap(scuff, 0, 1, 0.42, 0.68),
            Normal=n.bump(n.math("ADD", figure, n.math("MULTIPLY", gap, -2.0)), 0.25, 0.6))
    return n.mat


def anodised(name, colour, rough=0.32, brush_scale=(0.012, 1.0, 5.0)):
    n = Nodes(name)
    hair = n.noise(n.position(tuple(1000 * s for s in brush_scale)), 1.0, 3, 0.6)
    n.shade(Base_Color=n.mix(n.remap(hair, 0.35, 0.65, 0, 1), colour, tuple(c * 1.35 for c in colour)),
            Metallic=0.9, Roughness=n.remap(hair, 0.35, 0.65, rough - 0.05, rough + 0.06), Normal=n.bump(hair, 0.012, 0.7))
    return n.mat


def glass_window(name="display_glass"):
    """Smoked glass over a display: dark, glossy."""
    n = Nodes(name)
    n.shade(Base_Color=(0.004, 0.004, 0.005), Roughness=0.10, Specular_IOR_Level=0.45, Coat_Weight=0.5, Coat_Roughness=0.16)
    return n.mat


def add(mats):
    from materials import simple
    mats.update(
        cinder_paint=hammertone("cinder_paint", (0.105, 0.030, 0.018), (0.15, 0.046, 0.028)),
        casting=simple("casting", (0.42, 0.40, 0.37), 0.42, 1.0),
        floor=wood_floor(),
        rack_black=anodised("rack_black", (0.016, 0.0155, 0.015), 0.34),
        rack_rail=simple("rack_rail", (0.30, 0.29, 0.28), 0.34, 1.0),
        eq_panel=anodised("eq_panel", (0.062, 0.058, 0.054), 0.34),
        display_glass=glass_window(),
        black_plastic=simple("black_plastic", (0.012, 0.012, 0.012), 0.35),
        chrome=simple("chrome", (0.8, 0.79, 0.77), 0.08, 1.0),
        led_ember=None,
    )
    mats.pop("led_ember")
    return mats
