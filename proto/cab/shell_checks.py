"""Checks of the shell elements against closed forms (no measurement involved).  $BLOCKAMP_PY proto/cab/shell_checks.py"""
import sys
from pathlib import Path
import numpy as np
from scipy.linalg import eigh
sys.path.insert(0, str(Path(__file__).resolve().parent))
import shell as S

E, h, rho, nu = 3.0e9, 0.4e-3, 650.0, 0.3
D = E * h ** 3 / (12 * (1 - nu ** 2))

# 1. clamped flat circular plate: f = lambda^2 / (2 pi a^2) sqrt(D / rho h), lambda^2 = 10.2158, 39.771, 89.104 (axisymmetric)
a = 0.05
for n in (12, 24, 48):
    m = S.Mesh()
    ids = m.line([(0.0005 + (a - 0.0005) * i / n, 0.0) for i in range(n + 1)], "p", E, h, rho)
    m.fix(ids[0], [0, 2]); m.fix(ids[-1], [0, 1, 2])
    Ks, M = m.assemble()
    wz = [d for d in m.free]
    ev = eigh(Ks["p"][np.ix_(wz, wz)], M[np.ix_(wz, wz)], eigvals_only=True)
    fn = np.sqrt(np.abs(ev)) / (2 * np.pi)
    th = np.array([10.2158, 39.771, 89.104]) / (2 * np.pi * a ** 2) * np.sqrt(D / (rho * h))
    bend = [fn[np.argmin(np.abs(fn - t))] for t in th]
    print(f"clamped plate, {n:>2} elements: first three axisymmetric bending modes {np.round(bend, 1)} Hz, closed form {np.round(th, 1)} Hz,"
          f" error {np.round(100 * (np.array(bend) / th - 1), 2)} %")

# 2. thin ring / short cylinder breathing mode: f = (1 / 2 pi R) sqrt(E / rho) for a free short ring (hoop stress only)
R, Lc = 0.05, 0.004
m = S.Mesh()
ids = m.line([(R, Lc * i / 2) for i in range(3)], "c", E, h, rho)
Ks, M = m.assemble()
ev = eigh(Ks["c"], M, eigvals_only=True)
fn = np.sqrt(np.abs(ev)) / (2 * np.pi)
th = np.sqrt(E / rho) / (2 * np.pi * R)
print(f"free ring breathing mode: {fn[np.argmin(np.abs(fn - th))]:.1f} Hz, closed form {th:.1f} Hz; rigid-body modes at {np.round(fn[:2], 3)} Hz")

# 3. mass: a stiff free cone driven axially at low frequency moves as its mass, F = -w^2 m x
m = S.Mesh()
pts = [(0.022 + 0.1 * i / 20, -0.06 + 0.06 * i / 20) for i in range(21)]
ids = m.line(pts, "k", 1e13, h, rho)
Ks, M = m.assemble()
x = S.solve(Ks, M, m.free, {"k": 0.0}, [50.0], 3 * ids[0] + 1)
slant = np.hypot(0.1, 0.06)
mass = rho * h * np.pi * (0.022 + 0.122) * slant
print(f"rigid cone: mass from the response {abs(1 / ((2 * np.pi * 50) ** 2 * x[0, 3 * ids[0] + 1])) * 1e3:.3f} g, from geometry {mass * 1e3:.3f} g")
area, nr, nz = m.radiators(ids)
print(f"radiator bookkeeping: summed ring area {area.sum() * 1e4:.2f} cm2 (frustum {np.pi * (0.022 + 0.122) * slant * 1e4:.2f}); projected {np.sum(area * nz) * 1e4:.2f} cm2 (annulus {np.pi * (0.122 ** 2 - 0.022 ** 2) * 1e4:.2f})")
