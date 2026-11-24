"""Circumferential orders n >= 1 of the cone, cap and surround (round 5), excited by the two lead-out
wires. A wire's glued end is a small mass on the cone at radius wire_r; it rides on the axisymmetric motion, and its
inertia is a point force on the shell. A point force at angle t0 drives every order n as cos(n (theta - t0)); each
order is solved with shelln.py on the same mesh, materials and losses as the axisymmetric driver (first-order
perturbation: the wires' reaction on the axisymmetric motion is neglected). The coil former keeps the neck round and
is taken as held (no rocking of the coil); the surround is clamped at the frame.

The two wires sit at wire_ang +- wire_half from the direction in which the microphone is moved off the cap centre;
the four drivers are assumed to be mounted the same way round, so the angle is mirrored for the left pair.
"""
import numpy as np
import model as M
import model4 as M4
import shell as S
import shelln as SN

_cache = {}


def rings(p, f):
    """Normal displacement of orders 1..n_harm at the radiating rings of model4.driver_solution (same merging), per
    unit coil displacement, per kilogram of ONE wire and per unit of cos(n (theta - its angle)): (n_harm, len(f), rings)."""
    d = M4.driver_solution(p, f)
    m, cap, cone, sur, j = M4.build(p)
    cr = np.array(m.r)[cone]
    kw = int(np.clip(np.argmin(np.abs(cr - p["wire_r"])), 2, len(cone) - 3))
    key = (len(f), float(f[0]), float(f[-1]), kw) + tuple(float(p[k]) for k in sorted(M4.P4)) + tuple(float(p[k]) for k in ("fs_free", "rp", "cp", "n_harm"))
    if key in _cache:
        return _cache[key]
    ids = np.array(list(cap) + list(cone[j + 1:]) + list(sur[1:-1]))
    a_cap, nr_c, nz_c = m.radiators(cap)
    a_cone, nr_k, nz_k = m.radiators(cone[j:])
    a_sur, nr_s, nz_s = m.radiators(sur)
    area = np.concatenate([a_cap, a_cone[1:], a_sur[1:-1]]); area[len(cap) - 1] += a_cone[0]; area[len(cap) + len(cone[j + 1:]) - 1] += a_sur[0]
    nr = np.concatenate([nr_c, nr_k[1:], nr_s[1:-1]]); nz = np.concatenate([nz_c, nz_k[1:], nz_s[1:-1]])
    nc0, nc1 = len(cap), len(cap) + len(cone[j + 1:])
    k2 = ((nc1 - nc0) // 2) * 2
    def merge(vals):
        a2 = area[nc0:nc0 + k2].reshape(-1, 2)
        return np.concatenate([vals[..., :nc0], (vals[..., nc0:nc0 + k2] * area[nc0:nc0 + k2]).reshape(vals.shape[:-1] + (-1, 2)).sum(-1) / a2.sum(1), vals[..., nc0 + k2:]], -1)
    # the wire's node on the cone, its normal there, and the axisymmetric normal motion it rides on
    node = cone[kw]
    tr_, tz_ = cr[kw + 1] - cr[kw - 1], m.z[cone[kw + 1]] - m.z[cone[kw - 1]]
    nrm = np.array([-tz_, tr_]) / np.hypot(tr_, tz_)
    w0 = d["wn"][:, np.argmin(np.abs(d["r"] - cr[kw]) + 1e3 * (np.arange(len(d["r"])) < d["ncap"]))]      # (F,) at the nearest cone ring
    force = (2 * np.pi * f) ** 2 * w0
    eta_c = np.minimum(p["eta_cone"] * (f / 1000.0) ** p["eta_slope"], 0.3)
    etas = {"cone": eta_c, "cap": np.full(len(f), p["eta_cap"]), "sur": np.full(len(f), p["eta_sur"])}
    fixed = {4 * cone[0] + k for k in (0, 1, 3)} | {4 * sur[-1] + k for k in range(4)}
    free = np.array([i for i in range(4 * len(m.r)) if i not in fixed])
    out = []
    for n in range(1, int(p["n_harm"]) + 1):
        Ks, Mm = SN.assemble(m, n)
        rhs = np.zeros((len(f), 4 * len(m.r)), complex)
        rhs[:, 4 * node] = force * nrm[0]; rhs[:, 4 * node + 1] = force * nrm[1]
        x = SN.solve(Ks, Mm, free, etas, f, rhs)
        out.append(merge(x[:, 4 * ids] * nr[None, :] + x[:, 4 * ids + 1] * nz[None, :]))
    out = np.array(out)
    if len(_cache) > 4:
        _cache.clear()
    _cache[key] = out
    return out


def hook(speaker):
    """A model.SOURCE_HOOK: model4's radiating rings with the circumferential orders added on the miked cone."""
    def source_points(p, f, breakup=True, coarse=False):
        pos, qq, areas, zm = M4.source_points(p, f, breakup, coarse)
        if coarse or not breakup or p["wire_m"] <= 0:
            return pos, qq, areas, zm
        d = M4.driver_solution(p, f)
        wn = rings(p, f)
        nang = M4.NANG
        phi = (np.arange(nang) + 0.5) * 2 * np.pi / nang
        ang = np.radians(p["wire_ang"] if speaker[1] == "R" else 180.0 - p["wire_ang"])
        add = np.zeros((len(f), len(d["r"]), nang), complex)
        for n in range(1, wn.shape[0] + 1):
            pat = sum(np.cos(n * (phi - t0)) for t0 in (ang - np.radians(p["wire_half"]), ang + np.radians(p["wire_half"])))
            add += p["wire_m"] * (wn[n - 1] * d["area"][None, :])[:, :, None] * pat[None, None, :] / nang
        return pos, qq + add.reshape(len(f), -1), areas, zm
    return source_points
