"""Round 8 cabinet model: round 7's model (model7 at stage r7a2: model5's r5d plus the top-end layer)
with unit tolerance. One cabinet (Trad-style closed-back 4x12), one microphone (57-type). Nothing here reads a
reference. model4.py, model5.py, model7.py and harmonics5.py are not changed; with every deviation at zero this is
model7 exactly (run this file: `$BLOCKAMP_PY proto/cab/model8.py`).

Why: round 7 found that the four real drivers of the cabinet differ from each other in overall top-end level by about
as much as the model differs from any of them, and one shared driver cannot follow that.

The layer: each of the four cones (LL, LR, UL, UR) has its own value of four of the driver's numbers, written as a
relative deviation d from the shared value: unit value = shared value * (1 + d). The four numbers and their
tolerances (TOL8) were chosen and fixed before any fit, from what varies between production units of one driver type
and from the model's own sensitivity (no reference involved):

  E_cone    Young's modulus of the cone paper        +-15 %
  rho_cone  density of the felted pulp (cone and cap) +-5 %
  eta_cone  loss factor of the cone paper            +-20 %
  m_coil    coil, former, adhesive, half the spider  +-10 %

Rules, enforced by deviations():
  - |d| never exceeds the tolerance, and a unit's value never leaves the physical range of the shared number
    (model4.BOUNDS4);
  - the deviations of the three fitted units (LL, LR, UL) add up to zero for each number: the shared value is the mean
    of the units, so "the type" and "this unit" are separate things and a fit of the deviations cannot move the type;
  - the held-out unit (UR) has no deviation: it is the type.

Approximation: a response of speaker S is computed with all four cones of the cabinet carrying S's values. The three
cones the microphone is not on are 0.4 to 0.56 m from a microphone that is 4 to 7 cm from its own cone; their share is
bounded by check 3 below.
"""
import numpy as np
import model as M
import model4 as M4
import model5 as M5
import model7 as M7

UNITS = ("LL", "LR", "UL", "UR")
FITTED = ("LL", "LR", "UL")
HELD = "UR"
TOL8 = dict(E_cone=0.15, rho_cone=0.05, eta_cone=0.20, m_coil=0.10)
UNIT_KEYS = tuple(TOL8)


def key8(speaker, k):
    return f"u_{speaker}_{k}"


def limits(shared, k):
    """Allowed deviation (low, high) of number k around its shared value: the tolerance, cut where the unit's value
    would leave the physical range of the shared number."""
    lo, hi = M4.BOUNDS4[k]
    return max(-TOL8[k], lo / shared - 1.0), min(TOL8[k], hi / shared - 1.0)


def project(raw, lo, hi):
    """Raw deviations of the fitted units -> allowed ones: mean removed (they add up to zero), then scaled down
    together, if needed, until each is inside (lo, hi). lo <= 0 <= hi."""
    d = np.asarray(raw, float) - float(np.mean(raw))
    s = 1.0
    for x in d:
        if x > hi:
            s = min(s, hi / x)
        if x < lo:
            s = min(s, lo / x)
    return d * s


def deviations(v):
    """{speaker: {number: deviation}} from a values dict carrying u_<speaker>_<number> entries (missing = 0), with
    the rules above applied. UR is always zero."""
    out = {s: {} for s in UNITS}
    for k in UNIT_KEYS:
        lo, hi = limits(v[k], k)
        d = project([v.get(key8(s, k), 0.0) for s in FITTED], min(lo, 0.0), max(hi, 0.0))
        for s, x in zip(FITTED, d):
            out[s][k] = float(x)
        out[HELD][k] = 0.0
    return out


def unit_values(v, speaker):
    """The values of one unit: the shared ones with this unit's four numbers replaced; u_ entries removed."""
    d = deviations(v)[speaker]
    q = {k: x for k, x in v.items() if not k.startswith("u_")}
    for k in UNIT_KEYS:
        q[k] = v[k] * (1.0 + d[k])
    return q


def params(v, speaker):
    return {**M.P0, **M4.P4, **M5.P5, **M7.P7, **{k: x for k, x in unit_values(v, speaker).items() if k != "grille_gap"}}


def response(v, mic_name, x, dist, angle=0.0, speaker="LR"):
    return M7.response(params(v, speaker), mic_name, x, dist, angle, speaker)


def impulse_response(v, mic_name, x, dist, angle=0.0, speaker="LR"):
    return M7.impulse_response(params(v, speaker), mic_name, x, dist, angle, speaker)


def moving_mass(v, speaker):
    return float(M4.full(params(v, speaker))[1]["m_tot"])


if __name__ == "__main__":
    # checks without any measurement, at the defaults
    v = {**M.P0, **M4.P4, **M5.P5, **M7.P7, "grille_gap": 0.03}
    a = M7.response({k: x for k, x in v.items() if k != "grille_gap"}, "57", 0.0254, 0.04, 0.0, "LR")
    for s in UNITS:
        b = response(v, "57", 0.0254, 0.04, 0.0, s)
        ref = M7.response({k: x for k, x in v.items() if k != "grille_gap"}, "57", 0.0254, 0.04, 0.0, s)
        print(f"1. no deviations, {s}: equals model7 within {np.max(np.abs(ref - b)) / np.max(np.abs(ref)):.1e}")
    w = dict(v, **{key8("LL", "E_cone"): 0.30, key8("LR", "E_cone"): 0.10, key8("UL", "E_cone"): 0.05, key8("UR", "E_cone"): 0.5,
                   key8("LL", "rho_cone"): 0.2, key8("LR", "rho_cone"): -0.2})
    d = deviations(w)
    print("2. rules: E_cone raw +0.30 +0.10 +0.05 (and +0.5 asked for UR) -> " + " ".join(f"{s} {d[s]['E_cone']:+.3f}" for s in UNITS)
          + f"; sum of the fitted {sum(d[s]['E_cone'] for s in FITTED):+.1e}; rho_cone raw +0.2 -0.2 0 -> "
          + " ".join(f"{s} {d[s]['rho_cone']:+.3f}" for s in UNITS) + f" (limits {limits(v['rho_cone'], 'rho_cone')})")
    b = response(dict(v, **{key8("UR", "E_cone"): 0.5}), "57", 0.0254, 0.04, 0.0, "UR")
    ref = M7.response({k: x for k, x in v.items() if k != "grille_gap"}, "57", 0.0254, 0.04, 0.0, "UR")
    print(f"   the held-out unit ignores a deviation written for it: {np.max(np.abs(ref - b)) / np.max(np.abs(ref)):.1e}")
    # 3. the approximation: how much of a unit's change of response comes through the three cones the microphone is
    # not on. The same unit change (every number at its tolerance), once on all four cones (what the model does) and
    # once with the far cones and the baffle-edge sources left at the shared values; the truth lies between the two.
    import sys
    if len(sys.argv) > 1:                                   # optionally at a tuned set of shared values
        import json
        v = {**v, **{k: x for k, x in json.loads(open(sys.argv[1]).read())["values"].items() if not k.startswith("u_")}}
    u = dict(v, **{key8("LR", k): t for k, t in TOL8.items()}, **{key8("UL", k): -t for k, t in TOL8.items()})
    fb = np.geomspace(100.0, 16000.0, 23)
    def third(h):
        pw = np.abs(h) ** 2
        return np.array([10 * np.log10(pw[(M.F_FINE >= c / 2 ** (1 / 6)) & (M.F_FINE < c * 2 ** (1 / 6))].mean()) for c in fb])
    for x_in in (0.0, 2.0):
        h0 = response(v, "57", x_in * 0.0254, v["grille_gap"], 0.0, "LR")
        h1 = response(u, "57", x_in * 0.0254, v["grille_gap"], 0.0, "LR")
        hook0, qs = M7.hook, M4.full(params(v, "LR"))[0]
        def hook_far_shared(speaker):
            base = hook0(speaker)
            return lambda p, f, breakup=True, coarse=False: base(qs if coarse else p, f, breakup, coarse)
        M7.hook = hook_far_shared; M7._direct.clear()
        try:
            h2 = response(u, "57", x_in * 0.0254, v["grille_gap"], 0.0, "LR")
        finally:
            M7.hook = hook0; M7._direct.clear()
        ch, df = third(h1) - third(h0), third(h2) - third(h1)
        print(f"3. LR at {x_in:.0f} in, every number at its tolerance: the unit changes the 1/3-octave response by up to {np.abs(ch).max():.2f} dB"
              f" (rms {np.sqrt(np.mean(ch ** 2)):.2f}); far cones at shared values instead: differs by up to {np.abs(df).max():.2f} dB (rms {np.sqrt(np.mean(df ** 2)):.2f}),"
              f" above 1 kHz up to {np.abs(df[fb >= 1000]).max():.2f} dB")
    print("4. moving mass: " + "  ".join(f"{s} {1e3 * moving_mass(u, s):.2f} g" for s in UNITS))
