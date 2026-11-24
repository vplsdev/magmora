"""Camera, studio lights and render settings."""
import math

import bpy
from mathutils import Vector

import layout as L
import meshes as M


def camera(coll):
    data = bpy.data.cameras.new("Camera")
    data.lens, data.sensor_width = L.LENS_MM, L.SENSOR_MM
    data.sensor_fit = "HORIZONTAL"
    data.shift_y = -L.camera_height() / L.VIEW_W_MM
    data.clip_start, data.clip_end = 0.1, 20
    data.dof.use_dof = True
    data.dof.focus_distance = L.CAMERA_DIST * M.MM
    data.dof.aperture_fstop = 6.3
    obj = bpy.data.objects.new("Camera", data)
    coll.objects.link(obj)
    obj.location = (0, -L.CAMERA_DIST * M.MM, L.camera_height() * M.MM)
    obj.rotation_euler = (math.pi / 2, 0, 0)
    bpy.context.scene.camera = obj


def area(coll, name, position, target, size, watts, colour=(1, 1, 1)):
    data = bpy.data.lights.new(name, "AREA")
    data.shape = "RECTANGLE"
    data.size, data.size_y = size
    data.energy = watts
    data.color = colour
    obj = bpy.data.objects.new(name, data)
    coll.objects.link(obj)
    obj.location = position
    obj.rotation_euler = (Vector(target) - Vector(position)).to_track_quat("-Z", "Y").to_euler()
    obj.visible_camera = False
    return obj


def only_lights(light, receivers, name):
    linked = bpy.data.collections.new(name)
    for obj in receivers:
        linked.objects.link(obj)
    light.light_linking.receiver_collection = linked


def lights(coll):
    area(coll, "key", (-0.62, -1.05, 0.82), (-0.05, 0, 0), (0.85, 0.6), 18, (1.0, 0.93, 0.84))
    area(coll, "fill", (1.1, -1.0, 0.15), (0.1, 0, 0), (0.45, 0.9), 4.6, (0.76, 0.86, 1.0))
    area(coll, "rim", (0.1, 0.55, 0.75), (0, 0.05, 0.15), (1.3, 0.12), 9, (1.0, 0.95, 0.9))
    area(coll, "top_strip", (0.05, -0.3, 0.95), (0, -0.02, 0), (1.5, 0.12), 4, (1.0, 0.97, 0.93))
    area(coll, "bounce", (0.0, -1.0, -0.65), (0, 0, -0.05), (1.8, 0.3), 2.4, (1.0, 0.9, 0.8))
    for side, watts in ((-1, 1.6), (1, 1.5)):       # kickers that find the radii of the sides
        area(coll, f"kicker{side}", (side * 1.0, 0.3, 0.05), (side * 0.32, 0, 0), (0.1, 0.7), watts,
             (1.0, 0.9, 0.78) if side < 0 else (0.8, 0.88, 1.0))
    area(coll, "wall_wash", (-0.52, 0.30, 0.46), (-0.16, 0.82, 0.14), (0.22, 0.22), 2.6, (1.0, 0.86, 0.70))

    # the soft card a photographer puts up so that brushed metal has something to reflect
    sheen = area(coll, "panel_sheen", (-0.38, -1.4, -0.40), (-0.02, 0, -0.07), (1.5, 0.3), 3.3, (1.0, 0.92, 0.82))
    metal = [o for o in bpy.data.objects if o.type == "MESH" and (
        o.name in ("panel", "trim")
        or o.name.startswith(("panel_cup", "panel_screw", "legend_", "scale_", "rule_", "leader_")))]
    sheen.data.use_shadow = False      # shines from below: its shadows would point up
    only_lights(sheen, metal, "SheenReceivers")
    lift = area(coll, "panel_lift", (0.46, -1.2, -0.36), (0.25, 0, -0.07), (0.34, 0.3), 0.085, (1.0, 0.9, 0.78))
    lift.data.spread = 1.6
    lift.data.use_shadow = False
    only_lights(lift, metal, "LiftReceivers")
    low = area(coll, "cab_front", (0.0, -1.1, -0.55), (0, -0.04, -0.2), (1.4, 0.25), 1.6, (1.0, 0.93, 0.85))
    low.data.use_shadow = False
    only_lights(low, [o for o in bpy.data.objects if o.name in ("cab_cloth", "cab_piping")], "CabReceivers")
    grazing = area(coll, "grille_sheen", (-0.75, -0.75, 0.05), (-0.2, 0, 0.05), (0.5, 0.5), 3.2, (1.0, 0.93, 0.85))
    only_lights(grazing, [bpy.data.objects["grille"]], "GrilleReceivers")


def world():
    w = bpy.data.worlds.new("World")
    w.use_nodes = True
    background = w.node_tree.nodes["Background"]
    background.inputs[0].default_value = (0.9, 0.82, 0.74, 1)
    background.inputs[1].default_value = 0.025
    bpy.context.scene.world = w


def render_settings(scale, samples):
    s = bpy.context.scene
    s.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    try:
        prefs.compute_device_type = "METAL"
        prefs.get_devices()
        for device in prefs.devices:
            device.use = device.type == "METAL"
        s.cycles.device = "GPU"
    except TypeError:
        s.cycles.device = "CPU"
    s.cycles.samples = samples
    s.cycles.use_adaptive_sampling = True
    s.cycles.adaptive_threshold = 0.008
    s.cycles.use_denoising = True
    s.cycles.denoiser = "OPENIMAGEDENOISE"
    s.cycles.seed = 1
    s.cycles.use_animated_seed = False
    s.cycles.max_bounces = 8
    s.cycles.transparent_max_bounces = 12
    s.cycles.sample_clamp_indirect = 6
    s.cycles.caustics_reflective = s.cycles.caustics_refractive = False
    s.render.resolution_x, s.render.resolution_y = L.RENDER_W * scale, L.RENDER_H * scale
    s.render.resolution_percentage = 100
    s.render.image_settings.file_format = "PNG"
    s.render.image_settings.color_mode = "RGB"
    s.render.image_settings.color_depth = "8"
    s.view_settings.view_transform = "AgX"
    s.view_settings.look = "AgX - Medium High Contrast"
    s.view_settings.exposure = 0.0
    s.render.dither_intensity = 1.0
