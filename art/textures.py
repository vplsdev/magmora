"""Procedural height fields. Numpy only: runs inside Blender and in a plain Python.

Every texture in the render is generated here from a seed; nothing is downloaded or scanned.
"""
import numpy as np


def _fade(t):
    return t * t * t * (t * (t * 6 - 15) + 10)


def noise(u, v, period, seed):
    """Periodic 2D gradient noise, roughly -1..1. u, v are in lattice units."""
    pu, pv = (period, period) if np.isscalar(period) else period
    angle = np.random.default_rng(seed).random((pv, pu)) * 2 * np.pi
    gx, gy = np.cos(angle).astype(np.float32), np.sin(angle).astype(np.float32)
    iu, iv = np.floor(u).astype(np.int32), np.floor(v).astype(np.int32)
    fu, fv = u - iu, v - iv

    def dot(du, dv):
        i, j = (iu + du) % pu, (iv + dv) % pv
        return gx[j, i] * (fu - du) + gy[j, i] * (fv - dv)

    a, b = _fade(fu), _fade(fv)
    top = dot(0, 0) * (1 - a) + dot(1, 0) * a
    bottom = dot(0, 1) * (1 - a) + dot(1, 1) * a
    return (top * (1 - b) + bottom * b) * 1.55


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


TOLEX_TILE_MM = 120.0


def _wandering(u, v, pitch_mm, seed, stretch=1.0):
    """Band-limited noise pulled about by a slower field, so folds bend and fork."""
    tile = TOLEX_TILE_MM
    n = round(tile / pitch_mm)
    m = round(tile / (pitch_mm * 2.6))
    du = noise(u * m, v * m, m, seed + 100) * pitch_mm * stretch / tile
    dv = noise(u * m, v * m, m, seed + 200) * pitch_mm * stretch / tile
    return noise((u + du) * n, (v + dv) * n, n, seed)


def tolex(size=2400, seed=7):
    """Levant-grain vinyl, 0..1.

    Short folds that wander, fork and fade into one another. There are deliberately no cells
    and no creases that close into outlines: from a step away it is a satin speckle.
    """
    axis = (np.arange(size, dtype=np.float32) + 0.5) / size
    u, v = np.meshgrid(axis, axis)

    body = np.tanh(1.5 * _wandering(u, v, 1.25, seed, 0.75)) * 0.50
    body += np.tanh(1.3 * _wandering(u, v, 0.78, seed + 1, 0.8)) * 0.30

    # valleys along zero crossings, each kept only here and there so that none encloses a cell
    for k, (pitch, depth) in enumerate(((1.9, 0.30), (1.15, 0.20))):
        line = _wandering(u, v, pitch, seed + 10 + k, 1.1)
        keep = smoothstep(-0.15, 0.45, _wandering(u, v, pitch * 1.7, seed + 20 + k))
        body -= depth * np.exp(-(line / 0.22) ** 2) * keep

    body += 0.07 * _wandering(u, v, 0.36, seed + 30)
    body += 0.10 * _wandering(u, v, 7.5, seed + 31)

    low, high = np.percentile(body, [0.3, 99.7])
    return np.clip((body - low) / (high - low), 0, 1).astype(np.float32)


def sample(tex, u_mm, v_mm, tile_mm):
    """Bilinear, wrapping lookup in millimetres."""
    n = tex.shape[0]
    x = np.mod(u_mm / tile_mm, 1.0) * n - 0.5
    y = np.mod(v_mm / tile_mm, 1.0) * n - 0.5
    i, j = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
    a, b = x - i, y - j
    i0, i1, j0, j1 = i % n, (i + 1) % n, j % n, (j + 1) % n
    return (tex[j0, i0] * (1 - a) + tex[j0, i1] * a) * (1 - b) + (
        tex[j1, i0] * (1 - a) + tex[j1, i1] * a) * b
