"""What the head stands on and in front of: its speaker cabinet, a lead, a wall."""
import math

import numpy as np

import layout as L
import meshes as M
from parts import cabinet

CAB_FRONT = -40.0
CAB_WIDTH = 672.0


def cab_box():
    _, _, z0, *_ = cabinet.body_rect()
    top = z0 - 12.4
    return cabinet.Box("cab", -CAB_WIDTH / 2, CAB_WIDTH / 2, top - 120, top, CAB_FRONT, 356, 18.5, 14.5, 0.52)


def cab(b, tex):
    box = cab_box()
    cabinet.shell(b, tex, box)
    cabinet.piping(b, box)
    cabinet.corner_caps(b, box, ((-1, 1), (1, 1)))
    M.box("cab_cloth", (0, box.front + 12, (box.z0 + box.z1) / 2),
          (CAB_WIDTH - 2 * box.band_x + 3, 2, box.z1 - box.z0 - 2 * box.band_z + 3), b.mats["cloth"], b.static)


def lead(b):
    """An instrument lead in the jack: angled plug, then out of the picture to the left."""
    jx, _, jz = L.screen_to_world(*L.JACK)
    m = b.mats
    barrel = M.lathe("plug_tip", [(4.9, 4.5), (4.9, 9.0)], m["nickel"], b.static)
    M.place(barrel, (jx, 0, jz), M.UP_IS_FRONT)
    nose = [(5.4, 8.2), (6.6, 8.6), (6.9, 9.4), (6.9, 19.6), (6.7, 20.8), (6.0, 21.5), (0.1, 21.6)]
    M.place(M.lathe("plug_head", nose, m["satin_plastic"], b.static), (jx, 0, jz), M.UP_IS_FRONT)
    y = -15.5
    shoulder = [(jx, y, jz + 2), (jx, y, jz - 8), (jx - 0.2, y, jz - 21), (jx - 0.5, y - 0.2, jz - 30)]
    M.tube("plug_body", M.spline(shoulder, 8), 6.2, m["satin_plastic"], b.static, 40)
    relief = M.spline([(jx - 0.4, y - 0.1, jz - 28), (jx - 1.2, y - 2, jz - 37), (jx - 3, y - 8, jz - 46)], 24)
    ridges = np.sin(np.linspace(0, 9 * math.pi, len(relief))) ** 2
    M.tube("plug_relief", relief, np.linspace(5.0, 3.7, len(relief)) - 0.6 * ridges, m["rubber"], b.static, 32)
    top = cab_box().z1
    run = [(jx - 1.0, y - 2, jz - 35), (jx - 3.2, y - 8, jz - 47), (jx - 8.5, -32, jz - 64),
           (jx - 18, -46, top - 5), (jx - 27, -50, top - 34), (jx - 36, -50, top - 125)]
    M.tube("lead", M.spline(run, 32), 3.1, m["cable"], b.static, 28)


def wall(b):
    M.box("wall", (0, 820, 100), (3600, 4, 2000), b.mats["wall"], b.static)


def build(b, tex):
    cab(b, tex)
    lead(b)
    wall(b)
