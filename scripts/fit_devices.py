"""Device and load fits from digitised data-sheet curves (docs/research/data/*.csv).

Run:  python scripts/fit_devices.py        (numpy and scipy only)
Prints every table quoted in docs/research/DEVICES.md. Nothing here reads the prototype: the "old"
parameter sets are typed in below as they stood before this work, so the comparison stays reproducible.
"""
from __future__ import annotations
import os
import numpy as np
from scipy.optimize import least_squares, brentq

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "research", "data")


def load(name):
    rows = [l for l in open(os.path.join(DATA, name)) if not l.startswith("#")]
    return np.array([[float(v) for v in l.split(",")] for l in rows[1:]])


def sp(x):
    return np.logaddexp(0.0, x)


def rms(e):
    return float(np.sqrt(np.mean(np.square(e))))


# ----------------------------------------------------------------------------------------- 12AX7
KOREN_12AX7 = [100.0, 1.4, 1060.0, 600.0, 300.0, 0.0]            # Koren 1996, Table 1 (vct = 0)
DZ = {"RSD-1": [2.242e-3, 103.2, 1.26, 3.40], "RSD-2": [2.173e-3, 100.2, 1.28, 3.19],
      "EHX-1": [1.371e-3, 86.9, 1.349, 4.56]}                    # Dempwolf & Zoelzer DAFx-11, Table 1 (G, mu, gamma, C)
DZ_GRID = {"RSD-1": [6.177e-4, 1.314, 9.901], "RSD-2": [5.911e-4, 1.358, 11.76],
           "EHX-1": [3.263e-4, 1.156, 11.99]}                    # same table (Gg, xi, Cg)


def koren(p, va, vg):
    """Koren triode with a contact-potential offset vct added to the grid voltage. mA."""
    mu, ex, kg1, kp, kvb, vct = p
    e1 = va / kp * sp(kp * (1.0 / mu + (vg + vct) / np.sqrt(kvb + va * va)))
    return 2e3 * e1 ** ex / kg1


def dz(p, va, vg):
    G, mu, gam, C = p
    return 1e3 * G * (sp(C * (va / mu + vg)) / C) ** gam


def tri_res(f, p, d):
    return (f(p, d[:, 1], d[:, 0]) - d[:, 2]) / (d[:, 2] + 0.1)   # relative error with a 0.1 mA floor


def loadline_err(f, p, d):
    """Plate voltage where each digitised grid curve meets a resistive load line, model minus data."""
    e = []
    for vb, rl in ((295, 100e3), (295, 220e3), (310, 220e3), (310, 100e3)):
        for vg in (0, -0.5, -1, -1.5, -2, -2.5, -3):
            s = d[d[:, 0] == vg]
            g = lambda v: np.interp(v, s[:, 1], s[:, 2]) - (vb - v) / rl * 1e3
            a, b = s[:, 1].min(), s[:, 1].max()
            if g(a) * g(b) < 0:
                e.append(brentq(lambda v: f(p, v, vg) - (vb - v) / rl * 1e3, 0.1, vb) - brentq(g, a, b))
    return np.array(e)


def triode_section():
    ge, ph = load("12ax7_ge_plate.csv"), load("ecc83_philips_plate.csv")
    both = np.vstack([ge, ph])
    lo, hi = [50, 1.0, 200, 50, 1, -1], [150, 2.0, 5000, 3000, 5000, 1.5]
    fit = least_squares(lambda p: tri_res(koren, p, both), KOREN_12AX7[:5] + [0.3], bounds=(lo, hi)).x
    l5, h5 = lo[:5] + [-1e-9], hi[:5] + [1e-9]
    fit5 = least_squares(lambda p: tri_res(koren, p, both), KOREN_12AX7, bounds=(l5, h5)).x
    cands = [("Koren 1996 published", koren, KOREN_12AX7), ("Koren refit, no offset", koren, fit5),
             ("Koren + vct refit (recommended)", koren, fit)] + [("D&Z " + k, dz, v) for k, v in DZ.items()]
    print("== 12AX7 plate current against digitised curves (GE ET-T509B p.3; Philips ECC83 1970 sheet 5)")
    print("   recommended: mu %.2f ex %.4f kg1 %.1f kp %.1f kvb %.2f vct %.4f" % tuple(fit))
    for name, f, p in cands:
        for lab, d in (("GE", ge), ("Philips", ph)):
            e = f(p, d[:, 1], d[:, 0]) - d[:, 2]
            lowv = (d[:, 0] == 0) & (d[:, 1] < 100)
            ll = loadline_err(f, p, d)
            print("   %-32s %-8s rms %.3f mA  max %.3f  rel %.0f %%  | Vg=0,Va<100: %.3f mA | Ia<0.3 mA: %.3f mA"
                  " | load lines: %.1f V rms, %.1f V max" % (name, lab, rms(e), abs(e).max(), 100 * rms(tri_res(f, p, d)),
                                                              rms(e[lowv]), rms(e[d[:, 2] < 0.3]), rms(ll), abs(ll).max()))
    print("   data-sheet points: 250 V / -2 V: 1.2 mA, 1.6 mA/V, 62.5 k, mu 100;  100 V / -1 V: 0.5 mA, 1.25 mA/V, 80 k, mu 100")
    for name, f, p in cands[:3]:
        out = []
        for va, vg in ((250.0, -2.0), (100.0, -1.0)):
            i = f(p, va, vg); gm = (f(p, va, vg + .01) - f(p, va, vg - .01)) / .02; gp = (f(p, va + 1, vg) - f(p, va - 1, vg)) / 2
            out.append("%.2f mA, %.2f mA/V, %.0f k, mu %.0f" % (i, gm, 1 / gp, gm / gp))
        print("   %-32s %s;  %s" % (name, out[0], out[1]))
    print("   cold stage (39 k cathode, 100 k plate, 295 V): bias point")
    for name, f, p in cands:
        i = brentq(lambda i: f(p, 295 - i * 139.0, -i * 39.0) - i, 1e-4, 2.0)
        print("   %-32s Ia %.0f uA, Vgk %.2f V" % (name, i * 1e3, -i * 39))
    print("   positive grid: recommended plate current against D&Z cathode minus grid current (RSD-1), mA")
    for vg in (0.5, 1.0, 2.0):
        row = []
        for va in (20.0, 50.0, 100.0, 200.0):
            ig = 1e3 * DZ_GRID["RSD-1"][0] * (sp(DZ_GRID["RSD-1"][2] * vg) / DZ_GRID["RSD-1"][2]) ** DZ_GRID["RSD-1"][1]
            row.append("%g V: %.2f / %.2f" % (va, koren(fit, va, vg), dz(DZ["RSD-1"], va, vg) - ig))
        print("   Vg %+.1f  " % vg + ";  ".join(row))
    return fit


# ----------------------------------------------------------------------------------------- 6L6GC
OLD_6L6 = dict(mu=8.7, ex=1.35, kg1=1460.0, kp=48.0, kvb=12.0, rs=0.07, asc=0.22)   # Koren 1996 + our earlier split


def space(mu, ex, kg1, kp, vg, vs):
    return 1e3 * np.pi / kg1 * (vs / kp * sp(kp * (1.0 / mu + vg / vs))) ** ex       # mA per tube


def pent_old(p, vg, vs, vp):
    i0 = space(p["mu"], p["ex"], p["kg1"], p["kp"], vg, vs)
    g = 2 / np.pi * np.arctan(sp(4 * vp) / 4 / p["kvb"])
    return i0 * g, i0 * (p["rs"] + p["asc"] * (1 - g))


NEW_NAMES = ["mu", "ex", "kg1", "kp", "kv", "lam", "rs", "asc"]
KNEE_N = 3.0


def pent_new(p, vg, vs, vp):
    """Plate current = smooth minimum of the space current and a grid-independent 3/2-power line kv*Vp^1.5
    (the virtual-cathode limit below the knee), times a small plate-voltage slope. Screen = share above the knee
    plus a part that grows as 1/(1+w), w = kv*Vp^1.5/I0. kv in mA/V^1.5 here."""
    mu, ex, kg1, kp, kv, lam, rs, asc = p
    i0 = space(mu, ex, kg1, kp, vg, vs)
    vps = sp(4 * vp) / 4
    w = kv * vps * np.sqrt(vps) / (i0 + 1e-9)
    g = w / (1 + w ** KNEE_N) ** (1 / KNEE_N)
    return i0 * g * (1 + lam * vps), i0 * (rs + asc / (1 + w))


def pent_data():
    rows = []
    for vg, vp, ip in load("6l6gc_tungsol_plate_g2_400.csv"): rows.append((vg, 400, vp, ip, np.nan, 0))
    for vs, vg, ip in load("6l6gc_tungsol_transfer_vp_400.csv"): rows.append((vg, vs, 400, ip, np.nan, 1))
    for vs, vp, ip in load("6l6gc_tungsol_plate_g1_0.csv"): rows.append((0, vs, vp, ip, np.nan, 2))
    for vg, vp, i2 in load("6l6gc_tungsol_ig2_g2_400.csv"): rows.append((vg, 400, vp, np.nan, i2, 3))
    for vs, vp, i2 in load("6l6gc_tungsol_ig2_g1_0.csv"): rows.append((0, vs, vp, np.nan, i2, 4))
    return np.array(rows).T


def push_pull(model, p, vb, vs, bias, vpk, raa):
    """Static class AB1 pair on a resistive plate-to-plate load: power, average plate and screen currents."""
    V, IA, IS = [], [], []
    for t in np.linspace(0, 2 * np.pi, 361)[:-1]:
        ga, gb = bias + vpk * np.sin(t), bias - vpk * np.sin(t)
        v = brentq(lambda v: (model(p, ga, vs, vb - v)[0] - model(p, gb, vs, vb + v)[0]) * 1e-3 * raa / 4 - v, -1.2 * vb, 1.2 * vb)
        a, b = model(p, ga, vs, vb - v), model(p, gb, vs, vb + v)
        V.append(v); IA.append(a[0] + b[0]); IS.append(a[1] + b[1])
    q = model(p, bias, vs, vb)
    return 4 * np.mean(np.square(V)) / raa, 2 * q[0], np.mean(IA), 2 * q[1], np.mean(IS)


def pentode_section():
    VG, VS, VP, IP, IG2, SET = pent_data()
    scr = np.isfinite(IG2) & (VG <= 0) & (VG >= -10)      # screen curves at very negative grids show secondary-emission bumps
    def res(p):
        ip, i2 = pent_new(p, VG, VS, VP)
        return np.concatenate([((ip - IP) / (IP + 5))[np.isfinite(IP)], ((i2 - IG2) / (IG2 + 2))[scr]])
    fit = least_squares(res, [10.9, 1.43, 1700, 29, 0.87, 5e-4, 0.06, 0.4],
                        bounds=([4, 1.0, 300, 5, 0.05, 0, 0.0, 0.0], [20, 2.0, 5000, 300, 20, 2e-3, 0.3, 1.0])).x
    print("\n== 6L6GC against digitised Tung-Sol curves (per tube)")
    print("   recommended: " + "  ".join("%s %.5g" % (k, v) for k, v in zip(NEW_NAMES, fit)) +
          "  (kv in mA/V^1.5 = %.4g A/V^1.5; knee exponent %g fixed)" % (fit[4] * 1e-3, KNEE_N))
    for name, model, p in (("old (Koren 1996 + rs .07, asc .22)", pent_old, OLD_6L6), ("new", pent_new, fit)):
        ip, i2 = model(p, VG, VS, VP)
        def r(m, a, b): return "%.1f / %.1f" % (rms(a[m] - b[m]), np.abs(a[m] - b[m]).max())
        print("   %-36s plate Ec2=400: %s mA (rms / max) | knee, Ec1 >= -8, Vp < 100: %s | transfer Vp=400: %s |"
              " transfer below 60 mA: %s | plate Ec1=0: %s | screen Ec2=400: %s | screen Ec1=0: %s" % (
                  name, r(SET == 0, ip, IP), r((SET == 0) & (VP < 100) & (VG >= -8), ip, IP), r(SET == 1, ip, IP),
                  r((SET == 1) & (IP < 60), ip, IP), r(SET == 2, ip, IP), r((SET == 3) & scr, i2, IG2), r((SET == 4) & scr, i2, IG2)))
    print("   data-sheet typical operation (GE ET-T515A p.2), two tubes: output W, plate mA zero/max signal, screen mA zero/max")
    for label, args, ref in (("450/400/-37 V, 5600 ohm", (450, 400, -37, 35, 5600), "55 W, 116/210, 5.6/22"),
                             ("360/270/-22.5 V, 6600 ohm", (360, 270, -22.5, 22.5, 6600), "26.5 W, 88/132, 5/15"),
                             ("360/270/-22.5 V, 3800 ohm", (360, 270, -22.5, 22.5, 3800), "18 W, 88/140, 5/11")):
        for name, model, p in (("old", pent_old, OLD_6L6), ("new", pent_new, fit)):
            print("   %-26s %-4s %.1f W, %.0f/%.0f, %.1f/%.1f     (data sheet: %s)" % ((label, name) + push_pull(model, p, *args) + (ref,)))
    print("   class A points (plate / screen mA): 250/250/-14: old %.1f / %.1f, new %.1f / %.1f (72 / 5.0);"
          "  350/250/-18: old %.1f / %.1f, new %.1f / %.1f (54 / 2.5)" % (
              pent_old(OLD_6L6, -14., 250., 250.) + pent_new(fit, -14., 250., 250.) + pent_old(OLD_6L6, -18., 250., 350.) + pent_new(fit, -18., 250., 350.)))
    for name, model, p in (("old", pent_old, OLD_6L6), ("new", pent_new, fit)):
        vg16 = brentq(lambda v: model(p, v, 470., 475.)[0] - 16.0, -90, -30)
        pw = [(raa, 2 * push_pull(model, p, 475, 470, vg16, -vg16, 2 * raa)[0]) for raa in (2200, 2450, 3000, 3600, 4200)]
        print("   amp point 475 V plate / 470 V screen: 16 mA per tube needs %.1f V (%s); four tubes driven to 0 V, stiff supply: "
              % (vg16, name) + ", ".join("%d ohm %.0f W" % x for x in pw))
    # control-grid current, Tung-Sol plate #5819 upper graph (flat part, Vp >= 200 V)
    d = load("6l6gc_tungsol_ig1_g2_400.csv")
    v = np.array([4., 8., 12., 16., 20.]); i = np.array([d[(d[:, 0] == x) & (d[:, 1] >= 200), 2].mean() for x in v])
    f = lambda p, v, cg=8.0: 1e3 * p[0] * (sp(cg * v) / cg) ** p[1]
    g = least_squares(lambda p: (f(p, v) - i) / i, [1.3e-3, 1.0]).x
    print("   grid current at +4..+20 V: data " + " ".join("%.1f" % x for x in i) + " mA; old law (1.2e-3, 1.4): " +
          " ".join("%.1f" % x for x in f([1.2e-3, 1.4], v)) + "; fit gg %.4e xi %.4f: " % tuple(g) + " ".join("%.1f" % x for x in f(g, v)))
    return fit, g


# ----------------------------------------------------------------------------------------- speaker load
def speaker_section():
    r = np.array([[float(v) for v in l.split(",")] for l in open(os.path.join(DATA, "driver_8ohm_impedance_band.csv")) if l[0].isdigit()])
    sc = 12.9 / 7.3                                    # reference driver data sheet [N6]: Re 7.3 ohm (8 ohm) and 12.9 ohm (16 ohm)
    f, z = r[:, 0], r[:, 2] * sc
    w = 2j * np.pi * f
    zres = 1 / (1 / 85.0 + 1 / (w * 31e-3) + w * 68e-6)             # resonance branch as in CIRCUIT.md 7.4 (estimate)
    one = lambda p: 12.9 + 1 / (1 / (w * p[0]) + 1 / p[1]) + zres
    two = lambda p: 12.9 + 1 / (1 / (w * p[0]) + 1 / p[1]) + 1 / (1 / (w * p[2]) + 1 / p[3]) + zres
    db = lambda zz: 20 * np.log10(np.abs(zz) / z)
    m, hf = f >= 300, (f >= 2000) & (f <= 10000)
    a = least_squares(lambda p: db(one(p))[m], [1e-3, 70], bounds=([1e-4, 10], [1e-2, 1e3])).x
    b = least_squares(lambda p: db(two(p))[m], [0.5e-3, 15, 0.6e-3, 80], bounds=([1e-5, 1, 1e-5, 1], [1e-2, 500, 1e-2, 2e3])).x
    print("\n== Voice-coil rise of a 16 ohm load against the median of Zollner Fig. 11.9 (8 ohm family of the reference driver's maker x %.3f)" % sc)
    for name, zz in (("old: 1.6 mH || 70 ohm", one([1.6e-3, 70.0])), ("one section refit: %.3g mH || %.3g ohm" % (a[0] * 1e3, a[1]), one(a)),
                     ("two sections: %.4g mH || %.4g ohm + %.4g mH || %.4g ohm" % (b[0] * 1e3, b[1], b[2] * 1e3, b[3]), two(b))):
        e = db(zz)
        print("   %-58s 0.3-18 kHz: %.2f dB rms, %.2f max | 2-10 kHz: %.2f rms, %.2f max | |Z| 1k %.1f 3k %.1f 5k %.1f 8k %.1f"
              % (name, rms(e[m]), abs(e[m]).max(), rms(e[hf]), abs(e[hf]).max(), *[abs(zz[np.argmin(abs(f - x))]) for x in (1e3, 3e3, 5e3, 8e3)]))
    print("   target |Z|: " + " ".join("%g:%.1f" % (x, y) for x, y in zip(f, z) if x in (1000, 3000, 5000, 8000, 10000)))


if __name__ == "__main__":
    np.set_printoptions(precision=4, suppress=True)
    triode_section()
    pentode_section()
    speaker_section()
