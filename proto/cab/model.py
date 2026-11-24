"""Modelled closed-back 4x12 with close microphones: a frequency-domain physical model rendered to an impulse
response Prototype; nothing here reads the reference pack.

Layers (each can be switched off, which is how the stages 5a to 5d are scored):
  1. driver   electromechanical driver on the head's own load table, closed box (lumped, or rectangular-room modes)
  2. field    Rayleigh integral over four baffled cones, baffle-edge sources, floor image; pressure and particle
              velocity at the microphone
  3. breakup  a one-dimensional bending-wave model of the cone between coil and surround with a ring-stiffness term
              (rigid below the local ring frequency), an absorbing surround, a resonant dust cap, and seeded
              sector-to-sector irregularity (the statistical top)
  mics        finite diaphragm, first-order pattern with a frequency-dependent pressure share, published on-axis
              curve as a minimum-phase capsule response

Every number in P0 is either from a source we opened (docs/research/data/cab_driver_published.csv and the curve
files next to it) or an estimate, marked EST. docs/research/CABINET.md lists the sources.
"""
import csv
from pathlib import Path
import numpy as np
from scipy.special import j1

C0, RHO = 343.0, 1.204
ROUND = 3          # rounds 1 and 2 used earlier versions of this file (not published)
FS, NFFT, NTAPS = 48000, 8192, 4096     # round 2: 85 ms; 2048 taps cannot hold the late field
DATA = Path(__file__).resolve().parents[2] / "docs" / "research" / "data"

P0 = dict(
    # --- driver: the head's load table (proto/blockamp/amp.py spk_*; X11) plus one mass to make it mechanical
    re=12.9, le1=1.245e-3, rle1=11.11, le2=0.5784e-3, rle2=80.59, rp=85.0, lp=31e-3, cp=68e-6,
    fs_free=75.0,         # published free-air resonance
    a_eff=0.128,          # EST m, piston radius; shared by every cabinet (round 3)
    mms=0.030,            # EST kg, moving mass in the box at low frequency (cone, coil, cap, air load)
    cone_frac=0.5,        # EST share of that mass that is cone between coil and surround (the part that can decouple)
    # --- box (inner), EST from the published outer size less 18 mm walls and a recessed baffle
    box_w=0.729, box_h=0.799, box_d=0.326, box_q=10.0, box_fmax=3000.0,
    cone_dx=0.185, cone_dy=0.195,          # EST m, half-spacing of the cone centres
    baffle_w=0.765, baffle_h=0.835,        # published outer size
    # --- cone and cap (all EST; tuned in 5e inside the ranges of BOUNDS)
    r_coil=0.022,          # published coil diameter 44 mm
    r_cap=0.045, cap_h=0.012, cone_depth=0.060,
    c_bend=160.0,          # m/s, bending phase speed on the cone at 1 kHz
    gamma=0.5,             # dispersion exponent (0.5 = thin plate)
    eta=0.06, eta_slope=0.2,               # loss factor at 1 kHz and its exponent in frequency (paper: a few percent)
    f_ring=900.0,          # Hz, ring frequency at the rim; rises as 1/r toward the coil
    sur_mass=0.010,        # m, surround mass as an equivalent length of cone
    sur_res=0.6,           # surround resistance relative to the matched value (1 = absorbs the wave)
    f_dome=4000.0, c_cap=120.0, eta_cap=0.10,   # dust cap: a shallow dome with its own bending waves (round 2)
    irregularity=0.06,     # rms sector-to-sector spread of bending speed (the seeded layer)
    seed=1,
    # --- inside of the box above its modal range: a diffuse field that leaks back out through the cones (round 2)
    wall_alpha=0.08, alpha_slope=0.3,      # EST absorption coefficient of the bare walls at 1 kHz and its exponent
    late_gain=1.0,         # coupling of the transmitted field to a close microphone (1 = plane-wave estimate)
    late=1.0,
    # --- surroundings
    floor=1.0, floor_h=0.61, floor_beta=0.8,   # EST: miked cone centre above the floor, floor reflection factor
    edges=1.0,
)

# ranges for tuning (5e): physical meaning and a plausible span for each; nothing else is ever free
BOUNDS = dict(
    # round 3: ranges reviewed after eight values ended at a bound in round 2. Widened only where the old limit had no
    # physical basis (ring frequency, surround mass, cap membrane frequency and loss, box Q, the late-field coupling,
    # which is an order-of-magnitude estimate); kept where it has one (loss factors, absorption slope >= 0, depths
    # that must fit the published chassis).
    mms=(0.022, 0.040), a_eff=(0.120, 0.135), cone_frac=(0.3, 0.7), box_d=(0.28, 0.345), box_q=(2.0, 30.0),
    r_cap=(0.030, 0.060), cap_h=(0.004, 0.025), cone_depth=(0.045, 0.080),
    c_bend=(80.0, 400.0), gamma=(0.3, 0.7), eta=(0.02, 0.12), eta_slope=(0.0, 0.4), f_ring=(150.0, 2500.0),
    sur_mass=(0.0, 0.06), sur_res=(0.05, 1.5), f_dome=(600.0, 9000.0), c_cap=(40.0, 300.0), eta_cap=(0.03, 0.50),
    irregularity=(0.0, 0.15), floor_h=(0.15, 0.70), floor_beta=(0.3, 1.0),
    wall_alpha=(0.02, 0.30), alpha_slope=(0.0, 0.7), late_gain=(0.1, 1.5),
)

MIC0 = {
    "57": dict(curve="mic_57type_onaxis.csv", polar="57type", r_dia=0.011, setback=0.012),    # EST diaphragm, grille depth
    "421": dict(curve="mic_421type_onaxis.csv", polar="421type", r_dia=0.016, setback=0.020),  # EST
    "58": dict(curve="mic_58type_onaxis.csv", polar="57type", r_dia=0.011, setback=0.035),     # EST: 57-type pattern, ball grille
    "ideal_p": dict(curve=None, polar=None, r_dia=0.0, setback=0.0, alpha=1.0),
    "ideal_cardioid": dict(curve=None, polar=None, r_dia=0.0, setback=0.0, alpha=0.5),
}
STAGE = {   # which layers are on in each scored stage
    "5a": dict(box_modes=False, four=False, edges=False, floor=False, breakup=False, late=False, mic="ideal_p"),
    "5b": dict(box_modes=True, four=True, edges=True, floor=True, breakup=False, late=False, mic="ideal_cardioid"),
    "5c": dict(box_modes=True, four=True, edges=True, floor=True, breakup=False, late=False, mic=None),
    "5d": dict(box_modes=True, four=True, edges=True, floor=True, breakup=True, late=False, mic=None),
    "r2a": dict(box_modes=True, four=True, edges=True, floor=True, breakup=True, late=True, mic=None),   # round 2, all layers
}
STAGE["5e"] = STAGE["5d"]
for _k in ("r2b", "r2c", "r2e", "r3"):
    STAGE[_k] = STAGE["r2a"]
STAGE["r4a"] = dict(STAGE["5d"])       # round 4: shell-model driver, no inside field
STAGE["r4b"] = dict(STAGE["r2a"])      # round 4: plus the inside field


def derive(p):
    """Round 3: the piston radius is a property of the driver (one value for every cabinet), so nothing is derived
    any more; each box sets its own in-box resonance from its volume. Kept as a hook (returns p)."""
    return p


F_COARSE = np.geomspace(15.0, 23500.0, 352)     # 33 per octave (round 3; 512 before)
F_FINE = np.fft.rfftfreq(NFFT, 1.0 / FS)


def _csv(name):
    rows = [r for r in csv.reader(l for l in (DATA / name).read_text().splitlines() if not l.startswith("#"))]
    return rows[0], rows[1:]


def curve_db(name, f):
    """Published on-axis curve at f (dB re its 1 kHz value); beyond its ends the last slope continues, bounded."""
    _, rows = _csv(name)
    x = np.log10([float(r[0]) for r in rows]); y = np.array([float(r[1]) for r in rows])
    lf = np.log10(np.maximum(f, 1.0))
    out = np.interp(lf, x, y)
    lo = np.clip((y[2] - y[0]) / (x[2] - x[0]), 0.0, 40.0)           # dB per decade going down in frequency
    hi = np.clip((y[-1] - y[-3]) / (x[-1] - x[-3]), -80.0, 0.0)
    out = np.where(lf < x[0], y[0] + lo * (lf - x[0]), out)
    out = np.where(lf > x[-1], y[-1] + hi * (lf - x[-1]), out)
    return out - np.interp(3.0, x, y)


def alpha_of(polar, f):
    _, rows = _csv("mic_polar_readings.csv")
    pts = sorted((float(r[1]), float(r[4])) for r in rows if r[0] == polar)
    return np.interp(np.log10(np.maximum(f, 1.0)), np.log10([p[0] for p in pts]), [p[1] for p in pts])


def min_phase_from_db(db):
    """Complex minimum-phase response on F_FINE for a magnitude given in dB on F_FINE."""
    full = np.concatenate([db, db[-2:0:-1]]) * np.log(10) / 20
    c = np.fft.ifft(full).real
    w = np.zeros(NFFT); w[0] = 1; w[1:NFFT // 2] = 2; w[NFFT // 2] = 1
    return np.exp(np.fft.fft(c * w))[: NFFT // 2 + 1]


# ---------------------------------------------------------------- layer 1: driver and box
def absorption(p, f):
    """(absorption area of the box in m^2, Sabine reverberation time in s, mass-law power transmission of the cone)
    at f. Walls: wall_alpha (f / 1 kHz)^alpha_slope; the four cones absorb what they let through."""
    f = np.maximum(np.asarray(f, float), 1.0)
    V = p["box_w"] * p["box_h"] * p["box_d"]
    Sw = 2 * (p["box_w"] * p["box_h"] + p["box_w"] * p["box_d"] + p["box_h"] * p["box_d"])
    Sd = np.pi * p["a_eff"] ** 2
    ms = p["cone_frac"] * p["mms"] / (np.pi * (p["a_eff"] ** 2 - p["r_coil"] ** 2))
    tau = 1 / (1 + (2 * np.pi * f * ms / (2 * RHO * C0)) ** 2)
    A = np.minimum(p["wall_alpha"] * (f / 1000.0) ** p["alpha_slope"], 0.6) * Sw + 4 * Sd * tau
    return A, 0.161 * V / A, tau


def box_impedance(p, f, modes=True, parts=False):
    """Sum over the four cones of the acoustic transfer impedance of the box seen by one cone (Pa per m^3/s).
    Round 3: the modes' damping is no longer a free Q. It follows from the same absorption that sets the
    reverberation of the box (Q = 2 pi f RT / 13.8), floored by box_q for the lowest modes where the panels,
    not the air, take the energy. parts=True also returns the bare modal sum (the standing waves alone)."""
    w = 2 * np.pi * np.maximum(f, 1e-3)
    V = p["box_w"] * p["box_h"] * p["box_d"]
    Z = 4 * RHO * C0 ** 2 / (1j * w * V)                                  # the uniform-pressure (compliance) mode
    if not modes:
        return (Z, np.zeros(len(w), complex)) if parts else Z
    L = (p["box_w"], p["box_h"], p["box_d"])
    nmax = [int(2 * p["box_fmax"] * l / C0) + 1 for l in L]
    n = np.array(np.meshgrid(*[np.arange(0, m + 1) for m in nmax], indexing="ij")).reshape(3, -1).T[1:]
    kx, ky, kz = (n * np.pi / np.array(L)).T
    fn = C0 / (2 * np.pi) * np.sqrt(kx ** 2 + ky ** 2 + kz ** 2)
    keep = (fn < p["box_fmax"]) & (n[:, 0] % 2 == 0) & (n[:, 1] % 2 == 0)    # the four cones are symmetric about the centre
    kx, ky, fn, n = kx[keep], ky[keep], fn[keep], n[keep]
    kap = np.sqrt(kx ** 2 + ky ** 2) * p["a_eff"]
    disc = np.where(kap > 1e-9, 2 * j1(np.maximum(kap, 1e-9)) / np.maximum(kap, 1e-9), 1.0)
    x0, y0 = p["box_w"] / 2 + p["cone_dx"], p["box_h"] / 2 + p["cone_dy"]
    psi = np.cos(kx * x0) * np.cos(ky * y0) * disc
    lam = np.prod(np.where(n > 0, 0.5, 1.0), axis=1)
    wn = 2 * np.pi * fn
    q = np.minimum(np.maximum(2 * np.pi * fn * absorption(p, fn)[1] / 13.8, 1.0), p["box_q"] * np.maximum(fn / 300.0, 1.0) ** 2)
    Zm = np.zeros(len(w), complex)
    for i in range(0, len(wn), 256):                                        # in slices to bound memory
        s = slice(i, i + 256)
        Zm = Zm + (1j * w[:, None] * RHO * C0 ** 2 / V * 4 * psi[s] ** 2 / lam[s]
                   / (wn[s] ** 2 - w[:, None] ** 2 + 1j * w[:, None] * wn[s] / q[s])).sum(1)
    # the modes' low-frequency limit is an air mass; the shared table already holds the in-box total, so it is taken out
    Z = Z + Zm - 1j * w * RHO * C0 ** 2 / V * (4 * psi ** 2 / lam / wn ** 2).sum()
    return (Z, Zm) if parts else Z


def driver(p, f, modes=True, shed=None):
    """(coil velocity per volt at the terminals, electrical impedance) for one cone of four in the box. shed: the
    cone's effective-mass ratio on f (1 = rigid), from cone_profile(); it makes the moving mass frequency dependent."""
    p = derive(p)
    w = 2 * np.pi * np.maximum(f, 1e-3)
    bl2 = p["mms"] / p["cp"]
    rms, ktot = bl2 / p["rp"], bl2 / p["lp"]
    if shed is not None:
        # the surround's wave resistance already acts on the coil through `shed`; the table's resistance is the
        # total at the in-box resonance, so that share is taken out of it (never below a tenth)
        fc = 1 / (2 * np.pi * np.sqrt(p["lp"] * p["cp"]))
        rs = p["cone_frac"] * p["mms"] * p["sur_res"] * p["c_bend"] * (fc / 1000.0) ** p["gamma"] * p["a_eff"] / (0.5 * (p["a_eff"] ** 2 - p["r_coil"] ** 2))
        rms = max(rms - rs, 0.1 * rms)
    S = np.pi * p["a_eff"] ** 2
    V = p["box_w"] * p["box_h"] * p["box_d"]
    ksus = p["mms"] * (2 * np.pi * p["fs_free"]) ** 2                       # round 3: suspension from the published free-air
    # resonance; the box adds its own stiffness, so each cabinet has its own in-box resonance (in_box_fc below)
    par = lambda a, b: a * b / (a + b)
    ze = p["re"] + par(1j * w * p["le1"], p["rle1"]) + par(1j * w * p["le2"], p["rle2"])
    mass = p["mms"] if shed is None else p["mms"] * (1 - p["cone_frac"] + p["cone_frac"] * shed)
    zm = 1j * w * mass + rms + ksus / (1j * w) + S ** 2 * box_impedance(p, f, modes)
    bl = np.sqrt(bl2)
    return bl / (ze * zm + bl2), ze + bl2 / zm


def in_box_fc(p):
    """In-box resonance of this cabinet (Hz): suspension plus the box's share of stiffness on the moving mass."""
    S = np.pi * p["a_eff"] ** 2
    k = p["mms"] * (2 * np.pi * p["fs_free"]) ** 2 + S ** 2 * RHO * C0 ** 2 / (p["box_w"] * p["box_h"] * p["box_d"] / 4)
    return float(np.sqrt(k / p["mms"]) / (2 * np.pi))


def free_air_fs(p):
    return float(p["fs_free"])


# ---------------------------------------------------------------- layer 3: cone and cap
NR, NC, NSEC, NANG = 32, 10, 12, 24
_cache = {}


def _irregular(rng, x, amount):
    g = sum(np.cos(np.pi * m * x + rng.uniform(0, 2 * np.pi)) / np.sqrt(m) for m in (1, 2, 3, 5, 8, 13))
    return (1 + amount * g / np.sqrt(sum(0.5 / m for m in (1, 2, 3, 5, 8, 13)))) ** 2


def cone_profile(p, f, breakup=True):
    """Axial displacement per unit coil displacement. Returns r (cone radii, coil to rim), U (len(f), NSEC, NR+1),
    the cone's effective-mass ratio seen by the coil (len(f),), rc (cap radii, centre to its rim) and Uc
    (len(f), NSEC, NC+1). The cap is glued to the cone at the node nearest r_cap and is driven there."""
    p = derive(p)
    key = (breakup, len(f), float(f[0]), float(f[-1])) + tuple(p[k] for k in ("a_eff", "r_coil", "r_cap", "c_bend", "gamma", "eta", "eta_slope",
           "f_ring", "sur_mass", "sur_res", "f_dome", "c_cap", "eta_cap", "irregularity", "seed"))
    if key in _cache:
        return _cache[key]
    r = np.linspace(p["r_coil"], p["a_eff"], NR + 1)
    icap = int(np.clip(np.argmin(np.abs(r - p["r_cap"])), 1, NR - 2))
    rc = np.linspace(0.0, r[icap], NC + 1)
    if not breakup:
        out = (r, np.ones((len(f), NSEC, NR + 1), complex), np.ones(len(f), complex), rc, np.ones((len(f), NSEC, NC + 1), complex))
        _cache[key] = out
        return out
    h = r[1] - r[0]
    w = 2 * np.pi * f
    rng = np.random.default_rng(int(p["seed"]))
    xh = (0.5 * (r[1:] + r[:-1]) - r[0]) / (r[-1] - r[0])
    U = np.empty((len(f), NSEC, NR + 1), complex)
    Uc = np.empty((len(f), NSEC, NC + 1), complex)
    eta = np.minimum(p["eta"] * (f / 1000.0) ** p["eta_slope"], 0.3)
    c2 = (p["c_bend"] * (f / 1000.0) ** p["gamma"]) ** 2 * (1 + 1j * eta)       # (F,)
    wr2 = (2 * np.pi * p["f_ring"] * r[-1] / r) ** 2                             # (NR+1,)
    rh = 0.5 * (r[1:] + r[:-1])
    hc = rc[1] - rc[0]
    rch = 0.5 * (rc[1:] + rc[:-1])
    cc2 = (p["c_cap"] * (f / 1000.0) ** 0.5) ** 2 * (1 + 1j * p["eta_cap"])       # thin dome: plate dispersion
    wd2 = (2 * np.pi * p["f_dome"]) ** 2
    for s in range(NSEC):
        mult = _irregular(rng, xh, p["irregularity"])
        A = np.zeros((len(f), NR, NR), complex)                                  # unknowns u_1..u_NR (u_0 = 0)
        b = np.zeros((len(f), NR), complex)
        for i in range(1, NR):
            lo = c2 * mult[i - 1] * rh[i - 1] / (r[i] * h * h)
            hi = c2 * mult[i] * rh[i] / (r[i] * h * h)
            A[:, i - 1, i - 1] = -(lo + hi) + w ** 2 - wr2[i] * (1 + 1j * eta)
            if i > 1:
                A[:, i - 1, i - 2] = lo
            A[:, i - 1, i] = hi
            b[:, i - 1] = -w ** 2
        # rim node: half cell, flux into the surround  c2 u' = -(-w^2 mu + j w zeta)(u + 1), zeta relative to c_bend(f)
        lo = c2 * mult[-1] * rh[-1] / (r[-1] * h * h / 2)
        term = (-w ** 2 * p["sur_mass"] + 1j * w * p["sur_res"] * np.sqrt(np.abs(c2))) / (h / 2)
        A[:, NR - 1, NR - 1] = -lo + w ** 2 - wr2[-1] * (1 + 1j * eta) - term
        A[:, NR - 1, NR - 2] = lo
        b[:, NR - 1] = -w ** 2 + term
        U[:, s, 0] = 1.0
        U[:, s, 1:] = 1.0 + np.linalg.solve(A, b[..., None])[..., 0]
        # cap: unknowns u_0..u_{NC-1} relative to its rim (u_NC = 0), regular at the centre; dome membrane stiffness wd2
        multc = _irregular(rng, rch / rc[-1], p["irregularity"])
        Ac = np.zeros((len(f), NC, NC), complex)
        bc = np.full((len(f), NC), 1.0, complex) * (-w ** 2)[:, None]
        Ac[:, 0, 0] = -4 * cc2 * multc[0] / (hc * hc) + w ** 2 - wd2 * (1 + 1j * p["eta_cap"])   # axis: laplacian = 4 (u1 - u0) / h^2
        Ac[:, 0, 1] = 4 * cc2 * multc[0] / (hc * hc)
        for i in range(1, NC):
            lo = cc2 * multc[i - 1] * rch[i - 1] / (rc[i] * hc * hc)
            hi = cc2 * multc[i] * rch[i] / (rc[i] * hc * hc)
            Ac[:, i, i] = -(lo + hi) + w ** 2 - wd2 * (1 + 1j * p["eta_cap"])
            Ac[:, i, i - 1] = lo
            if i < NC - 1:
                Ac[:, i, i + 1] = hi
        uc = np.linalg.solve(Ac, bc[..., None])[..., 0]
        Uc[:, s, :NC] = U[:, s, icap][:, None] * (1.0 + uc)
        Uc[:, s, NC] = U[:, s, icap]
    # force the coil must supply, as a mass ratio: inertia of every ring plus the surround's reaction at the rim
    wt = r * h
    wt[0] *= 0.5; wt[-1] *= 0.5
    Um = U.mean(1)
    rim = (p["sur_mass"] - 1j * p["sur_res"] * np.sqrt(np.abs(c2)) / w) * r[-1] * Um[:, -1]
    shed = ((Um * wt).sum(1) + rim) / (wt.sum() + p["sur_mass"] * r[-1])
    out = (r, U, shed, rc, Uc)
    if len(_cache) > 8:
        _cache.clear()
    _cache[key] = out
    return out


def source_points(p, f, breakup=True, coarse=False):
    """Radiating points of one cone (positions relative to its centre at the rim plane, z toward the listener), their
    volume displacement per unit coil displacement and their areas: pos (M,3), q (len(f), M), dS (M,); and the mass ratio."""
    p = derive(p)
    r, U, shed, rc, Uc = cone_profile(p, f, breakup)
    nang = 8 if coarse else NANG
    icap = int(np.argmin(np.abs(r - rc[-1])))
    zc = lambda rad: -p["cone_depth"] * (r[-1] - rad) / (r[-1] - r[0])
    phi = (np.arange(nang) + 0.5) * 2 * np.pi / nang
    sec = (np.arange(nang) * NSEC) // nang
    pos, q, dS = [], [], []
    hc = rc[1] - rc[0]
    for k in range(0, NC, 3 if coarse else 1):                                   # cap rings at node mid-points
        rad = 0.5 * (rc[k] + rc[min(k + (3 if coarse else 1), NC)])
        width = rc[min(k + (3 if coarse else 1), NC)] - rc[k]
        z = zc(rc[-1]) + p["cap_h"] * (1 - (rad / rc[-1]) ** 2)
        for a, s in zip(phi, sec):
            pos.append((rad * np.cos(a), rad * np.sin(a), z))
            area = rad * width * (2 * np.pi / nang)
            q.append(0.5 * (Uc[:, s, k] + Uc[:, s, min(k + 1, NC)]) * area); dS.append(area)
    step = 4 if coarse else 1
    h = (r[1] - r[0])
    for i in range(icap + (step // 2 if coarse else 0), NR + 1, step):
        edge = 0.5 if (i == NR or i == icap) and not coarse else 1.0
        for a, s in zip(phi, sec):
            pos.append((r[i] * np.cos(a), r[i] * np.sin(a), zc(r[i])))
            area = r[i] * h * step * edge * (2 * np.pi / nang)
            q.append(U[:, s, i] * area); dS.append(area)
    return np.array(pos), np.array(q).T, np.array(dS), shed


# ---------------------------------------------------------------- layer 2: field and microphones
SOURCE_HOOK = None      # round 4 (model4.py) supplies the radiating points from its shell model through this hook


def _diaphragm(mic, x, dist, angle_deg):
    """Points and weights on the diaphragm, and the unit vector b from the microphone's front to its back."""
    th = np.radians(angle_deg)
    b = np.array([np.sin(th), 0.0, np.cos(th)])
    c = np.array([x, 0.0, dist + mic["setback"]])
    if mic["r_dia"] <= 0:
        return c[None, :], np.ones(1), b
    e1 = np.array([np.cos(th), 0.0, -np.sin(th)]); e2 = np.array([0.0, 1.0, 0.0])
    rr = mic["r_dia"] * np.sqrt(2.0 / 3.0)
    pts = [c] + [c + rr * (np.cos(a) * e1 + np.sin(a) * e2) for a in np.arange(6) * np.pi / 3]
    return np.array(pts), np.array([0.25] + [0.125] * 6), b


def _field(pos, q, pts, wts, b, f, alpha):
    """Microphone signal per unit coil displacement for sources (pos, q): alpha p + (1 - alpha) rho c u.b, averaged
    over the diaphragm points. Returns (F,). The common factor (j w)^2 rho / (2 pi) is applied by the caller."""
    k = 2 * np.pi * f / C0
    out = np.zeros(len(f), complex)
    for pt, wt in zip(pts, wts):
        d = pt[None, :] - pos                                                   # (M,3) source to microphone
        R = np.linalg.norm(d, axis=1)
        cosb = d @ b / R
        G = np.exp(-1j * k[:, None] * R[None, :]) / R[None, :]                  # (F,M)
        grad = (1 + 1 / (1j * k[:, None] * R[None, :])) * cosb[None, :]         # rho c u.b / p per source
        out += wt * ((alpha[:, None] + (1 - alpha[:, None]) * grad) * G * q).sum(1)
    return out


def transfer(p, mic_name, x, dist=0.025, angle=0.0, stage="5d", f=F_COARSE, parts=False, mic_over=None, fast=False, only_floor=False):
    """Microphone volts per unit coil *velocity* on the coarse grid, split into (direct, floor) with the bulk delay
    of each removed: returns [(H, delay_s), ...]."""
    p = derive(p)
    st = dict(STAGE[stage])
    mic = dict(MIC0[st["mic"] or mic_name]); mic.update(mic_over or {})
    alpha = np.full(len(f), mic["alpha"]) if "alpha" in mic else alpha_of(mic["polar"], f)
    pts, wts, b = _diaphragm(mic, x, dist, angle)
    w = 2 * np.pi * f
    pre = 1j * w * RHO / (2 * np.pi)                 # p = j w rho/(2 pi) * sum(v dS G); v = coil velocity * profile
    _sp = SOURCE_HOOK or source_points
    pos, q, dS, shed = _sp(p, f, st["breakup"])
    tau_d = float(np.linalg.norm(pts[0] - pos, axis=1).min() / C0)
    # round 4 (fast=True): sources farther than the miked cone are heard at the diaphragm's centre only (its size does
    # not matter at those distances); only_floor=True computes just the floor image (the rest is cached by the caller)
    pts_far, wts_far = (pts[:1], np.ones(1)) if fast else (pts, wts)
    H = np.zeros(len(f), complex) if only_floor else _field(pos, q, pts, wts, b, f, alpha)
    cpos, cq, _, _ = _sp(p, f, st["breakup"], coarse=True)
    others = [(-2 * p["cone_dx"], 0.0), (0.0, -2 * p["cone_dy"]), (-2 * p["cone_dx"], -2 * p["cone_dy"])] if st["four"] else []
    for ox, oy in ([] if only_floor else others):
        H = H + _field(cpos + np.array([ox, oy, 0.0]), cq, pts_far, wts_far, b, f, alpha)
    cones = [(0.0, 0.0)] + others
    if st["edges"] and p["edges"] > 0 and not only_floor:
        # baffle edge: perimeter sources of strength -1/2 (d_theta / 2 pi) of each cone's far-field monopole
        k = w / C0
        bw, bh = p["baffle_w"], p["baffle_h"]
        cx, cy = -p["cone_dx"], -p["cone_dy"]                                   # baffle centre in near-cone coordinates
        n = 24
        t = (np.arange(n) + 0.5) / n
        per = np.concatenate([np.stack([cx - bw / 2 + bw * t, np.full(n, cy - bh / 2)], 1), np.stack([cx - bw / 2 + bw * t, np.full(n, cy + bh / 2)], 1),
                              np.stack([np.full(n, cx - bw / 2), cy - bh / 2 + bh * t], 1), np.stack([np.full(n, cx + bw / 2), cy - bh / 2 + bh * t], 1)])
        Q = cq.sum(1)                                                           # volume displacement of one cone
        ka = k * p["a_eff"]
        direc = np.where(ka > 1e-6, 2 * j1(np.maximum(ka, 1e-6)) / np.maximum(ka, 1e-6), 1.0)   # a piston seen at 90 degrees
        E = np.zeros(len(f), complex)
        for ox, oy in cones:
            v = per - np.array([ox, oy])
            ang = np.unwrap(np.arctan2(v[:, 1], v[:, 0]).reshape(4, n), axis=1)
            dth = np.abs(np.gradient(ang, axis=1)).reshape(-1)
            re_ = np.linalg.norm(v, axis=1)
            for pt, wt in zip(pts_far, wts_far):
                dv = pt[None, :] - np.concatenate([per, np.zeros((len(per), 1))], 1)
                d = np.linalg.norm(dv, axis=1)
                grad = (1 + 1 / (1j * k[:, None] * d[None, :])) * (dv @ b / d)[None, :]
                E += wt * (-0.5 * p["edges"] * (dth / (2 * np.pi))[None, :] * (alpha[:, None] + (1 - alpha[:, None]) * grad)
                           * np.exp(-1j * k[:, None] * (re_ + d)[None, :]) / d[None, :]).sum(1) * Q * direc
        H = H + E
    out = [(pre * H * np.exp(1j * w * tau_d), tau_d)]
    if st["floor"] and p["floor"] > 0:
        # image of the microphone below the floor (y is up; the miked cone is floor_h above the floor)
        mp = pts_far.copy(); mp[:, 1] = -2 * p["floor_h"] - mp[:, 1]
        bm = b * np.array([1.0, -1.0, 1.0])
        Hf = sum(_field(cpos + np.array([ox, oy, 0.0]), cq, mp, wts_far, bm, f, alpha) for ox, oy in cones)
        tau_f = float(np.linalg.norm(mp[0] - cpos, axis=1).min() / C0)
        out.append((pre * p["floor"] * p["floor_beta"] * Hf * np.exp(1j * w * tau_f), tau_f))
    power = RHO * C0 * (np.abs(q) ** 2 / dS[None, :]).sum(1)                   # power one cone radiates to one side per (coil velocity)^2
    return (out, mic, shed, power) if parts else out


def _to_fine(H):
    """Complex interpolation from F_COARSE to F_FINE through log magnitude and unwrapped phase."""
    lf = np.log(np.maximum(F_FINE, F_COARSE[0]))
    mag = np.interp(lf, np.log(F_COARSE), np.log(np.abs(H) + 1e-300))
    ph = np.interp(lf, np.log(F_COARSE), np.unwrap(np.angle(H)))
    return np.exp(mag + 1j * ph)


def late_field(p, power_fine, vel):
    """The inside of the box above its modal range, as a diffuse field that leaks out through the cones.

    Energy balance: the four cones radiate into the box what they radiate outward (power_fine, per unit coil velocity,
    one cone); the reverberant pressure is p^2 = 4 W rho c / A with A the absorption area of the walls plus what the
    cones let through; a close microphone hears it through the cone's mass-law transmission. Its decay is Sabine's
    with the same A. The arrival is not earlier than the first return from the back wall. The phase is a seeded
    random field: this is a statistical layer, it has the density and decay of the real thing and not its pattern."""
    p = derive(p)
    f = np.maximum(F_FINE, 1.0)
    A, rt, tau = absorption(p, f)
    ka = 2 * np.pi * f / C0 * p["a_eff"]
    sigma = ka ** 2 / (1 + ka ** 2)
    p2 = 4 * RHO * C0 * (4 * sigma * power_fine) / A * tau * p["late_gain"] ** 2 * np.abs(vel) ** 2
    lo, hi = 0.67 * p["box_fmax"], p["box_fmax"]                             # below this the standing waves are explicit
    p2 = p2 * np.clip(np.log(f / lo) / np.log(hi / lo), 0, 1) ** 2
    t0 = 2 * p["box_d"] / C0
    rng = np.random.default_rng(int(p["seed"]) + 1000)
    g = rng.standard_normal(NTAPS)
    G = np.fft.rfft(g, NFFT)
    t = np.arange(NFFT) / FS
    centres = 300.0 * 2 ** (0.5 * np.arange(13))
    lf = np.log2(f)
    n = np.zeros(NFFT)
    for c in centres:
        m = np.cos(np.clip((lf - np.log2(c)) / 0.5, -1, 1) * np.pi / 2) ** 2     # half-octave raised-cosine partition
        n += np.fft.irfft(G * m, NFFT) * np.exp(-6.91 * t / np.interp(c, f, rt))
    N = np.fft.rfft(n)
    P = np.abs(N) ** 2
    cs = np.concatenate([[0.0], np.cumsum(P)])
    lo = np.searchsorted(F_FINE, F_FINE * 2 ** (-1 / 6)); hi = np.maximum(np.searchsorted(F_FINE, F_FINE * 2 ** (1 / 6)), lo + 1)
    sm = (cs[np.minimum(hi, len(P))] - cs[lo]) / (hi - lo)
    return N / np.sqrt(sm + 1e-300) * np.sqrt(p2) * np.exp(-2j * np.pi * F_FINE * t0), rt


def response(p, mic_name, x, dist=0.025, angle=0.0, stage="5d", mic_over=None):
    """Complex response on F_FINE from terminal volts to microphone volts (arbitrary level), direct sound at t = 0.3 ms."""
    p = derive(p)
    parts, mic, shed, power = transfer(p, mic_name, x, dist, angle, stage, parts=True, mic_over=mic_over)
    vel, _ = driver(p, F_FINE, STAGE[stage]["box_modes"], _to_fine(shed))
    w = 2 * np.pi * F_FINE
    t0 = parts[0][1]
    H = sum(_to_fine(Hp) * np.exp(-1j * w * (tau - t0 + 0.0003)) for Hp, tau in parts) * vel
    if STAGE[stage].get("late") and p["late"] > 0:
        # the standing waves of the box, heard through the paper: pressure behind the cone (outward motion lowers it)
        # times the cone's mass-law transmission. Deterministic: it depends on the box and on where the cones sit.
        Zm = box_impedance(p, F_FINE, True, parts=True)[1]
        fo = 0.67 * p["box_fmax"]
        ms = p["cone_frac"] * p["mms"] / (np.pi * (p["a_eff"] ** 2 - p["r_coil"] ** 2))
        thru = 1 / (1 + 1j * w * ms / (2 * RHO * C0))
        fade = 1 - np.clip(np.log(np.maximum(F_FINE, 1.0) / fo) / np.log(p["box_fmax"] / fo), 0, 1) ** 2
        H = H - p["late"] * p["late_gain"] * thru * np.pi * p["a_eff"] ** 2 * Zm * vel * np.sqrt(fade) * np.exp(-1j * w * 0.0003)
        pw = np.exp(np.interp(np.log(np.maximum(F_FINE, F_COARSE[0])), np.log(F_COARSE), np.log(power + 1e-300)))
        H = H + p["late"] * late_field(p, pw, vel)[0] * np.exp(-1j * w * 0.0003)
    if mic.get("curve"):
        H = H * min_phase_from_db(curve_db(mic["curve"], F_FINE))
    H[0] = 0.0
    edge = 0.5 * (1 + np.cos(np.pi * np.clip((F_FINE - 20000.0) / 3000.0, 0, 1)))    # band limit for the 48 kHz response
    return H * edge


def impulse_response(p, mic_name, x, dist=0.025, angle=0.0, stage="5d", mic_over=None):
    h = np.fft.irfft(response(p, mic_name, x, dist, angle, stage, mic_over), NFFT)[:NTAPS].copy()
    h[-512:] *= 0.5 * (1 + np.cos(np.linspace(0, np.pi, 512)))
    return h / np.abs(h).max()
