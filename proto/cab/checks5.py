"""Checks of the round-5 layers against limits and closed forms (no measurement involved).  $BLOCKAMP_PY proto/cab/checks5.py"""
import sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent))
import model as M, model4 as M4, model5 as M5

p = {**M.P0, **M4.P4, **M5.P5}
q, d = M4.full(p)
f = M.F_FINE
band = (f > 30) & (f < 2900)
off = dict(interior=False, panels=False, room=False, harmonics=False)

# 1. with every round-5 layer off the box is round 4's
Z4 = M.box_impedance(q, f, True)
Z5 = M5.box5(q, off, d["m_tot"])["Z"]
print(f"1. layers off: box impedance equals round 4's within {np.max(np.abs(Z5[band] - Z4[band]) / np.abs(Z4[band])):.1e}")

# 2. plates made very stiff and heavy: the coupled solution returns to the rigid box (reaction force aside: drivers weightless cone)
stiff = dict(q, E_ply=9e13, rho_ply=6.8e5)
a = M5.box5(stiff, dict(off, panels=True), 0.0)["Z"]
print(f"2. stiff, heavy plates: coupled box equals the rigid box within {np.max(np.abs(a[band] - Z5[band]) / np.abs(Z5[band])):.1e}")

# 3. interior: volume taken by the four drivers, and the first standing waves (Hz, orders, coupling strength, shift %)
bi = M5.box5(q, dict(off, interior=True), d["m_tot"])
V = q["box_w"] * q["box_h"] * q["box_d"]
print(f"3. box {V * 1e3:.1f} l, less the drivers {bi['volume'] * 1e3:.1f} l ({100 * (1 - bi['volume'] / V):.1f} % taken)")
for row in bi["modes"][:14]:
    print("     %7.1f Hz  %s  strength %.3f  shift %+.2f %%" % row)
# closed form for the perturbation: a rigid slab of thickness t on the end wall of a tube raises the axial modes by t / L
L, t = 0.29, 0.01
kz = np.pi / L
z = np.linspace(0, t, 2001)
shift = 0.5 * np.trapezoid(np.cos(kz * z) ** 2 - np.sin(kz * z) ** 2, z) / (L / 2)
print(f"   perturbation formula on a tube shortened by {t / L:.4f}: first mode rises by {shift:.4f} (exact {L / (L - t) - 1:.4f})")

# 4. plates: lowest frequencies, and the static limit (flexible walls add compliance: |Z| at 20 Hz falls)
bp = M5.box5(q, dict(off, interior=True, panels=True), d["m_tot"])
i20 = np.argmin(np.abs(f - 20.5))
print(f"4. plates: {bp['plate_f']['count']} modes coupled; baffle with drivers {bp['plate_f']['baffle']} Hz; lowest of all {bp['plate_f']['all_lowest']} Hz")
print(f"   |Z| at 20 Hz, flexible / rigid: {abs(bp['Z'][i20]) / abs(bi['Z'][i20]):.3f}")
Dp = q["E_ply"] * q["t_ply"] ** 3 / (12 * 0.91)
ss = np.pi / 2 * np.sqrt(Dp / (q["rho_ply"] * q["t_ply"])) * (1 / q["box_w"] ** 2 + 1 / q["box_h"] ** 2)
print(f"   simply supported back panel, closed form (1,1): {ss:.1f} Hz; model with edge_fix 0: {M5._plate(q['box_w'], q['box_h'], dict(q, edge_fix=0.0), 3000)[0][2] / (2 * np.pi):.1f} Hz")

# 5. circumferential orders (shelln.py) against closed forms
import shell as S, shelln as SN
from scipy.linalg import eigh
E, h, rho, nu = 3.0e9, 0.4e-3, 650.0, 0.3
Dp = E * h ** 3 / (12 * (1 - nu ** 2))
a = 0.05
for n, lam2 in ((1, (21.26, 60.82)), (2, (34.88, 84.58))):       # clamped circular plate, lambda^2 for one and two nodal diameters
    m = S.Mesh()
    ids = m.line([(0.0005 + (a - 0.0005) * i / 40, 0.0) for i in range(41)], "p", E, h, rho)
    Ks, Mm = SN.assemble(m, n)
    fixed = {4 * ids[-1] + k for k in range(4)}                       # the 1 mm hole at the centre is left free
    fr = [i for i in range(Mm.shape[0]) if i not in fixed]
    ev = eigh(Ks["p"][np.ix_(fr, fr)], Mm[np.ix_(fr, fr)], eigvals_only=True)
    fn = np.sqrt(np.abs(ev)) / (2 * np.pi)
    th = np.array(lam2) / (2 * np.pi * a ** 2) * np.sqrt(Dp / (rho * h))
    got = [fn[np.argmin(np.abs(fn - t))] for t in th]
    print(f"5. clamped plate, order {n}: {np.round(got, 1)} Hz, closed form {np.round(th, 1)} Hz, error {np.round(100 * (np.array(got) / th - 1), 2)} %")
R, Lc = 0.05, 0.004                                                # free thin ring, in-plane bending: w^2 = E I n^2 (n^2-1)^2 / (rho A R^4 (n^2+1))
m = S.Mesh()
ids = m.line([(R, Lc * i / 2) for i in range(3)], "c", E, h, rho)
for n in (2, 3, 4):
    Ks, Mm = SN.assemble(m, n)
    ev = eigh(Ks["c"], Mm, eigvals_only=True)
    fn = np.sqrt(np.abs(ev)) / (2 * np.pi)
    th = np.sqrt(E * h ** 2 / 12 * n ** 2 * (n ** 2 - 1) ** 2 / (rho * R ** 4 * (n ** 2 + 1))) / (2 * np.pi)
    print(f"   free ring, order {n}: lowest elastic mode {fn[fn > 0.2 * th][0]:.2f} Hz, closed form (thin beam) {th:.2f} Hz")
