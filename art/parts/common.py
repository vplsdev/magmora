"""What every part builder is handed, plus hardware that several of them use."""
from dataclasses import dataclass

import numpy as np

import layout as L
import meshes as M


@dataclass
class Build:
    mats: dict
    fonts: dict
    static: object
    cache: str
    quick: bool = False


def screen_mm(points):
    return points / L.PT_PER_MM


def screw(b, name, xyz, radius=2.6, height=1.3, angle=0.0, material="screw", matrix=None):
    """Pan-head Phillips screw, local +Z out of the surface."""
    rings, segs = 34, 72
    rho = np.linspace(1, 0.001, rings)
    z = height * np.sqrt(np.clip(1 - (rho * 0.97) ** 4, 0, 1))
    rho = np.concatenate(([1.0], rho)) * radius
    z = np.concatenate(([0.0], z))
    th = np.linspace(0, 2 * np.pi, segs, endpoint=False)
    x, y = np.outer(rho, np.cos(th)), np.outer(rho, np.sin(th))
    cx = x * np.cos(angle) + y * np.sin(angle)
    cy = -x * np.sin(angle) + y * np.cos(angle)
    arm, half = radius * 0.66, radius * 0.15
    d = np.minimum(np.maximum(np.abs(cx) / arm, np.abs(cy) / half),
                   np.maximum(np.abs(cy) / arm, np.abs(cx) / half))
    dip = np.clip((1.25 - d) / 0.45, 0, 1) * np.clip(1.1 - np.hypot(x, y) / arm * 0.5, 0, 1)
    h = z[:, None] - dip * height * 0.85
    obj = M.grid(name, np.stack((x, y, h), -1), b.mats[material], b.static, closed=True)
    obj.data.set_sharp_from_angle(angle=0.85)
    return M.place(obj, xyz, M.UP_IS_FRONT if matrix is None else matrix)
