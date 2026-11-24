"""Round 5 cabinet model: round 4's shell driver (model4.py) in a cabinet that is a structure and
stands in a room. One cabinet (Trad-style closed-back 4x12), one microphone (57-type). Nothing here reads a reference.

Layers added to round 4, each behind a flag in STAGE5 so that every stage can be scored on its own:

  interior   the inside of the box as built. The four drivers are mounted from behind, so each cone, its motor and
             the air in front of the cone stand inside the rectangular volume: they take compliance volume away and
             shift every standing wave (Rayleigh's first-order perturbation, from the geometry alone), and the cone
             drives each standing wave over its real, deep surface instead of a flat disc in the baffle plane. The
             floor image is placed correctly for the lower pair of drivers (round 4 mirrored it).
  panels     the baffle, the back, the sides, top and bottom as plywood plates (simply supported shapes, an edge-fixity
             number that moves their frequencies toward the clamped values), coupled to the standing waves and to the
             uniform pressure (modal coupling after Dowell). The baffle carries the four drivers' mass and is driven by
             the reaction of the motors; its motion radiates to the microphone.
  room       first-order images of the cabinet in the wall behind it, two side walls and the ceiling, and each of
             those again in the floor. Magnitude only is ever scored (the reference is minimum phase).
  harmonics  circumferential orders n >= 1 of the cone (shelln.py), driven by the lead-out wires' mass (first-order
             perturbation of the axisymmetric motion). They radiate nothing on the axis and more off it.
  capsule    the 57-type's diaphragm depth and radius and its pressure share as free numbers (stage r5e; it did
             not move the scores and is not part of the final stage).

Every number is geometry, material or a room distance; P5 marks estimates (EST).
"""
import numpy as np
import model as M
import model4 as M4

C0, RHO = M.C0, M.RHO

P5 = dict(
    # --- box, from the published outer size 768 x 759 x 362 mm (30 1/4 x 29 7/8 x 14 1/4 in) and 18 mm birch plywood
    box_w=0.723, box_h=0.732, baffle_w=0.759, baffle_h=0.768,
    box_d=0.290,             # EST: outer depth less back, baffle and the grille recess
    cone_dx=0.180, cone_dy=0.180,   # EST half-spacing of the driver centres
    motor_r=0.070, motor_len=0.055,  # EST: magnet and plates behind the cone (published magnet depth 38 mm, overall 135 mm)
    # --- plywood (Baltic birch, 18 mm): ranges in BOUNDS5
    t_ply=0.018, rho_ply=680.0, E_ply=9.0e9, eta_ply=0.03, edge_fix=0.5,
    m_driver=4.7,            # kg, published unit weight of this driver family
    f_struct=1500.0,         # Hz: panels are coupled below this; above it the walls are rigid
    # --- room (EST; all free in the room stage)
    wall_back=0.6, wall_l=1.5, wall_r=1.5, ceil_h=2.6, room_beta=0.3,
    f_shadow=144.0,          # EST: above c / (2 pi 0.38 m) the cabinet shadows what it sends backward
    # --- lead-out wires (EST; free in the harmonics stage)
    wire_m=0.25e-3, wire_r=0.060, wire_ang=90.0, wire_half=25.0, n_harm=6,
    # --- 57-type capsule (EST as in model.MIC0; free in the capsule stage): depth of the diaphragm behind the front of
    # the grille cap, diaphragm radius, and a shift of the pressure share read from the published polar pattern
    mic_setback=0.012, mic_rdia=0.011, mic_alpha=0.0,
)
BOUNDS5 = dict(
    box_d=(0.270, 0.310), cone_dx=(0.160, 0.200), cone_dy=(0.160, 0.200),
    E_ply=(6.0e9, 12.0e9), eta_ply=(0.01, 0.08), edge_fix=(0.0, 1.0),
    wall_back=(0.05, 3.0), wall_l=(0.5, 4.0), wall_r=(0.5, 4.0), ceil_h=(2.2, 4.0), room_beta=(0.0, 0.5),
    wire_m=(0.0, 1.0e-3), wire_r=(0.030, 0.100), wire_ang=(0.0, 180.0),
    mic_setback=(0.004, 0.025), mic_rdia=(0.006, 0.013), mic_alpha=(-0.15, 0.15),
)
STAGE5 = {
    "r5a": dict(interior=True, panels=False, room=False, harmonics=False),
    "r5b": dict(interior=True, panels=True, room=False, harmonics=False),
    "r5c": dict(interior=True, panels=True, room=True, harmonics=False),
    "r5d": dict(interior=True, panels=True, room=True, harmonics=True),
    "r5e": dict(interior=True, panels=True, room=True, harmonics=True, capsule=True),
}
for _v in STAGE5.values():
    _v.setdefault("capsule", False)
_box = {}
_direct = {}


# ---------------------------------------------------------------- the inside of the box
def _cone_depth(p, rho):
    """How far the cone's surface stands inside the box at radius rho (m), rim plane at 0."""
    rc, ro, d = p["r_coil"], p["r_cone_out"], p["cone_depth"]
    t = np.clip((np.maximum(rho, rc) - rc) / (ro - rc), 0, 1)
    z = d * (1 - t) ** p["cone_curv"]
    zc = d * (1 - np.clip((p["r_cap"] - rc) / (ro - rc), 0, 1)) ** p["cone_curv"]
    return np.where(rho < p["r_cap"], zc - p["cap_h"] * (1 - (rho / p["r_cap"]) ** 2), z)


def _samples(p):
    """Points of one driver relative to its centre: surface (x, y, z, projected area) and the volume it takes out of
    the box (x, y, z, dV): the air in front of the cone and the motor behind it."""
    nr, na = 8, 8
    ro = p["r_cone_out"]
    rho = (np.arange(nr) + 0.5) * ro / nr
    phi = (np.arange(na) + 0.5) * 2 * np.pi / na
    R, PH = np.meshgrid(rho, phi, indexing="ij")
    w = np.broadcast_to((2 * np.pi * rho * ro / nr / na)[:, None], R.shape)
    zb = _cone_depth(p, R)
    surf = np.stack([R * np.cos(PH), R * np.sin(PH), zb, w], -1).reshape(-1, 4)
    vol = [np.stack([R * np.cos(PH), R * np.sin(PH), zb * fr, w * zb / 3], -1).reshape(-1, 4) for fr in (1 / 6, 0.5, 5 / 6)]
    rm = (np.arange(3) + 0.5) * p["motor_r"] / 3
    Rm, PHm = np.meshgrid(rm, phi, indexing="ij")
    wm = np.broadcast_to((2 * np.pi * rm * p["motor_r"] / 3 / na)[:, None], Rm.shape)
    for fr in (0.25, 0.75):
        vol.append(np.stack([Rm * np.cos(PHm), Rm * np.sin(PHm), np.full(Rm.shape, p["cone_depth"] + fr * p["motor_len"]), wm * p["motor_len"] / 2], -1).reshape(-1, 4))
    return surf, np.concatenate(vol)


def _plate(a, b, p, fmax, odd_a=True, odd_b=True):
    """Modes of a plate a x b: (i, j, omega). Simply supported shapes sin(i pi x / a) sin(j pi y / b); edge_fix moves
    the frequencies toward the clamped plate (index + 1/2, the usual edge-effect approximation)."""
    Dp = p["E_ply"] * p["t_ply"] ** 3 / (12 * (1 - 0.3 ** 2))
    e = 0.5 * p["edge_fix"]
    out = []
    for i in range(1, 30, 2 if odd_a else 1):
        for j in range(1, 30, 2 if odd_b else 1):
            w = np.pi ** 2 * np.sqrt(Dp / (p["rho_ply"] * p["t_ply"])) * (((i + e) / a) ** 2 + ((j + e) / b) ** 2)
            if w < 2 * np.pi * fmax:
                out.append((i, j, w))
    return out


def _cs(n, i, L):
    """Integral over 0..L of cos(n pi x / L) sin(i pi x / L)."""
    n = np.asarray(n, float)
    odd = (np.round(n + i) % 2) == 1
    return np.where(odd, 2 * L * i / (np.pi * (i * i - n * n + (~odd))), 0.0)


def box5(p, flags, m_tot):
    """The box on F_FINE for unit coil velocity of the four cones. Returns dict: Z (Pa s / m^3, summed over the four
    cones, the form model.box_impedance has), Zm (its standing-wave part), fb and vb (frequencies below f_struct and
    the baffle's modal velocities there, outward, per unit coil velocity), baffle modes (i, j), and a table of modes."""
    key = (repr(sorted(flags.items())), float(m_tot)) + tuple(float(p[k]) for k in (
        "box_w", "box_h", "box_d", "box_q", "box_fmax", "cone_dx", "cone_dy", "r_coil", "r_cone_out", "cone_depth", "cone_curv", "r_cap", "cap_h",
        "motor_r", "motor_len", "t_ply", "rho_ply", "E_ply", "eta_ply", "edge_fix", "m_driver", "f_struct", "wall_alpha", "alpha_slope",
        "a_eff", "mms", "cone_frac"))
    if key in _box:
        return _box[key]
    f = M.F_FINE
    w = 2 * np.pi * np.maximum(f, 1e-3)
    W, H, Dd = p["box_w"], p["box_h"], p["box_d"]
    V = W * H * Dd
    L = (W, H, Dd)
    nmax = [int(2 * p["box_fmax"] * l / C0) + 1 for l in L]
    n = np.array(np.meshgrid(*[np.arange(0, m + 1) for m in nmax], indexing="ij")).reshape(3, -1).T[1:]
    k = n * np.pi / np.array(L)
    fn = C0 / (2 * np.pi) * np.linalg.norm(k, axis=1)
    keep = (fn < p["box_fmax"]) & (n[:, 0] % 2 == 0) & (n[:, 1] % 2 == 0)          # the four cones are symmetric about the centre
    n, k, fn = n[keep], k[keep], fn[keep]
    lam = np.prod(np.where(n > 0, 0.5, 1.0), axis=1)
    x0, y0 = W / 2 + p["cone_dx"], H / 2 + p["cone_dy"]
    if flags["interior"]:
        surf, vol = _samples(p)
        ps = np.cos(np.outer(k[:, 0], x0 + surf[:, 0])) * np.cos(np.outer(k[:, 1], y0 + surf[:, 1])) * np.cos(np.outer(k[:, 2], surf[:, 2]))
        psi = ps @ surf[:, 3] / surf[:, 3].sum()
        cx, sx = np.cos(np.outer(k[:, 0], x0 + vol[:, 0])), np.sin(np.outer(k[:, 0], x0 + vol[:, 0]))
        cy, sy = np.cos(np.outer(k[:, 1], y0 + vol[:, 1])), np.sin(np.outer(k[:, 1], y0 + vol[:, 1]))
        cz, sz = np.cos(np.outer(k[:, 2], vol[:, 2])), np.sin(np.outer(k[:, 2], vol[:, 2]))
        grad = ((k[:, 0:1] * sx * cy * cz) ** 2 + (k[:, 1:2] * cx * sy * cz) ** 2 + (k[:, 2:3] * cx * cy * sz) ** 2) / (k ** 2).sum(1)[:, None]
        dv = 4 * vol[:, 3].sum()
        shift = np.clip(4 * (((cx * cy * cz) ** 2 - grad) @ vol[:, 3]) / (V * lam), -0.25, 0.25)
        Ve = V - dv
        fn = fn * np.sqrt(1 + shift)
    else:
        kap = np.hypot(k[:, 0], k[:, 1]) * p["a_eff"]
        disc = np.where(kap > 1e-9, 2 * M.j1(np.maximum(kap, 1e-9)) / np.maximum(kap, 1e-9), 1.0)
        psi = np.cos(k[:, 0] * x0) * np.cos(k[:, 1] * y0) * disc
        Ve, shift = V, np.zeros(len(fn))
    wn = 2 * np.pi * fn
    q = np.minimum(np.maximum(2 * np.pi * fn * M.absorption(p, fn)[1] / 13.8, 1.0), p["box_q"] * np.maximum(fn / 300.0, 1.0) ** 2)
    g = RHO * C0 ** 2 / (Ve * lam)
    Zr = np.zeros(len(w), complex)                      # rigid-wall standing waves
    for i in range(0, len(wn), 256):
        s = slice(i, i + 256)
        Zr = Zr + (1j * w[:, None] * g[s] * 4 * psi[s] ** 2 / (wn[s] ** 2 - w[:, None] ** 2 + 1j * w[:, None] * wn[s] / q[s])).sum(1)
    comp = 4 * RHO * C0 ** 2 / (1j * w * Ve)
    airmass = 1j * w * (g * 4 * psi ** 2 / wn ** 2).sum()                       # see model.box_impedance: the table holds it already
    out = dict(modes=sorted(zip(fn.round(1).tolist(), [tuple(int(i) for i in r) for r in n], (psi ** 2 / lam).round(3).tolist(), (100 * shift / 2).round(2).tolist()))[:24], volume=Ve)
    if not flags["panels"]:
        out.update(Z=comp + Zr - airmass, Zm=Zr, fb=f[:0], vb=np.zeros((0, 0), complex), bmodes=[])
        _box[key] = out
        return out
    # ---- panels: unknowns are the amplitudes of the standing waves below 1.2 f_struct (and the uniform pressure) and
    # the outward modal velocities of the plates
    fc = p["f_struct"]
    nb = int(np.searchsorted(f, fc))
    sel = np.nonzero(fn < 1.2 * fc)[0]
    na_ = np.concatenate([np.zeros((1, 3)), n[sel]])
    psa = np.concatenate([[1.0], psi[sel]])
    wna = np.concatenate([[0.0], wn[sel]])
    qa = np.concatenate([[1.0], q[sel]])
    ga = np.concatenate([[RHO * C0 ** 2 / Ve], g[sel]])
    S = np.pi * p["a_eff"] ** 2
    Cs, Mm, Km, mult, Phi, bmodes = [], [], [], [], [], []
    rh = p["rho_ply"] * p["t_ply"]
    hole = 1 - 4 * np.pi * 0.1415 ** 2 / (W * H)                               # the four cut-outs (published 283 mm)
    for name, a, b in (("baffle", W, H), ("back", W, H), ("side", H, Dd), ("top", W, Dd)):
        face = name in ("baffle", "back")
        for i, j, wp in _plate(a, b, p, 1.6 * fc, True, face):
            if face:
                c = _cs(na_[:, 0], i, W) * _cs(na_[:, 1], j, H) * (np.cos(np.pi * na_[:, 2]) if name == "back" else 1.0)
            elif name == "side":                                                # the walls at x = 0 and W: over y and z
                c = _cs(na_[:, 1], i, H) * _cs(na_[:, 2], j, Dd)
            else:
                c = _cs(na_[:, 0], i, W) * _cs(na_[:, 2], j, Dd)
            m0 = rh * a * b / 4
            if name == "baffle":
                ph = np.sin(i * np.pi * x0 / W) * np.sin(j * np.pi * y0 / H)   # the plate's shape at a driver
                Mm.append(m0 * hole + 4 * p["m_driver"] * ph ** 2); Km.append(m0 * hole * wp ** 2); Phi.append(4 * ph); bmodes.append((i, j))
            else:
                Mm.append(m0); Km.append(m0 * wp ** 2); Phi.append(0.0)
            Cs.append(c); mult.append(1.0 if face else 2.0)
    Cm = np.array(Cs).T                                                         # (acoustic, plate)
    Mm, Km, mult, Phi = map(np.array, (Mm, Km, mult, Phi))
    Na, Np = Cm.shape
    wb = w[:nb]
    A = np.zeros((nb, Na + Np, Na + Np), complex)
    ia, ip = np.arange(Na), Na + np.arange(Np)
    A[:, ia, ia] = wna[None, :] ** 2 - wb[:, None] ** 2 + 1j * wb[:, None] * np.maximum(wna, 1.0)[None, :] / qa[None, :]
    A[:, :Na, Na:] = 1j * wb[:, None, None] * (ga[:, None] * Cm * mult[None, :])[None]
    A[:, Na:, :Na] = -Cm.T[None]
    A[:, ip, ip] = 1j * wb[:, None] * Mm[None, :] + Km[None, :] * (1 + 1j * p["eta_ply"]) / (1j * wb[:, None])
    rhs = np.zeros((nb, Na + Np), complex)
    rhs[:, :Na] = -1j * wb[:, None] * (ga * 4 * S * psa)[None, :]
    rhs[:, Na:] = -(1j * wb[:, None] * m_tot) * Phi[None, :]                    # reaction of the four motors on the baffle
    sol = np.linalg.solve(A, rhs[..., None])[..., 0]
    Zc = -(sol[:, :Na] @ psa) / S
    # the uniform pressure with the plates alone (what "compliance" means once the walls give)
    i0 = np.concatenate([[0], ip])
    sol0 = np.linalg.solve(A[:, i0][:, :, i0], rhs[:, i0][..., None])[..., 0]
    Z0 = -sol0[:, 0] / S
    rest = np.ones(len(wn), bool); rest[sel] = False
    Zrest = np.zeros(nb, complex)
    for i in range(0, int(rest.sum()), 256):
        s = np.nonzero(rest)[0][i:i + 256]
        Zrest = Zrest + (1j * wb[:, None] * g[s] * 4 * psi[s] ** 2 / (wn[s] ** 2 - wb[:, None] ** 2 + 1j * wb[:, None] * wn[s] / q[s])).sum(1)
    Z = comp + Zr - airmass
    Zm = Zr.copy()
    Z[:nb] = Zc + Zrest - airmass[:nb]
    Zm[:nb] = Zc + Zrest - Z0
    nbm = len(bmodes)
    pw = np.sort(np.sqrt(Km / Mm) / (2 * np.pi))
    out.update(Z=Z, Zm=Zm, fb=f[:nb], vb=sol[:, Na:Na + nbm], bmodes=bmodes, plate_f=dict(
        baffle=np.sort(np.sqrt(Km[:nbm] / Mm[:nbm]) / (2 * np.pi))[:4].round(0).tolist(), all_lowest=pw[:8].round(0).tolist(), count=int(Np)))
    if len(_box) > 8:
        _box.clear()
    _box[key] = out
    return out


def baffle_field(p, bx, mic, x, dist, angle):
    """Microphone signal on F_FINE (zero above f_struct) from the baffle's motion, per unit coil velocity, with the
    true propagation phase from the baffle's front face."""
    nb = len(bx["fb"])
    out = np.zeros(len(M.F_FINE), complex)
    if nb == 0 or not bx["bmodes"]:
        return out
    W, H = p["box_w"], p["box_h"]
    ng = 12
    gx, gy = np.meshgrid((np.arange(ng) + 0.5) * W / ng, (np.arange(ng) + 0.5) * H / ng, indexing="ij")
    shapes = np.array([np.sin(i * np.pi * gx / W) * np.sin(j * np.pi * gy / H) for i, j in bx["bmodes"]]).reshape(len(bx["bmodes"]), -1)
    pos = np.stack([gx.ravel() - W / 2 - p["cone_dx"], gy.ravel() - H / 2 - p["cone_dy"], np.full(gx.size, p["t_ply"])], 1)
    q = (bx["vb"] @ shapes) * (W * H / ng ** 2)
    fb = np.maximum(bx["fb"], 1.0)
    alpha = M.alpha_of(mic["polar"], fb) if "alpha" not in mic else (
        np.interp(fb, M.F_COARSE, mic["alpha"]) if np.ndim(mic["alpha"]) else np.full(nb, mic["alpha"]))
    pts, wts, b = M._diaphragm(mic, x, dist, angle)
    out[:nb] = 1j * 2 * np.pi * fb * RHO / (2 * np.pi) * M._field(pos, q, pts[:1], np.ones(1), b, fb, alpha)
    out[:nb] *= 0.5 * (1 + np.cos(np.pi * np.clip((fb - 0.8 * p["f_struct"]) / (0.2 * p["f_struct"]), 0, 1)))
    return out


# ---------------------------------------------------------------- the room
def room_parts(p, q, mic, x, dist, angle, speaker, floor_y):
    """Images of the microphone in the walls and the ceiling, and each again in the floor: [(H on F_COARSE with its
    bulk delay removed, delay)], per unit coil velocity. Coordinates are model.transfer's: the miked cone's centre at
    the origin, z toward the listener, the other cones at -2 cone_dx and -2 cone_dy; floor_y is the floor's y there."""
    f = M.F_COARSE
    w = 2 * np.pi * f
    alpha = np.full(len(f), mic["alpha"]) if "alpha" in mic else M.alpha_of(mic["polar"], f)
    pts, wts, b = M._diaphragm(mic, x, dist, angle)
    cpos, cq, _, _ = M4.source_points(q, f, True, coarse=True)
    # the images are metres away: neighbouring rings and angles of each cone are merged (volume displacement summed)
    nr_ = len(cpos) // 8
    grp = (np.arange(nr_)[:, None] // 2) * 4 + (np.arange(8)[None, :] // 2)
    ng = int(grp.max()) + 1
    G = np.zeros((len(cpos), ng)); G[np.arange(len(cpos)), grp.ravel()] = 1.0
    cpos, cq = (G.T @ cpos) / G.sum(0)[:, None], cq @ G
    cones = [(0.0, 0.0), (-2 * q["cone_dx"], 0.0), (0.0, -2 * q["cone_dy"]), (-2 * q["cone_dx"], -2 * q["cone_dy"])]
    cpos = np.concatenate([cpos + np.array([ox, oy, 0.0]) for ox, oy in cones]); cq = np.tile(cq, (1, 4))
    up = 1.0 if floor_y < 0 else -1.0                                   # which way is up in these coordinates
    ceil_y = floor_y + up * p["ceil_h"]
    right = speaker[1] == "R"                                            # the miked cone sits toward +x of the cabinet's centre
    xw_near = -q["cone_dx"] + (p["wall_r"] if right else p["wall_l"])
    xw_far = -q["cone_dx"] - (p["wall_l"] if right else p["wall_r"])
    zb = -(0.362 - 0.045) - p["wall_back"]                              # the wall behind: cabinet depth behind the rim plane (EST)
    shadow = 1 / np.sqrt(1 + (f / p["f_shadow"]) ** 2)
    planes = [(0, xw_near, 1.0), (0, xw_far, 1.0), (1, ceil_y, 1.0), (2, zb, shadow)]
    pre = 1j * w * RHO / (2 * np.pi)
    out = []
    for axis, c, filt in planes:
        for second in (False, True):
            mp, bm = pts[:1].copy(), b.copy()
            mp[:, axis] = 2 * c - mp[:, axis]; bm[axis] = -bm[axis]
            coef = p["room_beta"] * filt
            if second:
                if axis == 1:
                    continue
                mp[:, 1] = 2 * floor_y - mp[:, 1]; bm[1] = -bm[1]
                coef = coef * p["floor_beta"]
            Hi = M._field(cpos, cq, mp, np.ones(1), bm, f, alpha)
            tau = float(np.linalg.norm(mp[0] - cpos, axis=1).min() / C0)
            out.append((pre * coef * Hi * np.exp(1j * w * tau), tau))
    return out


# ---------------------------------------------------------------- response
def driver5(q, d, zm_fine, bx):
    f = M.F_FINE
    w = 2 * np.pi * np.maximum(f, 1e-3)
    par = lambda a, b: a * b / (a + b)
    ze = q["re"] + par(1j * w * q["le1"], q["rle1"]) + par(1j * w * q["le2"], q["rle2"])
    zm = zm_fine + d["s_piston"] ** 2 * bx["Z"]
    bl = np.sqrt(d["bl2"])
    return bl / (ze * zm + d["bl2"]), ze + d["bl2"] / zm


def response(p, mic_name, x, dist, angle=0.0, stage="r5a", speaker="LR"):
    """Terminal volts to microphone volts on F_FINE. p["floor_h"] is the height of a LOWER cone's centre above the floor."""
    fl = STAGE5[stage]
    q, d = M4.full(p)
    lower = speaker[0] == "L"
    floor_y = p["floor_h"] if lower else -(p["floor_h"] + 2 * p["cone_dy"])     # lower pair: these coordinates are upside down
    key = (d["serial"], mic_name, float(x), float(dist), float(angle), stage, speaker[1] if fl["harmonics"] else "") + tuple(
        float(q[k]) for k in ("cone_dx", "cone_dy", "baffle_w", "baffle_h", "edges", "wire_m", "wire_r", "wire_ang", "mic_setback", "mic_rdia", "mic_alpha"))
    over = None
    if fl["capsule"]:
        over = dict(setback=p["mic_setback"], r_dia=p["mic_rdia"],
                    alpha=np.clip(M.alpha_of(M.MIC0[mic_name]["polar"], M.F_COARSE) + p["mic_alpha"], 0.0, 1.0))
    hook = M4.source_points
    if fl["harmonics"]:
        import harmonics5 as H5
        hook = H5.hook(speaker)
    M.SOURCE_HOOK = hook
    try:
        if key not in _direct:
            if len(_direct) > 40:
                _direct.clear()
            _direct[key] = M.transfer(dict(q, floor=0.0), mic_name, x, dist, angle, "r4b", parts=True, fast=True, mic_over=over)
        parts, mic, zmc, power = _direct[key]
        parts = list(parts)
        M.SOURCE_HOOK = M4.source_points
        if q["floor"] > 0:
            parts += M.transfer(dict(q, floor_h=-floor_y), mic_name, x, dist, angle, "r4b", parts=True, fast=True, only_floor=True, mic_over=over)[0][1:]
        if fl["room"] and p["room_beta"] > 0:
            parts += room_parts(p, q, mic, x, dist, angle, speaker, floor_y)
    finally:
        M.SOURCE_HOOK = None
    bx = box5(q, fl, d["m_tot"])
    vel, _ = driver5(q, d, M._to_fine(zmc), bx)
    w = 2 * np.pi * M.F_FINE
    t0 = parts[0][1]
    H = sum(M._to_fine(Hp) * np.exp(-1j * w * (tau - t0 + 0.0003)) for Hp, tau in parts) * vel
    if fl["panels"]:
        H = H + baffle_field(q, bx, mic, x, dist, angle) * vel * np.exp(1j * w * (t0 - 0.0003))
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


def impulse_response(p, mic_name, x, dist, angle=0.0, stage="r5a", speaker="LR"):
    h = np.fft.irfft(response(p, mic_name, x, dist, angle, stage, speaker), M.NFFT)[: M.NTAPS].copy()
    h[-512:] *= 0.5 * (1 + np.cos(np.linspace(0, np.pi, 512)))
    return h / np.abs(h).max()


def impedance(p, stage="r5a"):
    q, d = M4.full(p)
    return driver5(q, d, M._to_fine(d["zm"]), box5(q, STAGE5[stage], d["m_tot"]))[1]
