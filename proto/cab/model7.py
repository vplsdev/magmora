"""Round 7 cabinet model: round 5's final model (model5, stage r5d) with one more layer, the top end
of the driver. One cabinet (Trad-style closed-back 4x12), one microphone (57-type). Nothing here reads a reference.
model4.py, model5.py and harmonics5.py are not changed; with the layer's three numbers at zero this is r5d exactly
(run this file: `$BLOCKAMP_PY proto/cab/model7.py`).

Why: r5d's level from 6 to 13 kHz falls too steeply from the cap centre to the edge. r5d's driver is
perfectly axisymmetric and its dust cap is a clean shell with a constant loss factor, glued to the cone by a
massless joint. On the axis of a perfect body of revolution every ring adds in phase at all frequencies: the cap
centre is a focus that a real cone, which is not uniform around its circumference, does not have.

The layer, three numbers, all with physical ranges fixed before any fit (BOUNDS7):

  irr            the paper is not uniform around the circumference (thickness and density of the felted pulp vary by
                 sector), so bending waves run at slightly different speeds in different sectors: irr is the rms
                 relative spread of that speed. The shell solution gives, at every ring and frequency, the phase lag
                 of the ring behind the coil, which is what the wave has accumulated on its way from the neck (followed
                 up from the lowest frequency, where it is zero). In a sector whose speed is lower by the
                 fraction e that lag is larger by the same fraction, so each ring's radiation is multiplied by
                 exp(-j lag irr g(angle)), g a fixed smooth pattern of unit rms (orders 1 to 8, seeded once; mirrored
                 for the left pair of drivers like the lead-out wires). Where the cone moves as a whole the lag is zero
                 and nothing changes; amplitudes are untouched, so no energy is added or removed. First-order
                 perturbation: the reaction on the axisymmetric motion is neglected, as for the wires. This is the
                 seeded sector-to-sector spread of rounds 1 to 3, on the shell model. Range 0 to 0.15 (round 3's).
  eta_cap_slope  the cap's loss factor rises with frequency as (f / 1 kHz)^eta_cap_slope, as the cone's already does
                 (treated cloth and paper are viscoelastic); never above 0.6. Range 0 to 0.5 (the range of the cone's
                 exponent). r5d: 0, a constant loss factor.
  m_joint        mass of the glue bead that joins the cap to the cone, a ring mass on the joint node; it counts in
                 the moving mass. Range 0 to 2 g. r5d: 0.

Not in the layer: the 57-type capsule's own numbers (stage r5e tried them alone; they did not move the scores).
The circumferential orders driven by the lead-out wires (harmonics5.py) keep r5d's constant cap loss and have no bead;
they move the response by tenths of a dB.
"""
import numpy as np
import model as M
import model4 as M4
import model5 as M5
import shell as S

C0, RHO = M.C0, M.RHO

P7 = dict(irr=0.0, eta_cap_slope=0.0, m_joint=0.0, irr_seed=7.0, irr_orders=8.0)
BOUNDS7 = dict(irr=(0.0, 0.15), eta_cap_slope=(0.0, 0.5), m_joint=(0.0, 2.0e-3))
OFF7 = dict(irr=0.0, eta_cap_slope=0.0, m_joint=0.0)
ETA_CAP_MAX = 0.6
_direct = {}
_cache = {}
_last = {}
_driver_solution_r4 = M4.driver_solution


def cap_eta(p, f):
    return np.minimum(p["eta_cap"] * (np.maximum(f, 1.0) / 1000.0) ** p.get("eta_cap_slope", 0.0), max(ETA_CAP_MAX, p["eta_cap"]))


def driver_solution(p, f):
    """model4.driver_solution with the cap's loss rising with frequency and the glue bead at the cap joint. With both
    numbers at zero it IS model4's function (same cache, same result)."""
    slope, mj = float(p.get("eta_cap_slope", 0.0)), float(p.get("m_joint", 0.0))
    if slope == 0.0 and mj == 0.0:
        return _driver_solution_r4(p, f)
    key = (len(f), float(f[0]), float(f[-1]), slope, mj) + tuple(float(p[k]) for k in sorted(M4.P4)) + (float(p["fs_free"]), float(p["rp"]), float(p["cp"]))
    if key in _cache:
        return _cache[key]
    m, cap, cone, sur, j = M4.build(p)
    Ks, Mm = m.assemble()
    dof = 3 * cone[0] + 1
    ez = np.zeros(Mm.shape[0]); ez[1::3] = 1.0
    surn = np.array(sur[1:])
    ez_s = ez.copy(); ez_s[3 * surn + 1] = np.linspace(1, 0, len(sur))[1:]
    m_shell = float(ez_s @ Mm @ ez_s) + mj                                  # the bead moves with the cone
    m_tot = m_shell + p["m_coil"]
    bl2 = m_tot / p["cp"]
    rms = p["rms_scale"] * bl2 / p["rp"]
    x0 = S.solve(Ks, Mm, m.free, {g: 0.0 for g in Ks}, [0.5], dof)[0, dof].real
    k_sur = 1.0 / x0
    k_spider = max(m_tot * (2 * np.pi * p["fs_free"]) ** 2 - k_sur, 0.0)
    eta_c = np.minimum(p["eta_cone"] * (f / 1000.0) ** p["eta_slope"], 0.3)
    lumped = {dof: (p["m_coil"], k_spider, rms)}
    if mj > 0:
        lumped[3 * cone[j]] = (mj, 0.0, 0.0); lumped[3 * cone[j] + 1] = (mj, 0.0, 0.0)
    x = S.solve(Ks, Mm, m.free, {"cone": eta_c, "cap": cap_eta(p, f), "sur": np.full(len(f), p["eta_sur"])}, f, dof, lumped=lumped)
    xc = x[:, dof]
    ids = list(cap) + list(cone[j + 1:]) + list(sur[1:-1])
    a_cap, nr_c, nz_c = m.radiators(cap)
    a_cone, nr_k, nz_k = m.radiators(cone[j:])
    a_sur, nr_s, nz_s = m.radiators(sur)
    area = np.concatenate([a_cap, a_cone[1:], a_sur[1:-1]]); area[len(cap) - 1] += a_cone[0]; area[len(cap) + len(cone[j + 1:]) - 1] += a_sur[0]
    nr = np.concatenate([nr_c, nr_k[1:], nr_s[1:-1]]); nz = np.concatenate([nz_c, nz_k[1:], nz_s[1:-1]])
    ids = np.array(ids)
    wn = (x[:, 3 * ids] * nr[None, :] + x[:, 3 * ids + 1] * nz[None, :]) / xc[:, None]
    nc0, nc1 = len(cap), len(cap) + len(cone[j + 1:])
    k2 = ((nc1 - nc0) // 2) * 2
    def merge(vals, wts):
        a2 = wts[nc0:nc0 + k2].reshape(-1, 2)
        return np.concatenate([vals[..., :nc0], (vals[..., nc0:nc0 + k2] * wts[nc0:nc0 + k2]).reshape(vals.shape[:-1] + (-1, 2)).sum(-1) / a2.sum(1), vals[..., nc0 + k2:]], -1)
    rr_, zz_ = np.array(m.r)[ids], np.array(m.z)[ids]
    wn, nz, rr_, zz_ = merge(wn, area), merge(nz, area), merge(rr_, area), merge(zz_, area)
    area = np.concatenate([area[:nc0], area[nc0:nc0 + k2].reshape(-1, 2).sum(1), area[nc0 + k2:]])
    out = dict(zm=1.0 / (2j * np.pi * f * xc), wn=wn, area=area, r=rr_, z=zz_, nz=nz, m_tot=m_tot, m_shell=m_shell,
               bl2=bl2, k_sur=k_sur, k_spider=k_spider, s_piston=float(np.sum(area * wn[0].real)), ncap=len(cap))
    M4._serial[0] += 1
    out["serial"] = M4._serial[0]
    if len(_cache) > 6:
        _cache.clear()
    _cache[key] = out
    return out


M4.driver_solution = driver_solution        # model4.full / source_points and harmonics5 now come through here


def lag(d):
    """Phase lag (rad) of every radiating ring's normal motion behind the coil's, followed up from the lowest frequency
    where the whole cone moves with the coil (lag 0): unwrapped along frequency, ring by ring, so it is continuous in
    frequency. (F, rings); needs the solution on a grid as dense as F_COARSE."""
    ph = np.unwrap(np.angle(d["wn"]), axis=0)
    return -(ph - ph[:1])


def pattern(p, phi):
    """The fixed circumferential pattern g(angle): zero mean, unit rms, orders 1..irr_orders with weights 1/sqrt(n)."""
    rng = np.random.default_rng(int(p.get("irr_seed", P7["irr_seed"])))
    n = np.arange(1, int(p.get("irr_orders", P7["irr_orders"])) + 1)
    th = rng.uniform(0, 2 * np.pi, len(n))
    g = (np.cos(np.outer(phi, n) + th[None, :]) / np.sqrt(n)[None, :]).sum(1)
    return g / np.sqrt((0.5 / n).sum())


def hook(speaker):
    """A model.SOURCE_HOOK: harmonics5's (model4's rings plus the wires' orders) with the sector-to-sector spread."""
    import harmonics5 as H5
    base = H5.hook(speaker)
    def source_points(p, f, breakup=True, coarse=False):
        top = (float(p.get("eta_cap_slope", 0.0)), float(p.get("m_joint", 0.0)))
        if _last.get("top") != top:                 # harmonics5's cache does not know this layer's numbers
            H5._cache.clear(); _last["top"] = top
        pos, qq, areas, zm = base(p, f, breakup, coarse)
        if coarse or not breakup or p.get("irr", 0.0) <= 0:
            return pos, qq, areas, zm
        d = M4.driver_solution(p, f)
        nang = M4.NANG
        phi = (np.arange(nang) + 0.5) * 2 * np.pi / nang
        g = pattern(p, phi if speaker[1] == "R" else np.pi - phi)
        psi = p["irr"] * lag(d)[:, :, None] * g[None, None, :]              # (F, rings, angles)
        return pos, qq * np.exp(-1j * psi).reshape(len(f), -1), areas, zm
    return source_points


def response(p, mic_name, x, dist, angle=0.0, speaker="LR"):
    """Terminal volts to microphone volts on F_FINE: model5.response at stage r5d with the top-end layer."""
    fl = M5.STAGE5["r5d"]
    q, d = M4.full(p)
    lower = speaker[0] == "L"
    floor_y = p["floor_h"] if lower else -(p["floor_h"] + 2 * p["cone_dy"])
    key = (d["serial"], mic_name, float(x), float(dist), float(angle), speaker[1]) + tuple(
        float(q.get(k, 0.0)) for k in ("cone_dx", "cone_dy", "baffle_w", "baffle_h", "edges", "wire_m", "wire_r", "wire_ang", "irr", "irr_seed", "irr_orders"))
    M.SOURCE_HOOK = hook(speaker)
    try:
        if key not in _direct:
            if len(_direct) > 40:
                _direct.clear()
            _direct[key] = M.transfer(dict(q, floor=0.0), mic_name, x, dist, angle, "r4b", parts=True, fast=True)
        parts, mic, zmc, power = _direct[key]
        parts = list(parts)
        M.SOURCE_HOOK = M4.source_points
        if q["floor"] > 0:
            parts += M.transfer(dict(q, floor_h=-floor_y), mic_name, x, dist, angle, "r4b", parts=True, fast=True, only_floor=True)[0][1:]
        if p["room_beta"] > 0:
            parts += M5.room_parts(p, q, mic, x, dist, angle, speaker, floor_y)
    finally:
        M.SOURCE_HOOK = None
    bx = M5.box5(q, fl, d["m_tot"])
    vel, _ = M5.driver5(q, d, M._to_fine(zmc), bx)
    w = 2 * np.pi * M.F_FINE
    t0 = parts[0][1]
    H = sum(M._to_fine(Hp) * np.exp(-1j * w * (tau - t0 + 0.0003)) for Hp, tau in parts) * vel
    H = H + M5.baffle_field(q, bx, mic, x, dist, angle) * vel * np.exp(1j * w * (t0 - 0.0003))
    if q["late"] > 0:
        fo = 0.67 * q["box_fmax"]
        ms = p["rho_cone"] * p["h_cone"]
        thru = 1 / (1 + 1j * w * ms / (2 * RHO * C0))
        fade = 1 - np.clip(np.log(np.maximum(M.F_FINE, 1.0) / fo) / np.log(q["box_fmax"] / fo), 0, 1) ** 2
        H = H - q["late"] * q["late_gain"] * thru * d["s_piston"] * bx["Zm"] * vel * np.sqrt(fade) * np.exp(-1j * w * 0.0003)
        pw = np.exp(np.interp(np.log(np.maximum(M.F_FINE, M.F_COARSE[0])), np.log(M.F_COARSE), np.log(power + 1e-300)))
        H = H + q["late"] * M.late_field(q, pw, vel)[0] * np.exp(-1j * w * 0.0003)
    if mic.get("curve"):
        H = H * M.min_phase_from_db(M.curve_db(mic["curve"], M.F_FINE))
    H[0] = 0.0
    return H * 0.5 * (1 + np.cos(np.pi * np.clip((M.F_FINE - 20000.0) / 3000.0, 0, 1)))


def impulse_response(p, mic_name, x, dist, angle=0.0, speaker="LR"):
    h = np.fft.irfft(response(p, mic_name, x, dist, angle, speaker), M.NFFT)[: M.NTAPS].copy()
    h[-512:] *= 0.5 * (1 + np.cos(np.linspace(0, np.pi, 512)))
    return h / np.abs(h).max()


def impedance(p):
    q, d = M4.full(p)
    return M5.driver5(q, d, M._to_fine(d["zm"]), M5.box5(q, M5.STAGE5["r5d"], d["m_tot"]))[1]


if __name__ == "__main__":
    # checks without any measurement
    p = {**M.P0, **M4.P4, **M5.P5, **P7}
    a = M5.response(p, "57", 0.0254, 0.04, 0.0, "r5d", "LR")
    b = response(p, "57", 0.0254, 0.04, 0.0, "LR")
    print(f"1. layer off: response equals round 5's r5d within {np.max(np.abs(a - b)) / np.max(np.abs(a)):.1e}")
    # 2. the rewritten driver solution with a vanishing bead and slope equals model4's
    f = M.F_COARSE
    d0 = _driver_solution_r4(p, f)
    d1 = driver_solution(dict(p, m_joint=1e-12), f)
    print(f"2. driver solution with a 1e-12 kg bead equals model4's within {np.max(np.abs(d1['wn'] - d0['wn'])) / np.max(np.abs(d0['wn'])):.1e}"
          f" (ring motion), {np.max(np.abs(d1['zm'] - d0['zm']) / np.abs(d0['zm'])):.1e} (impedance at the coil)")
    d2 = driver_solution(dict(p, m_joint=1e-3), f)
    print(f"3. a 1 g bead: moving mass {1e3 * d0['m_tot']:.2f} g -> {1e3 * d2['m_tot']:.2f} g")
    # 4. the pattern: zero mean, unit rms; the spread changes no amplitude (power of the miked cone unchanged)
    phi = (np.arange(M4.NANG) + 0.5) * 2 * np.pi / M4.NANG
    g = pattern(p, phi)
    M.SOURCE_HOOK = None
    q, d = M4.full(p)
    _, q0, _, _ = hook("LR")(dict(q, irr=0.0), f, True)
    _, q1, _, _ = hook("LR")(dict(q, irr=0.1), f, True)
    print(f"4. pattern mean {g.mean():+.1e}, rms {np.sqrt(np.mean(g ** 2)):.3f}; |q| changed by at most {np.max(np.abs(np.abs(q1) - np.abs(q0))) / np.max(np.abs(q0)):.1e}")
    L = lag(d)
    for fq in (500.0, 2000.0, 6000.0, 10000.0, 13000.0):
        i = int(np.argmin(np.abs(f - fq)))
        print(f"   lag at {fq:6.0f} Hz: cap centre {L[i, 0]:+6.2f} rad, cap joint {L[i, d['ncap'] - 1]:+6.2f}, mid cone {L[i, (d['ncap'] + L.shape[1]) // 2]:+6.2f}, rim {L[i, -8]:+6.2f}")
    # 5. on the axis the spread lowers the top, off the axis less (the focus): level of the direct sound, irr 0.1 against 0
    for x_in in (0.0, 1.0, 2.0):
        h0 = response(dict(p, irr=0.0), "57", x_in * 0.0254, 0.04, 0.0, "LR")
        h1 = response(dict(p, irr=0.1), "57", x_in * 0.0254, 0.04, 0.0, "LR")
        band = lambda h, lo, hi: 10 * np.log10(np.mean(np.abs(h[(M.F_FINE >= lo) & (M.F_FINE < hi)]) ** 2))
        print(f"5. {x_in:.0f} in from the centre, irr 0.1 against 0: 200 Hz-5 kHz {band(h1, 200, 5000) - band(h0, 200, 5000):+.2f} dB, 6-13 kHz {band(h1, 6000, 13000) - band(h0, 6000, 13000):+.2f} dB")
