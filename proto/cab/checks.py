"""Limit checks of the cabinet model against physics and published data (no reference pack). Runs on the current
(round-3) model.py; earlier rounds' numbers came from earlier versions of the model, which are not published.

    $BLOCKAMP_PY proto/cab/checks.py
"""
import csv, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent))
import model as M

f = M.F_COARSE
p = dict(M.P0)
db = lambda x: 20 * np.log10(np.abs(x) + 1e-300)
rows = lambda name: np.array([[float(v) for v in r[:2]] for r in M._csv(name)[1]])

print("5a  driver and box")
par = lambda a, b: a * b / (a + b)
w = 2j * np.pi * M.F_FINE[1:]
head = p["re"] + par(w * p["le1"], p["rle1"]) + par(w * p["le2"], p["rle2"]) + 1 / (1 / p["rp"] + 1 / (w * p["lp"]) + w * p["cp"])
m = (M.F_FINE[1:] >= 20) & (M.F_FINE[1:] <= 20000)
for modes in (False, True):
    z = M.driver(p, M.F_FINE[1:], modes)[1]
    d = db(z) - db(head)
    k = np.argmax(np.abs(d[m]))
    print(f"  impedance against the head's load, box {'modal ' if modes else 'lumped'}: worst {np.abs(d[m]).max():.2f} dB at {M.F_FINE[1:][m][k]:.0f} Hz"
          f"   (G-C8 limit 1 dB){'  <- identical by construction' if not modes else ''}")
band = np.array([[float(v) for v in r] for r in M._csv("driver_8ohm_impedance_band.csv")[1]])
z = np.abs(M.driver(p, band[:, 0], True)[1]) * 7.3 / 12.9
inside = (z >= band[:, 1] * 0.97) & (z <= band[:, 3] * 1.03)
print(f"  impedance against the published family band (scaled to 8 ohm), 200 Hz to 18 kHz: inside at {inside.sum()} of {len(z)} points;"
      f" rms {np.sqrt(np.mean((db(z) - db(band[:, 2])) ** 2)):.2f} dB from the median")
print(f"  free-air resonance implied by the shared table: {M.free_air_fs(p):.1f} Hz (published 75 Hz); in box {1 / (2 * np.pi * np.sqrt(p['lp'] * p['cp'])):.1f} Hz")
print(f"  force factor implied: {np.sqrt(p['mms'] / p['cp']):.1f} T m (16 ohm coil), mechanical resistance {p['mms'] / p['cp'] / p['rp']:.2f} kg/s  [mass is EST]")

print("5b  field")
flat = dict(p, cone_depth=0.0, cap_h=0.0)
st = dict(M.STAGE["5a"]); M.STAGE["chk"] = st
for z0 in (0.025, 0.1, 0.3):
    H = M.transfer(flat, "57", 0.0, z0, stage="chk")[0][0]
    k = 2 * np.pi * f / M.C0; a = M.derive(p)["a_eff"]
    # rigid annulus-free piston of radius a: |p| = 2 rho c v |sin(k/2 (sqrt(z^2+a^2) - z))|
    th = 2 * M.RHO * M.C0 * np.abs(np.sin(k / 2 * (np.sqrt(z0 ** 2 + a ** 2) - z0)))
    mm = (f < 6000) & (db(th) > db(th).max() - 12)          # away from the axial nulls, where a dB error has no meaning
    print(f"  on-axis pressure of a flat rigid piston at {z0 * 1e3:.0f} mm against the closed form, to 6 kHz, within 12 dB of its maximum:"
          f" worst {np.abs(db(H[mm]) - db(th[mm])).max():.2f} dB")
M.STAGE["chk_c"] = dict(st, mic="ideal_cardioid")
for z0 in (0.025, 0.05, 0.1, 0.3):
    g = db(M.transfer(flat, "57", 0.0, z0, stage="chk_c")[0][0]) - db(M.transfer(flat, "57", 0.0, z0, stage="chk")[0][0])
    print(f"  ideal cardioid against pressure at {z0 * 1e3:.0f} mm from the piston: {np.interp(100, f, g):+.1f} dB at 100 Hz, {np.interp(1000, f, g):+.1f} dB at 1 kHz")
pub = rows("driver_ref_onaxis.csv")
for name, stg in (("rigid cone", "chk"), ("with breakup (untuned)", "chk_b")):
    M.STAGE["chk_b"] = dict(st, breakup=True)
    parts, _, shed, _ = M.transfer(p, "57", 0.0, 1.0, stage=stg, parts=True)
    H = parts[0][0] * M.driver(p, f, False, shed)[0]
    d = np.interp(np.log(pub[:, 0]), np.log(f), db(H)) - pub[:, 1]
    for lo, hi in ((100, 1000), (1000, 5000), (5000, 10000)):
        mm = (pub[:, 0] >= lo) & (pub[:, 0] < hi)
        d0 = d[(pub[:, 0] >= 200) & (pub[:, 0] < 1000)].mean()
        print(f"  far field at 1 m, {name}, against the published driver curve {lo}-{hi} Hz: rms {np.sqrt(np.mean((d[mm] - d0) ** 2)):.1f} dB, mean {np.mean(d[mm] - d0):+.1f} dB")
    print("    (the published curve states neither distance nor baffle; level aligned over 200 Hz to 1 kHz)")

print("5c  microphones")
for mic in ("57", "421", "58"):
    M.STAGE["chk_m"] = dict(st, mic=None)
    far = dict(p, cone_depth=0.0, cap_h=0.0)
    H = M.transfer(far, mic, 0.0, 8.0, stage="chk_m")[0][0] / M.transfer(far, mic, 0.0, 8.0, stage="chk")[0][0]
    cur = M.curve_db(M.MIC0[mic]["curve"], f)
    tot = db(H) + cur
    c = rows(M.MIC0[mic]["curve"])
    mm = c[:, 0] <= 12000
    err = np.interp(np.log(c[mm, 0]), np.log(f), tot) - (c[mm, 1] - np.interp(3.0, np.log10(c[:, 0]), c[:, 1]))
    near = db(M.transfer(flat, mic, 0.0, 0.025, stage="chk_m")[0][0]) - db(M.transfer(flat, mic, 0.0, 0.025, stage="chk")[0][0])
    off = db(M.transfer(flat, mic, 0.06, 0.025, stage="chk_m")[0][0]) - db(M.transfer(flat, mic, 0.06, 0.025, stage="chk")[0][0])
    print(f"  {mic}-type: plane-wave response against its published curve to 12 kHz: worst {np.abs(err).max():.2f} dB;"
          f" proximity at 25 mm from a piston: {np.interp(100, f, near):+.1f} dB at 100 Hz;"
          f" diaphragm and pattern at 8 kHz, 60 mm off centre: {np.interp(8000, f, off):+.1f} dB")
