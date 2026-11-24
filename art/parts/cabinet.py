"""The covered wooden box: tolex, piping, corner caps, handle, feet."""
import math
import os
from dataclasses import dataclass

import numpy as np
from mathutils import Matrix

import layout as L
import meshes as M
import textures
from parts.common import screw

FRONT = -14.0          # front face of the frame, mm behind (+) or before (-) the panel
DEPTH = 276.0
EDGE_R = 8.0
INNER_R = 4.5
CORNER_R = 13.0
GRAIN_MM = 0.25


@dataclass(frozen=True)
class Box:
    name: str
    x0: float
    x1: float
    z0: float
    z1: float
    front: float
    depth: float
    band_x: float
    band_z: float
    step: float = 0.4


def body_rect():
    x0, _, z1 = L.screen_to_world(L.HEAD[0], L.HEAD[1], FRONT)
    x1, _, z0 = L.screen_to_world(L.HEAD[2], L.HEAD[3], FRONT)
    ix0, _, iz1 = L.screen_to_world(L.OPENING[0], L.OPENING[1], FRONT)
    return x0, x1, z0, z1, ix0 - x0, z1 - iz1


def head_box():
    x0, x1, z0, z1, band_x, band_z = body_rect()
    return Box("head", x0, x1, z0, z1, FRONT, DEPTH, band_x, band_z)


def _grain(cache):
    path = os.path.join(cache, "tolex_v2.npy")
    if os.path.exists(path):
        return np.load(path)
    tex = textures.tolex()
    np.save(path, tex)
    return tex


def grain_at(tex, points, normals):
    """Projected from three sides, so it neither stretches nor breaks on the radii."""
    weight = np.abs(normals) ** 6
    weight /= weight.sum(-1, keepdims=True)
    x, y, z = (points[..., i] for i in range(3))
    out = np.zeros(x.shape, np.float32)
    for axis, (u, v, shift) in enumerate(((y, z, 37.0), (x, z, 0.0), (x, y, 71.0))):
        w = weight[..., axis]
        use = w > 0.002
        if use.any():
            out[use] += w[use] * textures.sample(tex, u[use] + shift, v[use] + shift, textures.TOLEX_TILE_MM)
    return out


def profile(box, step):
    """Frame section from the back along the outside, over the front, into the opening.

    Columns: inset from the outline, depth, then the normal in (outward, depth).
    """
    f, band, back = box.front, box.band_x, box.front + box.depth
    rows = []
    for a in np.linspace(math.pi / 2, 0, 10, endpoint=False):          # rounded back edge
        rows.append((EDGE_R - EDGE_R * math.cos(a), back - EDGE_R + EDGE_R * math.sin(a),
                     math.cos(a), math.sin(a)))
    rows += [(0, d, 1, 0) for d in np.arange(back - EDGE_R, f + 70, -step * 3)]
    rows += [(0, d, 1, 0) for d in np.arange(f + 70, f + EDGE_R, -step * 1.5)]
    for a in np.linspace(0, math.pi / 2, int(math.pi / 2 * EDGE_R / step), endpoint=False):
        rows.append((EDGE_R - EDGE_R * math.cos(a), f + EDGE_R - EDGE_R * math.sin(a),
                     math.cos(a), -math.sin(a)))
    rows += [(i, f, 0, -1) for i in np.arange(EDGE_R, band - INNER_R, step)]
    for a in np.linspace(0, math.pi / 2, int(math.pi / 2 * INNER_R / step), endpoint=False):
        rows.append((band - INNER_R + INNER_R * math.sin(a), f + INNER_R - INNER_R * math.cos(a),
                     -math.sin(a), -math.cos(a)))
    rows += [(band, d, -1, 0) for d in np.arange(f + INNER_R, f + 32, step)]
    return np.array(rows, float)


def loop(x0, x1, z0, z1, inset_x, inset_z, step):
    """Rounded rectangle at one inset, with a point count that does not depend on the inset."""
    r = max(CORNER_R - inset_x, 1.2)
    nx = int((x1 - x0 - 2 * CORNER_R) / step)
    nz = int((z1 - z0 - 2 * CORNER_R) / step)
    nc = max(int(math.pi / 2 * CORNER_R / step), 8)
    ax, bx, az, bz = x0 + inset_x, x1 - inset_x, z0 + inset_z, z1 - inset_z
    parts = []

    def line(p, q, n, normal):
        s = np.linspace(0, 1, n, endpoint=False)
        parts.append((p[0] + (q[0] - p[0]) * s, p[1] + (q[1] - p[1]) * s,
                      np.full(n, normal[0]), np.full(n, normal[1])))

    def arc(cx, cz, start):
        a = np.radians(start) + np.linspace(0, math.pi / 2, nc, endpoint=False)
        parts.append((cx + r * np.cos(a), cz + r * np.sin(a), np.cos(a), np.sin(a)))

    line((ax + r, az), (bx - r, az), nx, (0, -1))
    arc(bx - r, az + r, 270)
    line((bx, az + r), (bx, bz - r), nz, (1, 0))
    arc(bx - r, bz - r, 0)
    line((bx - r, bz), (ax + r, bz), nx, (0, 1))
    arc(ax + r, bz - r, 90)
    line((ax, bz - r), (ax, az + r), nz, (-1, 0))
    arc(ax + r, az + r, 180)
    return [np.concatenate(v) for v in zip(*parts)]


def shell(b, tex, box):
    x0, x1, z0, z1, band_x, band_z = box.x0, box.x1, box.z0, box.z1, box.band_x, box.band_z
    step = 1.4 if b.quick else box.step
    points, normals = [], []
    for inset, depth, out, forward in profile(box, step):
        # the top and bottom bands are a little narrower than the sides
        inset_z = inset if inset <= EDGE_R else EDGE_R + (inset - EDGE_R) * (band_z - EDGE_R) / (band_x - EDGE_R)
        x, z, nx, nz = loop(x0, x1, z0, z1, inset, inset_z, step)
        points.append(np.stack((x, np.full_like(x, depth), z), -1))
        normals.append(np.stack((nx * out, np.full_like(x, forward), nz * out), -1))
    p, n = np.array(points), np.array(normals)
    h = grain_at(tex, p + (0, 0, 19.0), n)
    # vinyl pulled over a board: a slight slow waviness, and a butt seam under the bottom edge
    wave = 0.10 * np.sin(p[..., 0] * 0.047 + 1.0) * np.sin(p[..., 0] * 0.013 + p[..., 2] * 0.05)
    seam = np.exp(-((p[..., 0] - 96.0) / 0.5) ** 2) * (p[..., 2] < z0 + band_z + 1)
    p = p + n * ((h - 0.6) * GRAIN_MM + wave - 0.3 * seam)[..., None]
    # vinyl is buffed where it is handled: along the radii
    worn = np.clip(1 - np.abs(np.abs(n[..., 1]) - 0.6) / 0.35, 0, 1)
    M.grid(box.name + "_shell", p, b.mats["tolex"], b.static, closed=True,
           attrs={"grain": h * (1 - 0.8 * seam), "worn": worn})
    M.box(box.name + "_back", (0, box.front + box.depth - 12, (z0 + z1) / 2),
          (x1 - x0 - 20, 4, z1 - z0 - 20), b.mats["interior"], b.static)


def piping(b, box):
    """Welting round the opening."""
    cx, cz = (box.x0 + box.x1) / 2, (box.z0 + box.z1) / 2
    outline = M.rounded_rect(box.x1 - box.x0 - 2 * box.band_x + 1.4,
                             box.z1 - box.z0 - 2 * box.band_z + 1.4, 2.4, 10)
    outline = outline + outline[:2]
    M.tube(box.name + "_piping", [(cx + x, box.front + INNER_R * 0.75, cz + z) for x, z in outline],
           1.45, b.mats["piping"], b.static, 20)


def _surface(box, sx, sz, d_arc, across):
    """Point and normal on the box beside one corner, in (along the outline, over the profile)."""
    x0, x1, z0, z1, f = box.x0, box.x1, box.z0, box.z1, box.front
    quarter = EDGE_R * math.pi / 2
    d = np.asarray(d_arc, float)
    side, face = d < 0, d >= quarter
    angle = np.clip(d / EDGE_R, 0, math.pi / 2)
    inset = np.where(face, np.minimum(EDGE_R + d - quarter, box.band_z - 0.5), EDGE_R - EDGE_R * np.cos(angle))
    inset = np.where(side, 0.0, inset)
    depth = np.where(side, f + EDGE_R - d, f + EDGE_R - EDGE_R * np.sin(angle))
    depth = np.where(face, f, depth)
    out, forward = np.cos(angle), -np.sin(angle)

    r = np.maximum(CORNER_R - inset, 1.2)
    half = math.pi / 4
    s = np.asarray(across, float)
    a = half + np.clip(s / CORNER_R, -half, half)
    along = np.sign(s) * np.maximum(np.abs(s) - CORNER_R * half, 0)
    cx = (x1 if sx > 0 else x0) - sx * (inset + r)
    cz = (z1 if sz > 0 else z0) - sz * (inset + r)
    x = cx + sx * r * np.cos(a) - sx * np.maximum(along, 0)
    z = cz + sz * r * np.sin(a) - sz * np.maximum(-along, 0)
    n = np.stack((sx * np.cos(a) * out, forward, sz * np.sin(a) * out), -1)
    return np.stack((x, depth, z), -1), n


def corner_caps(b, box, corners=((-1, -1), (-1, 1), (1, -1), (1, 1))):
    """Thin pressed steel lying on the vinyl: a boss over the corner, three ears, a rolled rim."""
    quarter = EDGE_R * math.pi / 2
    front_end = quarter + box.band_z - EDGE_R - 2.6
    back_end = -27.0
    for sx, sz in corners:
        v = np.linspace(0, 1, 130)[:, None]
        u = np.linspace(-1, 1, 200)[None, :]
        d = back_end + (front_end - back_end) * v
        # widest over the edge, drawn in towards the ends of the ears
        behind = np.clip(d / back_end, 0, 1)
        before = np.clip((d - quarter * 0.5) / (front_end - quarter * 0.5), 0, 1)
        width = 31.0 - 15.0 * behind ** 1.6 - 8.5 * before ** 1.9
        for end, sign in ((back_end, 1), (front_end, -1)):
            t = np.clip((d - end) * sign / 5.0, 0, 1)
            width = width - 5.5 * (1 - np.sqrt(1 - (1 - t) ** 2))
        s = u * width
        d = np.broadcast_to(d, s.shape)
        p, n = _surface(box, sx, sz, d, s)
        edge = np.minimum.reduce((d - back_end, front_end - d, width - np.abs(s)))
        rim = np.sqrt(np.clip(1 - (1 - np.clip(edge / 0.9, 0, 1)) ** 2, 0, 1))
        bead = 0.22 * np.exp(-((edge - 1.6) / 0.7) ** 2)
        boss = 1.0 * np.exp(-(s / 13.0) ** 2 - ((d - quarter / 2) / 11.0) ** 2)
        seat = 0.0
        for across in (-20.0, 20.0):
            seat = seat + np.exp(-((s - across) ** 2 + (d - quarter - 2.9) ** 2) / 11.0)
        lift = 0.09 + 0.62 * rim + bead + boss * rim - 0.2 * seat
        scuff = np.exp(-(s / 9.0) ** 2 - ((d - quarter / 2) / 5.0) ** 2)
        scuff = scuff * (1.0 if sx < 0 < sz else 0.55)
        name = f"{box.name}_corner_cap_{sx}_{sz}"
        M.grid(name, p + n * lift[..., None], b.mats["cap_steel"], b.static,
               attrs={"scuff": scuff}, flip=sx * sz < 0)
        for k, across in enumerate((-20.0, 20.0)):
            p, n = _surface(box, sx, sz, np.array(quarter + 2.9), np.array(across))
            screw(b, f"{name}_screw{k}", p + n * 0.5, 2.4, 1.1, 0.5 * sx + 0.8 * k + 0.3 * sz, "dark_nickel")


def handle(b, tex):
    *_, z1, _, _ = body_rect()
    y = FRONT + 112
    half = 126.0
    u = np.linspace(-1, 1, 420)
    lift = z1 + 6.0 + (14.0 - 1.6 * u) * np.clip(1 - np.abs(u) ** 2.4, 0, 1)
    section = np.array(M.rounded_rect(28, 8, 3.2, 8))
    edges = []
    for a, c in zip(section, np.roll(section, -1, 0)):
        edges += [a + (c - a) * t for t in np.linspace(0, 1, max(int(np.linalg.norm(c - a) / 0.6), 1), endpoint=False)]
    section = np.array(edges)
    swell = 1 + 0.12 * np.clip(1 - (u / 0.55) ** 2, 0, 1)
    slope = np.arctan(np.gradient(lift, u * half))
    pts = np.stack((
        (u * half)[:, None] - section[:, 1] * swell[:, None] * np.sin(slope)[:, None],
        y + section[:, 0] * swell[:, None],
        lift[:, None] + section[:, 1] * swell[:, None] * np.cos(slope)[:, None],
    ), -1)
    out = pts - np.stack((u * half, np.full_like(u, y), lift), -1)[:, None, :]
    out /= np.linalg.norm(out, axis=-1, keepdims=True)
    pts = pts + out * ((grain_at(tex, pts, out) - 0.5) * 0.22)[..., None]
    M.grid("handle_strap", pts, b.mats["strap"], b.static, closed=True)
    over = np.linspace(-0.9, 0.9, 64)
    stitches, quads = [], []
    face = y - 14 * np.interp(over, u, swell) - 0.2
    middle = np.interp(over, u, lift) + 0.9
    for k in range(len(over) - 1):
        a, c = over[k] * half, over[k] * half + 2.4
        za, zc = middle[k], middle[k] + (middle[k + 1] - middle[k]) * 2.4 / ((over[1] - over[0]) * half)
        quads.append(tuple(range(len(stitches), len(stitches) + 4)))
        stitches += [(a, face[k], za - 0.4), (c, face[k + 1], zc - 0.4),
                     (c, face[k + 1], zc + 0.4), (a, face[k], za + 0.4)]
    M.mesh("handle_stitching", stitches, quads, b.mats["thread"], b.static, smooth=False)
    for side in (-1, 1):
        x = side * (half + 1)
        cap = M.rounded_plate(f"handle_end_{side}", 25, 31, 5,
                              [(0, 0), (0, 6.2), (0.4, 7.9), (1.6, 9.0), (3.6, 9.4)],
                              b.mats["handle_metal"], b.static)
        M.place(cap, (x, y, z1 + 0.3))
        screw(b, f"handle_screw_{side}", (x + side * 3, y - 4, z1 + 9.5), 2.6, 1.2, 0.6,
              "screw", Matrix.Identity(4))


def feet(b):
    x0, x1, z0, *_ = body_rect()
    shape = [(12, 0), (17.5, 0.2), (19.5, 1.2), (20.5, 3.0), (21.5, 11.5), (19, 12.5)]
    for x in (x0 + 62, x1 - 62):
        for y in (FRONT + 34, FRONT + DEPTH - 40):
            foot = M.lathe("foot", shape, b.mats["rubber"], b.static)
            M.place(foot, (x, y, z0 - 12.2))


def build(b):
    os.makedirs(b.cache, exist_ok=True)
    tex = _grain(b.cache)
    box = head_box()
    shell(b, tex, box)
    piping(b, box)
    corner_caps(b, box)
    handle(b, tex)
    feet(b)
    return tex
