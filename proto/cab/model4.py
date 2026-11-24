"""Round 4 cabinet model: the driver is an axisymmetric shell model (shell.py) of the paper cone, the
dust cap and the surround, with the coil mass, the spider and the table's mechanical resistance lumped at the neck.
Everything around it (box, field, microphones, floor, edges, the inside field) is the round-3 code in model.py, which
is given the shell's radiating rings through SOURCE_HOOK. The mechanical impedance the coil sees is the shell's own, so
the electrical impedance follows from the same structure that radiates.

Driver parameters are material and geometry only (all EST until tuned; ranges in BOUNDS4):
  cone   Young's modulus, thickness (and its taper to the rim), density, loss; depth, curvature, outer radius
  cap    Young's modulus, thickness, loss; radius, height (density as the cone's)
  surround  half roll: Young's modulus, thickness, loss (density fixed)
  coil   mass of coil, former, glue and half the spider
There is no seeded irregularity in the driver: 11.1 showed that real units repeat below 5 kHz.
"""
import numpy as np
import model as M
import shell as S

P4 = dict(
    r_coil=0.022,            # published coil diameter 44 mm
    r_frame=0.135,           # EST m: where the surround is clamped (the cut-out is 283 mm)
    r_cone_out=0.115,        # EST m: outer edge of the paper
    cone_depth=0.060, cone_curv=1.0,
    E_cone=2.5e9, h_cone=0.40e-3, h_taper=1.0, rho_cone=650.0, eta_cone=0.05, eta_slope=0.2,
    r_cap=0.045, cap_h=0.012, E_cap=1.5e9, h_cap=0.30e-3, eta_cap=0.10,
    E_sur=4.0e7, h_sur=0.6e-3, rho_sur=1100.0, eta_sur=0.35,
    m_coil=0.012,            # kg
    rib_amp=0.0, rib_pitch=0.009, rib_start=0.5,   # concentric corrugations of the paper (this cone type has them): depth
                             # of the ripple (m), its pitch along the radius (m), where it starts (fraction of the cone's width)
    rms_scale=1.0,           # mechanical resistance relative to the head's table value (EST there as well)
    nu=0.3,
)
BOUNDS4 = dict(
    r_cone_out=(0.105, 0.124), cone_depth=(0.045, 0.080), cone_curv=(0.6, 1.6),
    E_cone=(0.6e9, 7.0e9), h_cone=(0.22e-3, 0.75e-3), h_taper=(0.5, 1.3), rho_cone=(450.0, 950.0), eta_cone=(0.02, 0.14), eta_slope=(0.0, 0.5),
    r_cap=(0.030, 0.062), cap_h=(0.004, 0.028), E_cap=(0.2e9, 6.0e9), h_cap=(0.12e-3, 0.70e-3), eta_cap=(0.03, 0.45),
    E_sur=(3.0e6, 4.0e8), h_sur=(0.3e-3, 1.3e-3), eta_sur=(0.08, 0.9),
    m_coil=(0.005, 0.020),
    rib_amp=(0.0, 1.6e-3), rib_pitch=(0.005, 0.016), rib_start=(0.15, 0.8), rms_scale=(0.4, 2.5), fs_free=(66.0, 86.0),
)
LOG_KEYS = ("E_cone", "E_cap", "E_sur")          # searched on a logarithmic scale
N_CONE, N_CAP, N_SUR, NANG = 100, 18, 8, 24      # 1 mm elements on the cone: the corrugations need them
_cache = {}
_direct = {}
_serial = [0]


def build(p):
    """Mesh of one driver. Returns (mesh, cap node ids, cone node ids, surround node ids, index of the cap joint in the cone chain)."""
    m = S.Mesh()
    rc, ro, d = p["r_coil"], p["r_cone_out"], p["cone_depth"]
    def zc(r):
        t_ = (r - rc) / (ro - rc)
        rib = p["rib_amp"] * np.sin(2 * np.pi * (r - rc) / p["rib_pitch"]) * np.clip((t_ - p["rib_start"]) / 0.08, 0, 1) * np.clip((1 - t_) / 0.05, 0, 1)
        return -d * (1 - t_) ** p["cone_curv"] + rib
    t = np.linspace(0, 1, N_CONE + 1)
    rr = rc + (ro - rc) * t
    j = int(np.clip(np.argmin(np.abs(rr - p["r_cap"])), 2, N_CONE - 4))
    rr[j] = float(np.clip(p["r_cap"], rr[j - 1] + 5e-4, rr[j + 1] - 5e-4))
    hh = p["h_cone"] * (1 + (p["h_taper"] - 1) * 0.5 * (t[1:] + t[:-1]))
    cone = m.line([(r, zc(r)) for r in rr], "cone", p["E_cone"], hh, p["rho_cone"])
    rcap = rr[j]
    rs = 0.0015 + (rcap - 0.0015) * np.linspace(0, 1, N_CAP + 1)
    cap = m.line([(r, zc(rcap) + p["cap_h"] * (1 - (r / rcap) ** 2)) for r in rs], "cap", p["E_cap"], p["h_cap"], p["rho_cone"], last=cone[j])
    wsur = 0.5 * (p["r_frame"] - ro)
    th = np.linspace(np.pi, 0, N_SUR + 1)
    sur = m.line([(ro + wsur + wsur * np.cos(a), wsur * np.sin(a)) for a in th], "sur", p["E_sur"], p["h_sur"], p["rho_sur"], first=cone[-1])
    m.fix(cap[0], [0, 2])            # axis: no radial motion, no slope
    m.fix(cone[0], [0])              # the former keeps the neck round
    m.fix(sur[-1], [0, 1, 2])        # clamped to the frame
    return m, cap, cone, sur, j


def driver_solution(p, f):
    """Shell solution on f, cached. Returns dict: zm (mechanical impedance at the coil, N s/m), pos/normal displacement
    per unit coil displacement at the radiating rings, their areas and positions, masses, piston area."""
    key = (len(f), float(f[0]), float(f[-1])) + tuple(float(p[k]) for k in sorted(P4))+ (float(p["fs_free"]), float(p["rp"]), float(p["cp"]))
    if key in _cache:
        return _cache[key]
    m, cap, cone, sur, j = build(p)
    Ks, Mm = m.assemble()
    dof = 3 * cone[0] + 1
    # rigid-body mass of the paper, cap and half the surround: low-frequency response with everything made stiff is not
    # needed; the mass matrix gives it directly for a unit axial translation
    ez = np.zeros(Mm.shape[0]); ez[1::3] = 1.0
    surn = np.array(sur[1:])
    ez_s = ez.copy(); ez_s[3 * surn + 1] = np.linspace(1, 0, len(sur))[1:]      # the roll goes from full motion to none
    m_shell = float(ez_s @ Mm @ ez_s)
    m_tot = m_shell + p["m_coil"]
    bl2 = m_tot / p["cp"]                                # the head's table: C = M / Bl^2 (X11)
    rms = p["rms_scale"] * bl2 / p["rp"]
    # spider: whatever axial stiffness the surround does not supply toward the published free-air resonance
    x0 = S.solve(Ks, Mm, m.free, {g: 0.0 for g in Ks}, [0.5], dof)[0, dof].real
    k_sur = 1.0 / x0
    k_spider = max(m_tot * (2 * np.pi * p["fs_free"]) ** 2 - k_sur, 0.0)
    eta_c = np.minimum(p["eta_cone"] * (f / 1000.0) ** p["eta_slope"], 0.3)
    x = S.solve(Ks, Mm, m.free, {"cone": eta_c, "cap": np.full(len(f), p["eta_cap"]), "sur": np.full(len(f), p["eta_sur"])}, f, dof,
                lumped={dof: (p["m_coil"], k_spider, rms)})
    xc = x[:, dof]
    ids = list(cap) + list(cone[j + 1:]) + list(sur[1:-1])
    a_cap, nr_c, nz_c = m.radiators(cap)
    a_cone, nr_k, nz_k = m.radiators(cone[j:])
    a_sur, nr_s, nz_s = m.radiators(sur)
    area = np.concatenate([a_cap, a_cone[1:], a_sur[1:-1]]); area[len(cap) - 1] += a_cone[0]; area[len(cap) + len(cone[j + 1:]) - 1] += a_sur[0]
    nr = np.concatenate([nr_c, nr_k[1:], nr_s[1:-1]]); nz = np.concatenate([nz_c, nz_k[1:], nz_s[1:-1]])
    ids = np.array(ids)
    wn = (x[:, 3 * ids] * nr[None, :] + x[:, 3 * ids + 1] * nz[None, :]) / xc[:, None]
    # radiation does not need the 1 mm mesh: neighbouring cone rings are merged in pairs (volume displacement summed)
    nc0, nc1 = len(cap), len(cap) + len(cone[j + 1:])
    k2 = ((nc1 - nc0) // 2) * 2
    def merge(vals, wts):
        a2 = wts[nc0:nc0 + k2].reshape(-1, 2)
        return np.concatenate([vals[..., :nc0], (vals[..., nc0:nc0 + k2] * wts[nc0:nc0 + k2]).reshape(vals.shape[:-1] + (-1, 2)).sum(-1) / a2.sum(1), vals[..., nc0 + k2:]], -1)
    rr_, zz_ = np.array(m.r)[ids], np.array(m.z)[ids]
    wn, nz, rr_, zz_ = merge(wn, area), merge(nz, area), merge(rr_, area), merge(zz_, area)
    area = np.concatenate([area[:nc0], area[nc0:nc0 + k2].reshape(-1, 2).sum(1), area[nc0 + k2:]])
    out = dict(zm=1.0 / (2j * np.pi * f * xc), wn=wn, area=area, r=rr_, z=zz_, nz=nz, m_tot=m_tot, m_shell=m_shell,
               bl2=bl2, k_sur=k_sur, k_spider=k_spider, s_piston=float(np.sum(area * wn[0].real)), ncap=len(cap))       # effective piston area: the static deflection shape
    _serial[0] += 1
    out["serial"] = _serial[0]
    if len(_cache) > 6:
        _cache.clear()
    _cache[key] = out
    return out


def full(p):
    """Parameter set for model.py's box, field and inside-field code, derived from the shell driver."""
    d = driver_solution(p, M.F_COARSE)
    q = dict(p)
    q["a_eff"] = float(np.sqrt(d["s_piston"] / np.pi))
    q["mms"] = d["m_tot"]
    q["cone_frac"] = float(np.clip(p["rho_cone"] * p["h_cone"] * np.pi * (q["a_eff"] ** 2 - p["r_coil"] ** 2) / d["m_tot"], 0.05, 0.95))
    return q, d


def source_points(p, f, breakup=True, coarse=False):
    """Radiating rings of one driver for model.transfer: pos (M,3), volume displacement per unit coil displacement
    (len(f), M), areas (M,), and the mechanical impedance at the coil in the slot the round-3 code calls 'shed'."""
    d = driver_solution(p, f)
    wn = d["wn"] if breakup else np.broadcast_to(d["nz"][None, :], d["wn"].shape)          # rigid: axial unit motion
    r, z, area = d["r"], d["z"], d["area"]
    if coarse:
        g = 3
        k = (len(r) // g) * g
        A = area[:k].reshape(-1, g)
        qq = (wn[:, :k] * area[None, :k]).reshape(len(f), -1, g).sum(2)
        r, z, area = (r[:k].reshape(-1, g) * A).sum(1) / A.sum(1), (z[:k].reshape(-1, g) * A).sum(1) / A.sum(1), A.sum(1)
    else:
        qq = wn * area[None, :]
    nang = 8 if coarse else NANG
    phi = (np.arange(nang) + 0.5) * 2 * np.pi / nang
    pos = np.stack([np.outer(r, np.cos(phi)).ravel(), np.outer(r, np.sin(phi)).ravel(), np.repeat(z, nang)], 1)
    return pos, np.repeat(qq, nang, axis=1) / nang, np.repeat(area, nang) / nang, d["zm"]


def driver(p, d, zm_fine, modes=True):
    """(coil velocity per volt, electrical impedance) on F_FINE for one driver of four in its box."""
    f = M.F_FINE
    w = 2 * np.pi * np.maximum(f, 1e-3)
    par = lambda a, b: a * b / (a + b)
    ze = p["re"] + par(1j * w * p["le1"], p["rle1"]) + par(1j * w * p["le2"], p["rle2"])
    zm = zm_fine + d["s_piston"] ** 2 * M.box_impedance(p, f, modes)
    bl = np.sqrt(d["bl2"])
    return bl / (ze * zm + d["bl2"]), ze + d["bl2"] / zm


def response(p, mic_name, x, dist=0.025, angle=0.0, stage="r4b", mic_over=None):
    """Terminal volts to microphone volts on F_FINE (as model.response, with the shell driver)."""
    q, d = full(p)
    st = M.STAGE[stage]
    # everything but the floor image is the same for every speaker of a cabinet: cached per driver, geometry and position
    key = (d["serial"], mic_name, float(x), float(dist), float(angle), stage, repr(mic_over)) + tuple(float(q[k]) for k in ("cone_dx", "cone_dy", "baffle_w", "baffle_h", "edges"))
    M.SOURCE_HOOK = source_points
    try:
        if key not in _direct:
            if len(_direct) > 40:
                _direct.clear()
            _direct[key] = M.transfer(dict(q, floor=0.0), mic_name, x, dist, angle, stage, parts=True, mic_over=mic_over, fast=True)
        parts, mic, zmc, power = _direct[key]
        parts = list(parts)
        if st["floor"] and q["floor"] > 0:
            parts += M.transfer(q, mic_name, x, dist, angle, stage, parts=True, mic_over=mic_over, fast=True, only_floor=True)[0][1:]
    finally:
        M.SOURCE_HOOK = None
    vel, _ = driver(q, d, M._to_fine(zmc), st["box_modes"])
    w = 2 * np.pi * M.F_FINE
    t0 = parts[0][1]
    H = sum(M._to_fine(Hp) * np.exp(-1j * w * (tau - t0 + 0.0003)) for Hp, tau in parts) * vel
    if st.get("late") and q["late"] > 0:
        Zm = M.box_impedance(q, M.F_FINE, True, parts=True)[1]
        fo = 0.67 * q["box_fmax"]
        ms = p["rho_cone"] * p["h_cone"]                                       # the paper's own areal mass
        thru = 1 / (1 + 1j * w * ms / (2 * M.RHO * M.C0))
        fade = 1 - np.clip(np.log(np.maximum(M.F_FINE, 1.0) / fo) / np.log(q["box_fmax"] / fo), 0, 1) ** 2
        H = H - q["late"] * q["late_gain"] * thru * d["s_piston"] * Zm * vel * np.sqrt(fade) * np.exp(-1j * w * 0.0003)
        pw = np.exp(np.interp(np.log(np.maximum(M.F_FINE, M.F_COARSE[0])), np.log(M.F_COARSE), np.log(power + 1e-300)))
        H = H + q["late"] * M.late_field(q, pw, vel)[0] * np.exp(-1j * w * 0.0003)
    if mic.get("curve"):
        H = H * M.min_phase_from_db(M.curve_db(mic["curve"], M.F_FINE))
    H[0] = 0.0
    return H * 0.5 * (1 + np.cos(np.pi * np.clip((M.F_FINE - 20000.0) / 3000.0, 0, 1)))


def impulse_response(p, mic_name, x, dist=0.025, angle=0.0, stage="r4b", mic_over=None):
    h = np.fft.irfft(response(p, mic_name, x, dist, angle, stage, mic_over), M.NFFT)[: M.NTAPS].copy()
    h[-512:] *= 0.5 * (1 + np.cos(np.linspace(0, np.pi, 512)))
    return h / np.abs(h).max()


def impedance(p, f=None):
    """Electrical impedance of one driver in its box on F_FINE (for G-C8 and the published family band)."""
    q, d = full(p)
    return driver(q, d, M._to_fine(d["zm"]), True)[1]
