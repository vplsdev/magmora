"""The overdrive pedal ("Cinder", working name): netlist, device data, K-method system and SPICE deck.

Every value, its place in the circuit, its sources and its confidence mark are in docs/research/PEDAL.md; the
designators below are our own (the readings disagree on numbering). Cluster "O".

The head's Circuit knows R, C, L, V, I, diodes, valves and an ideal transformer. The pedal also needs
voltage-controlled current sources (the op-amp macro-model and the emitter followers). They are kept in a list
beside the Circuit and stamped into the matrices that dk.build returns, so dk.py, the Newton solver, the runner
and the SPICE exporter are used unchanged and the same description still feeds both simulators.

    python -m blockamp.pedal --fit      refit the diode and transistor laws from docs/research/data and compare
"""
from __future__ import annotations
import math, os
import numpy as np
from .circuit import Circuit, GND
from . import dk, spice

VT25 = 0.025693          # kT/q at 25 C, the temperature of the data-sheet curves

DEFAULTS = dict(drive=5.0, tone=5.0, level=5.0)      # panel positions 0..10 (ChainParams.h ids 23..25)

# Device laws. i = isat*(exp(v/nvt) - 1), continued as a straight line above vcrit (devices.diode / spice._diode_lines).
# Fitted by fit_devices() below to curves digitised by us (docs/research/data); PEDAL.md section 4.
D_CLIP = dict(isat=4.3116e-9, nvt=1.9442 * VT25, vcrit=0.90)    # 1N914 / 1N4148, onsemi 1N914-D figs 3-4, 1 uA..3 mA
Q_BE = dict(isat=1.5479e-13, nvt=1.1318 * VT25, vcrit=0.90)      # KSC1815 (2SC1815) IC(VBE), Fairchild fig 2, 0.1..3 mA
D_CLAMP = dict(isat=1e-12, nvt=VT25, vcrit=0.75)                 # ESTIMATE: op-amp output saturation, macro-model only
CLAMP_DROP = 0.55        # volts across D_CLAMP at the roughly 1 nA .. 30 uA it carries: the clamp sources are set back by this

PHYS = dict(
    # supply and bias (PEDAL.md 3.1)
    vcc=9.0, vbias=4.5,
    # input buffer
    c_in=22e-9, r_in=1e3, r_b1=510e3, r_e1=10e3,
    # into the clipping amplifier
    c_c1=1e-6, r_p1=10e3,
    # clipping amplifier
    r_g=4.7e3, c_g=47e-9, r_f=51e3, p_drive=500e3, c_f=51e-12,
    c_d=0.87e-12,            # capacitance of each clipping diode at 0 V: onsemi 1N914-D fig 7 (typical; the table's maximum is 4 pF)
    # tone and level
    r_lp=1e3, c_lp=0.22e-6, r_p2=10e3, p_tone=20e3, r_t=220.0, c_t=0.22e-6, r_fb2=1e3,
    c_c2=1e-6, r_lv=1e3, p_level=100e3, r_sw=510e3,
    # output buffer and output network (1979 version: 100 ohm / 10 k; 1981: 470 ohm / 100 k)
    c_c3=1e-6, r_b2=510e3, r_e2=10e3, r_os=100.0, c_out=10e-6, r_osh=10e3,
    r_load=1e6,              # what the pedal drives: the head's grid leak
    # tapers (knob laws; they do not change what the circuit can do, only where on the knob it happens)
    drive_taper="audio15", drive_a_mid=0.15,   # sources disagree; the pedal reference decides: audio, 15 % at half rotation (PEDAL.md 3.3, 6)
    tone_taper="w",          # ESTIMATE of the special taper
    level_taper="audio", taper_a_mid=0.10,
    pot_rmin=1.0,
    # op-amp: NJM4558 data sheet (PEDAL.md 4.3)
    oa_a0=1e5, oa_gbw=3e6, oa_sr=1e6,       # 100 dB, 3 MHz, 1 V/us (typical)
    oa_cc=30e-12,            # ESTIMATE: compensation capacitor; only the ratios gm/cc and imax/cc are data
    oa_ro=100.0,             # ESTIMATE: open-loop output resistance (the TI sheet of the same type plots 25 to 200 ohm)
    oa_swing=2.5,            # volts either side of bias at a 9 V supply: NJM sheet, swing = supply - 2 V, extrapolated below +-6 V
    # followers
    q_beta=200.0,            # KSC1815 fig 3 (flat at 200); classes span 70 to 700
    buf_r=1.0,               # ohm, numerical: output resistance of the unity buffer inside the follower model
)


def taper_log(x, mid):
    x = min(max(x, 0.0), 1.0)
    b = (1.0 / mid - 1.0) ** 2
    return (b ** x - 1.0) / (b - 1.0)


def taper(kind, x, ph):
    """Resistance fraction between the counter-clockwise end and the wiper at rotation x (0..1)."""
    x = min(max(x, 0.0), 1.0)
    if kind == "linear":
        return x
    if kind == "audio":
        return taper_log(x, ph["taper_a_mid"])
    if kind == "audio15":
        return taper_log(x, ph["drive_a_mid"])
    if kind == "w":          # symmetric: audio law mirrored about the centre (slow at both ends, fast through the middle)
        return 0.5 * taper_log(2 * x, ph["taper_a_mid"]) if x <= 0.5 else 1.0 - 0.5 * taper_log(2 * (1 - x), ph["taper_a_mid"])
    raise ValueError(kind)


class PedalCircuit(Circuit):
    """A Circuit plus voltage-controlled current sources and, for SPICE-only variants, raw deck lines."""

    def __init__(self, title="pedal"):
        super().__init__(title)
        self.vccs = []       # (name, a, b, cp, cn, gm): gm*(v(cp)-v(cn)) flows from a through the source to b
        self.raw = []        # SPICE lines with no K-method counterpart (the full device models of the X3 study)

    def G(self, name, a, b, cp, cn, gm):
        self.vccs.append((name, a, b, cp, cn, float(gm)))
        return self


def _opamp(c, name, p, n, out, ph, model):
    """Two-stage macro-model. Stage 1: transconductance gm1 into r1 // cc at the internal node (dc gain a0, pole
    gbw/a0), with diode clamps for output saturation. Stage 2: unity buffer with output resistance ro.
    model "onepole": linear gm1. model "tanh" (SPICE only): the input pair's tanh law, which is what limits the
    slew rate: imax = sr*cc, and imax/gm1 = sr/(2 pi gbw) = 53 mV for the data-sheet numbers, the 2*VT of a bipolar pair."""
    cc = ph["oa_cc"]; gm1 = 2 * math.pi * ph["oa_gbw"] * cc; r1 = ph["oa_a0"] / gm1
    x = name + "_x"
    if model == "onepole":
        c.G(name + "_g1", GND, x, p, n, gm1)
    elif model == "tanh":
        imax = ph["oa_sr"] * cc
        c.raw.append(f"B{name}_g1 0 {x} I = {imax:.9g}*tanh({gm1:.9g}*(v({p})-v({n}))/{imax:.9g})")
    elif model == "pair":
        # The same tanh law built from elements the K-method has: a differential pair. Each input is copied by a
        # unity buffer (it draws nothing from the circuit) onto a junction; the two junctions share a tail current
        # imax, so their currents differ by imax*tanh(vd / (2 nvt)); that difference drives the internal node.
        # 2*nvt = imax/gm1 makes the small-signal transconductance gm1 and the slew rate sr, as in "tanh".
        imax = ph["oa_sr"] * cc; rb = ph["buf_r"]
        dj = dict(isat=1e-16, nvt=imax / gm1 / 2.0, vcrit=0.95)
        tail = name + "_tl"
        for side, node, sign in (("p", p, 1.0), ("n", n, -1.0)):
            bx = f"{name}_b{side}"
            c.G(f"{name}_gb{side}", GND, bx, node, GND, 1.0 / rb); c.R(f"{name}_rb{side}", bx, GND, rb)
            c.diode(f"{name}_j{side}", bx, tail, dj)
            c.G(f"{name}_go{side}", GND, x, node, bx, sign / rb)      # junction current = (v(node) - v(bx)) / rb
        c.I(name + "_it", tail, GND, imax)
        c.R(name + "_rt", tail, GND, 1e9)
    else:
        raise ValueError(model)
    c.R(name + "_r1", x, GND, r1); c.C(name + "_cc", x, GND, cc)
    c.diode(name + "_dh", x, "vhi", D_CLAMP); c.diode(name + "_dl", "vlo", x, D_CLAMP)
    c.G(name + "_g2", GND, out, x, GND, 1.0 / ph["oa_ro"]); c.R(name + "_ro", out, GND, ph["oa_ro"])


def _follower(c, name, b, e, ph, model):
    """Emitter follower, collector on the supply. "diode": forward-active transistor as a unity buffer of the base
    voltage feeding the base-emitter junction law, with the base drawing 1/(beta+1) of the emitter current; exact
    for a transistor that never saturates (the base cannot reach the 9 V rail here). "linear": the same with the
    junction replaced by its small-signal resistance at the idle current. "bjt" (SPICE only): ngspice's own
    Gummel-Poon transistor with the same saturation current, emission coefficient and beta."""
    rb = ph["buf_r"]; beta = ph["q_beta"]
    if model == "bjt":
        c.raw.append(f"Q{name} vcc {b} {e} qfol")
        return
    bx = name + "_bx"
    c.G(name + "_gb", GND, bx, b, GND, 1.0 / rb); c.R(name + "_rb", bx, GND, rb)
    c.G(name + "_gi", b, GND, b, bx, 1.0 / (rb * (beta + 1.0)))
    if model == "diode":
        c.diode(name + "_be", bx, e, Q_BE)
    elif model == "linear":
        ie = (ph["vbias"] - 0.6) / (ph["r_e1"] + ph["r_b1"] / (beta + 1.0))      # idle emitter current, about 0.31 mA
        c.R(name + "_re", bx, e, Q_BE["nvt"] / ie)
    else:
        raise ValueError(model)


def build(ctl=None, phys=None, followers="diode", opamp="pair", clip=True):
    """The whole pedal from the input jack to the load. Input source "in" (volts at the jack); outputs "out" (the
    pedal's output jack, loaded with r_load) and the internal probes. clip=False removes the clipping diodes."""
    ctl = {**DEFAULTS, **(ctl or {})}; ph = {**PHYS, **(phys or {})}
    x = lambda k: min(max(ctl[k] / 10.0, 0.0), 1.0)
    rmin = ph["pot_rmin"]
    c = PedalCircuit("cinder pedal")
    c.V("in", "in", GND, 0.0, is_input=True)
    c.V("vb", "vb", GND, ph["vbias"])
    c.V("vhi", "vhi", GND, ph["vbias"] + ph["oa_swing"] - CLAMP_DROP)
    c.V("vlo", "vlo", GND, ph["vbias"] - ph["oa_swing"] + CLAMP_DROP)
    if followers == "bjt":
        c.V("vcc", "vcc", GND, ph["vcc"])
        c.raw.append(f".model qfol npn(is={Q_BE['isat']:.6g} nf={Q_BE['nvt'] / VT25:.6g} bf={ph['q_beta']:.6g} vaf=100 cjc=4p cje=10p tf=1.6n tnom=25)")   # capacitances: Fairchild figs 5, 6; vaf, cje ESTIMATE
    # input buffer
    c.C("Cin", "in", "a", ph["c_in"]); c.R("Rin", "a", "b1", ph["r_in"]); c.R("Rb1", "b1", "vb", ph["r_b1"])
    _follower(c, "Q1", "b1", "e1", ph, followers)
    c.R("Re1", "e1", GND, ph["r_e1"])
    # clipping amplifier
    c.C("Cc1", "e1", "p1", ph["c_c1"]); c.R("Rp1", "p1", "vb", ph["r_p1"])
    _opamp(c, "UA", "p1", "n1", "o1", ph, opamp)
    c.C("Cg", "n1", "m1", ph["c_g"]); c.R("Rg", "m1", GND, ph["r_g"])
    c.R("Rf", "n1", "f1", ph["r_f"])
    c.R("Pdrive", "f1", "o1", max(ph["p_drive"] * taper(ph["drive_taper"], x("drive"), ph), rmin))
    c.C("Cf", "n1", "o1", ph["c_f"])
    if clip:
        c.diode("D1", "n1", "o1", D_CLIP); c.diode("D2", "o1", "n1", D_CLIP)
        c.C("Cd", "n1", "o1", 2.0 * ph["c_d"])
    # tone stage. Tone at 10 puts the wiper at the inverting input (treble boost), at 0 at the non-inverting input.
    c.R("Rlp", "o1", "t", ph["r_lp"]); c.C("Clp", "t", GND, ph["c_lp"]); c.R("Rp2", "t", "vb", ph["r_p2"])
    _opamp(c, "UB", "t", "n2", "o2", ph, opamp)
    ft = taper(ph["tone_taper"], x("tone"), ph)
    c.R("Ptone_a", "t", "w", max(ph["p_tone"] * ft, rmin)); c.R("Ptone_b", "w", "n2", max(ph["p_tone"] * (1 - ft), rmin))
    c.C("Ct", "w", "w2", ph["c_t"]); c.R("Rt", "w2", GND, ph["r_t"])
    c.R("Rfb2", "o2", "n2", ph["r_fb2"])
    # level
    c.C("Cc2", "o2", "x", ph["c_c2"]); c.R("Rlv", "x", "lvt", ph["r_lv"])
    fl = taper(ph["level_taper"], x("level"), ph)
    c.R("Plevel_a", "lvt", "lw", max(ph["p_level"] * (1 - fl), rmin)); c.R("Plevel_b", "lw", "vb", max(ph["p_level"] * fl, rmin))
    c.R("Rsw", "lw", "vb", ph["r_sw"])
    # output buffer
    c.C("Cc3", "lw", "b2", ph["c_c3"]); c.R("Rb2", "b2", "vb", ph["r_b2"])
    _follower(c, "Q2", "b2", "e2", ph, followers)
    c.R("Re2", "e2", GND, ph["r_e2"])
    c.R("Ros", "e2", "oc", ph["r_os"]); c.C("Cout", "oc", "out", ph["c_out"]); c.R("Rosh", "out", GND, ph["r_osh"])
    c.R("Rload", "out", GND, ph["r_load"])
    for lab, node in (("out", "out"), ("o1", "o1"), ("e1", "e1"), ("o2", "o2"), ("e2", "e2")):
        c.out(lab, node)
    return c


# ---------------------------------------------------------------------------------------------------------------
def system(c: PedalCircuit, fs: float) -> dk.System:
    """dk.build plus the controlled sources: stamp them into G and Gdc and recompute the K-method matrices."""
    if c.raw:
        raise ValueError("this variant has SPICE-only device models; build with followers='diode'|'linear', opamp='pair'|'onepole'")
    S = dk.build(c, fs, extra_caps=False)
    ix = lambda n: -1 if n == GND else S.idx[n]
    for _, a, b, cp, cn, gm in c.vccs:
        for M in (S.G, S.Gdc):
            for node, sg in ((ix(a), 1.0), (ix(b), -1.0)):
                if node < 0: continue
                if ix(cp) >= 0: M[node, ix(cp)] += sg * gm
                if ix(cn) >= 0: M[node, ix(cn)] -= sg * gm
    A = dk.refined_inverse(S.G); gs = S.gs; ns = len(gs)
    S.A = A
    S.K = S.Nv @ A @ S.Bi; S.Pv = S.Nv @ A @ S.Bv; S.Ps = S.Nv @ A @ S.Bs
    Z = S.Ns @ A
    S.Sv = 2.0 * gs[:, None] * (Z @ S.Bv); S.Ss = 2.0 * gs[:, None] * (Z @ S.Bs) - np.eye(ns); S.Si = 2.0 * gs[:, None] * (Z @ S.Bi)
    S.Yv = S.No @ A @ S.Bv; S.Ys = S.No @ A @ S.Bs; S.Yi = S.No @ A @ S.Bi
    S.Xv = A @ S.Bv; S.Xs = A @ S.Bs; S.Xi = A @ S.Bi
    return S


class Sim:
    """Build, find the operating point, run blocks; state is kept between calls (as blockamp.sim.Sim)."""

    def __init__(self, circuit, fs, tol=1e-7, maxit=50):
        self.c = circuit; self.fs = fs; self.tol = tol; self.maxit = maxit
        self.S = system(circuit, fs)
        self.x0, self.v = dk.dc_op(self.S)
        self.s = dk.init_state(self.S, self.x0)
        self.iters = 0; self.fails = 0; self.samples = 0

    def node_dc(self, name):
        return self.x0[self.S.idx[name]]

    def run(self, u):
        S = self.S
        u = np.asarray(u, dtype=np.float64)
        if u.ndim == 1: u = u[:, None]
        y, it, fl = dk.run(u, S.vdc.copy(), S.in_idx, self.s, self.v, S.Pv, S.Ps, S.K, S.Sv, S.Ss, S.Si,
                           S.Yv, S.Ys, S.Yi, S.dev_type, S.dev_port, S.dev_par, self.tol, self.maxit)
        self.iters += it; self.fails += fl; self.samples += len(u)
        return y


def spice_deck(c: PedalCircuit, input_exprs=None, header=()):
    """The ngspice deck of the same description: the head's exporter for the common elements, then the controlled
    sources and any SPICE-only device lines."""
    L = [spice.export(c, input_exprs, header).rstrip("\n")]
    for name, a, b, cp, cn, gm in c.vccs:
        L.append(f"G{name} {a} {b} {cp} {cn} {gm:.12g}")
    L += c.raw
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------------------------------------------
def closed_form(f, ctl=None, phys=None, probe="out"):
    """Small-signal response from the input jack, written from the stage equations by hand: it shares the component
    values with build() but not the netlist, the matrix builder or the solver. Ideal followers except for their
    emitter resistance re and base loading; op-amps as a0/(1 + s a0/(2 pi gbw)) with no output resistance; the
    clipping diodes as their zero-bias conductance. probe: "o1" (clipping amplifier output) or "out"."""
    ctl = {**DEFAULTS, **(ctl or {})}; ph = {**PHYS2, **(phys or {})}      # the product model's values
    x = lambda k: min(max(ctl[k] / 10.0, 0.0), 1.0)
    s = 2j * np.pi * np.asarray(f, dtype=float)
    par = lambda *z: 1.0 / sum(1.0 / q for q in z)
    zc = lambda cap: 1.0 / (s * cap)
    beta = ph["q_beta"]; rmin = ph["pot_rmin"]
    ie = (ph["vbias"] - 0.6) / (ph["r_e1"] + ph["r_b1"] / (beta + 1.0)); re = Q_BE["nvt"] / ie
    A = ph["oa_a0"] / (1.0 + s * ph["oa_a0"] / (2 * np.pi * ph["oa_gbw"]))
    # input buffer: emitter load, base input impedance, input high-pass, follower gain
    zl1 = par(ph["r_e1"], zc(ph["c_c1"]) + ph["r_p1"])
    zb1 = par(ph["r_b1"], (beta + 1.0) * (re + zl1))
    h = zb1 / (zc(ph["c_in"]) + ph["r_in"] + zb1) * zl1 / (re + zl1)
    h = h * ph["r_p1"] / (ph["r_p1"] + zc(ph["c_c1"]))
    # clipping amplifier, non-inverting, finite gain
    gd0 = 2.0 * D_CLIP["isat"] / D_CLIP["nvt"]
    rdrv = max(ph["p_drive"] * taper(ph["drive_taper"], x("drive"), ph), rmin)
    zf = 1.0 / (1.0 / (ph["r_f"] + rdrv) + s * (ph["c_f"] + 2.0 * ph["c_d"]) + gd0)
    zg = ph["r_g"] + zc(ph["c_g"])
    b = zg / (zg + zf)
    h = h * A / (1.0 + A * b)
    if probe == "o1":
        return h
    # tone stage: nodes t (non-inverting input), w (wiper), n (inverting input), o (output); o = A (t - n)
    ft = taper(ph["tone_taper"], x("tone"), ph)
    ra = max(ph["p_tone"] * ft, rmin); rb = max(ph["p_tone"] * (1 - ft), rmin)
    ys = 1.0 / (ph["r_t"] + zc(ph["c_t"]))
    out = np.empty(len(s), dtype=complex)
    for k in range(len(s)):
        M = np.array([
            [1 / ph["r_lp"] + s[k] * ph["c_lp"] + 1 / ph["r_p2"] + 1 / ra, -1 / ra, 0, 0],
            [-1 / ra, 1 / ra + 1 / rb + ys[k], -1 / rb, 0],
            [0, -1 / rb, 1 / rb + 1 / ph["r_fb2"], -1 / ph["r_fb2"]],
            [-A[k], 0, A[k], 1.0]], dtype=complex)
        out[k] = np.linalg.solve(M, np.array([1 / ph["r_lp"], 0, 0, 0], dtype=complex))[3]
    h = h * out
    # level control and output buffer
    fl = taper(ph["level_taper"], x("level"), ph)
    rup = max(ph["p_level"] * (1 - fl), rmin); rlo = max(ph["p_level"] * fl, rmin)
    zo = ph["r_os"] + zc(ph["c_out"]) + par(ph["r_osh"], ph["r_load"])
    zl2 = par(ph["r_e2"], zo)
    zb2 = par(ph["r_b2"], (beta + 1.0) * (re + zl2))
    zw = par(rlo, ph["r_sw"], zc(ph["c_c3"]) + zb2)
    h = h * zw / (zc(ph["c_c2"]) + ph["r_lv"] + rup + zw) * zb2 / (zc(ph["c_c3"]) + zb2)
    h = h * zl2 / (re + zl2) * par(ph["r_osh"], ph["r_load"]) / zo
    return h


# ---------------------------------------------------------------------------------------------------------------
def fit_devices(data_dir=None):
    """Refit the junction laws from the digitised data-sheet curves. Returns {name: (isat, n, rms mV, max mV)}."""
    from scipy.optimize import least_squares
    d = data_dir or os.path.join(os.path.dirname(__file__), "..", "..", "docs", "research", "data")
    res = {}
    def load(fn):
        rows = [ln.split(",") for ln in open(os.path.join(d, fn)) if ln.strip() and not ln.startswith("#")]
        return rows[0], np.array([[float(q) for q in r[:2]] for r in rows[1:]])
    hdr, a = load("1n914_onsemi_forward.csv"); assert hdr[:2] == ["i_A", "v_V"]
    m = a[:, 0] <= 3e-3
    sets = {"D_CLIP": (a[m, 0], a[m, 1], True)}
    hdr, b = load("ksc1815_fairchild_transfer.csv"); assert [h.strip() for h in hdr] == ["vbe_V", "ic_A"]
    m = b[:, 1] <= 3e-3
    sets["Q_BE"] = (b[m, 1], b[m, 0], False)
    for name, (i, v, minus1) in sets.items():
        model = lambda p: p[1] * VT25 * np.log(i / math.exp(p[0]) + (1.0 if minus1 else 0.0))
        r = least_squares(lambda p: model(p) - v, [math.log(1e-9), 1.5])
        e = (model(r.x) - v) * 1e3
        res[name] = (math.exp(r.x[0]), r.x[1], float(np.sqrt(np.mean(e ** 2))), float(np.abs(e).max()))
    return res


if __name__ == "__main__":
    import sys
    if "--fit" in sys.argv:
        for name, (isat, n, rms, mx) in fit_devices().items():
            cur = globals()[name]
            print(f"{name}: isat {isat:.4e} A, n {n:.4f} (rms {rms:.2f} mV, max {mx:.2f} mV over the fitted range); "
                  f"in use: isat {cur['isat']:.4e}, n {cur['nvt'] / VT25:.4f}")


# ===============================================================================================================
# The product model: scalar-port devices, one cluster or four (PEDAL.md 5 and 8, round 2)
#
# Above, every nonlinear element is assembled from junction elements so that the head's solver runs it unchanged;
# that 12-port construction stays as the lineage that was first verified against ngspice. Below is the same circuit
# with each nonlinear element as ONE scalar function of ONE control voltage, which is what the engine implements:
#   "dpair"  the two clipping diodes (they may differ): i = is1 (exp(u/nvt1) - 1) - is2 (exp(-u/nvt2) - 1)
#   "tanh"   an op-amp's input pair:                     i = imax tanh((u + vos) / vs)
#   "clamp"  an op-amp's output saturation:              i = is (exp((u - vhi)/vt) - exp((vlo - u)/vt))
#   "bjt"    an emitter follower's transistor:           ic = is exp(u/nvt), u = vbe + (nvt/vaf) vce
# A device has a control row (node, coefficient) and an injection column, which need not be the same nodes, so no
# helper buffers are needed. Seven ports in all. model.split() cuts the circuit into four clusters that are solved
# one after the other within the same sample, with no delay anywhere.
from numba import njit

K_DPAIR, K_TANH, K_CLAMP, K_BJT = 0, 1, 2, 3
EXP_MAX = 40.0

# The reference unit: component values of one physical pedal, fitted inside each part's tolerance to
# the pedal reference's training captures (docs/research/PEDAL.md 7.6). Nominal
# values stay in PHYS above. Drive pot: the reference's eleven positions follow the 15 % audio law with this total.
UNIT = dict(r_g=4655.0, c_g=43.42e-9, r_f=50.32e3, c_f=49.96e-12, r_lp=1007.0, c_lp=0.2536e-6, p_tone=19.76e3, c_t=0.2063e-6,
            oa_gbw=2.835e6, p_drive=481e3)

PHYS2 = dict(
    PHYS, **UNIT,
    # unit-to-unit quantities; defaults are the data-sheet typical values, section 4 and 7 of PEDAL.md give ranges
    d_is_scale=1.0,          # both clipping diodes: saturation current relative to the onsemi typical curve
    d_n_scale=1.0,           # both clipping diodes: emission coefficient relative to the onsemi typical curve (1.944)
    d_mismatch_mv=8.0,       # forward-voltage difference between the two clipping diodes at equal current (mV): fitted to
                             # the pedal reference's second harmonic on its training captures (PEDAL.md 7.3); 0 = matched
    oa_vos_a=0.0, oa_vos_b=0.0,   # input offset voltages (NJM4558: 0.5 mV typical, 6 mV maximum)
    oa_ib=25e-9,             # input bias current, into each input (NJM4558: 25 nA typical, 500 nA maximum)
    q_vaf=65.0,              # Early voltage: slope of the Fairchild sheet's output characteristics (50 to 80 V read)
    prewarp_hz=20000.0,      # frequency at which the trapezoidal rule's warping is cancelled for PREWARP_CAPS (0 = off); see system2()
)


# Capacitors whose discrete value is prewarped: the two that shape the treble after the clipper. Not the op-amps'
# compensation capacitors (they set the slew rate: scaling them by 1 % moved harmonics 57 dB down by 1 dB at 3 V in)
# and not the feedback capacitor.
PREWARP_CAPS = ("Clp", "Ct")


class Net(PedalCircuit):
    def __init__(self, title="pedal"):
        super().__init__(title)
        self.devs = []       # (name, kind, ctrl [(node, coeff)], inj [(node, coeff)], params[6])

    def dev(self, name, kind, ctrl, inj, par):
        self.devs.append((name, kind, list(ctrl), list(inj), list(par) + [0.0] * (6 - len(par))))
        return self


def _opamp2(c, name, p, n, out, ph, vos, linear=False):
    cc = ph["oa_cc"]; gm1 = 2 * math.pi * ph["oa_gbw"] * cc; r1 = ph["oa_a0"] / gm1; x = name + "_x"
    if linear:
        c.G(name + "_g1", GND, x, p, n, gm1)
    else:
        imax = ph["oa_sr"] * cc
        c.dev(name + "_in", K_TANH, [(p, 1.0), (n, -1.0)], [(x, 1.0)], [imax, imax / gm1, vos])
        c.dev(name + "_sat", K_CLAMP, [(x, 1.0)], [(x, -1.0)], [D_CLAMP["isat"], D_CLAMP["nvt"],
              ph["vbias"] + ph["oa_swing"] - CLAMP_DROP, ph["vbias"] - ph["oa_swing"] + CLAMP_DROP])
    c.R(name + "_r1", x, GND, r1); c.C(name + "_cc", x, GND, cc)
    c.G(name + "_g2", GND, out, x, GND, 1.0 / ph["oa_ro"]); c.R(name + "_ro", out, GND, ph["oa_ro"])


def _bjt(c, name, b, e, ph):
    if ph.get("spice_q"):          # SPICE-only cross-check: ngspice's own Gummel-Poon transistor (system2 refuses it)
        c.raw.append(f"Q{name} vcc {b} {e} qfol")
        if not any(l.startswith(".model qfol") for l in c.raw):
            c.raw.append(f".model qfol npn(is={Q_BE['isat']:.6g} nf={Q_BE['nvt'] / VT25:.6g} bf={ph['q_beta']:.6g} vaf={ph['q_vaf']:.6g} cjc=4p cje=10p tf=1.6n tnom=25)")
        return
    # Forward-active transistor as in the Gummel-Poon model without high-injection terms: the base current is the
    # junction law over beta and does not see the collector voltage; the collector current is the junction law times
    # (1 + vcb/vaf), written as exp(vcb/vaf) inside the exponent (the two differ by under 1 % at 9 V and 65 V).
    beta = ph["q_beta"]
    if ph["q_vaf"] > 0:
        k = Q_BE["nvt"] / ph["q_vaf"]
        c.dev(name + "_c", K_BJT, [(b, 1.0 - k), (e, -1.0), ("vcc", k)], [(e, 1.0), ("vcc", -1.0)], [Q_BE["isat"], Q_BE["nvt"]])
        c.dev(name + "_b", K_BJT, [(b, 1.0), (e, -1.0)], [(e, 1.0), (b, -1.0)], [Q_BE["isat"] / beta, Q_BE["nvt"]])
    else:
        c.dev(name, K_BJT, [(b, 1.0), (e, -1.0)], [(e, 1.0 + 1.0 / beta), (b, -1.0 / beta), ("vcc", -1.0)], [Q_BE["isat"], Q_BE["nvt"]])


def _clip_params(ph):
    """Saturation currents of the two diodes: common scale, and a forward-voltage mismatch split evenly."""
    nvt = D_CLIP["nvt"] * ph["d_n_scale"]; r = math.exp(0.5e-3 * ph["d_mismatch_mv"] / nvt)
    return [D_CLIP["isat"] * ph["d_is_scale"] / r, nvt, D_CLIP["isat"] * ph["d_is_scale"] * r, nvt]


def _stage_in(c, ph, src=True):
    if src: c.V("in", "in", GND, 0.0, is_input=True)
    c.C("Cin", "in", "a", ph["c_in"]); c.R("Rin", "a", "b1", ph["r_in"]); c.R("Rb1", "b1", "vb", ph["r_b1"])
    _bjt(c, "Q1", "b1", "e1", ph); c.R("Re1", "e1", GND, ph["r_e1"])
    c.C("Cc1", "e1", "p1", ph["c_c1"]); c.R("Rp1", "p1", "vb", ph["r_p1"])
    c.I("Iba", "p1", GND, ph["oa_ib"])


def _tone_net(c, ph, x):
    c.R("Rlp", "o1", "t", ph["r_lp"]); c.C("Clp", "t", GND, ph["c_lp"]); c.R("Rp2", "t", "vb", ph["r_p2"])
    ft = taper(ph["tone_taper"], x("tone"), ph); rmin = ph["pot_rmin"]
    c.R("Ptone_a", "t", "w", max(ph["p_tone"] * ft, rmin)); c.R("Ptone_b", "w", "n2", max(ph["p_tone"] * (1 - ft), rmin))
    c.C("Ct", "w", "w2", ph["c_t"]); c.R("Rt", "w2", GND, ph["r_t"]); c.R("Rfb2", "o2", "n2", ph["r_fb2"])
    c.I("Ibb1", "t", GND, ph["oa_ib"]); c.I("Ibb2", "n2", GND, ph["oa_ib"])


def _stage_clip(c, ph, x):
    _opamp2(c, "UA", "p1", "n1", "o1", ph, ph["oa_vos_a"])
    c.C("Cg", "n1", "m1", ph["c_g"]); c.R("Rg", "m1", GND, ph["r_g"]); c.R("Rf", "n1", "f1", ph["r_f"])
    c.R("Pdrive", "f1", "o1", max(ph["p_drive"] * taper(ph["drive_taper"], x("drive"), ph), ph["pot_rmin"]))
    c.C("Cf", "n1", "o1", ph["c_f"] + 2.0 * ph["c_d"])
    c.dev("Dclip", K_DPAIR, [("n1", 1.0), ("o1", -1.0)], [("o1", 1.0), ("n1", -1.0)], _clip_params(ph))
    c.I("Ibn", "n1", GND, ph["oa_ib"])


def _stage_out(c, ph, x):
    fl = taper(ph["level_taper"], x("level"), ph); rmin = ph["pot_rmin"]
    c.C("Cc2", "o2", "x", ph["c_c2"]); c.R("Rlv", "x", "lvt", ph["r_lv"])
    c.R("Plevel_a", "lvt", "lw", max(ph["p_level"] * (1 - fl), rmin)); c.R("Plevel_b", "lw", "vb", max(ph["p_level"] * fl, rmin))
    c.R("Rsw", "lw", "vb", ph["r_sw"]); c.C("Cc3", "lw", "b2", ph["c_c3"]); c.R("Rb2", "b2", "vb", ph["r_b2"])
    _bjt(c, "Q2", "b2", "e2", ph); c.R("Re2", "e2", GND, ph["r_e2"])
    c.R("Ros", "e2", "oc", ph["r_os"]); c.C("Cout", "oc", "out", ph["c_out"]); c.R("Rosh", "out", GND, ph["r_osh"])
    c.R("Rload", "out", GND, ph["r_load"])


def _rails(c, ph, vcc=True):
    c.V("vb", "vb", GND, ph["vbias"])
    if vcc: c.V("vcc", "vcc", GND, ph["vcc"])


def build2(ctl=None, phys=None):
    """The whole pedal as one cluster with seven scalar ports. Outputs: out, o1, e1, o2, e2."""
    ctl = {**DEFAULTS, **(ctl or {})}; ph = {**PHYS2, **(phys or {})}
    x = lambda k: min(max(ctl[k] / 10.0, 0.0), 1.0)
    c = Net("cinder pedal"); _rails(c, ph)
    _stage_in(c, ph); _stage_clip(c, ph, x); _tone_net(c, ph, x); _opamp2(c, "UB", "t", "n2", "o2", ph, ph["oa_vos_b"]); _stage_out(c, ph, x)
    for lab in ("out", "o1", "e1", "o2", "e2"): c.out(lab, lab)
    c.ph = ph
    return c


def split(ctl=None, phys=None, linear_opamps=False):
    """The same circuit as four clusters, solved in this order within one sample:
      O1  input buffer up to the clipping amplifier's input (that input draws only its bias current: exact cut)
      O2  clipping amplifier, loaded by a linear copy of the tone stage (its op-amp without tanh and saturation)
      O3  tone stage, driven by O2's output voltage
      O4  level control and output buffer, driven by O3's output voltage; O3 carries a linear copy of this load
    Returns [(net, input source name, node of the previous cluster that drives it)]."""
    ctl = {**DEFAULTS, **(ctl or {})}; ph = {**PHYS2, **(phys or {})}
    x = lambda k: min(max(ctl[k] / 10.0, 0.0), 1.0)
    o1 = Net("O1"); _rails(o1, ph); _stage_in(o1, ph); o1.out("p1", "p1"); o1.out("e1", "e1")
    o2 = Net("O2"); _rails(o2, ph, vcc=False); o2.V("p1", "p1", GND, ph["vbias"], is_input=True)
    _stage_clip(o2, ph, x); _tone_net(o2, ph, x); _opamp2(o2, "UBl", "t", "n2", "o2", ph, 0.0, linear=True); o2.out("o1", "o1")
    o3 = Net("O3"); _rails(o3, ph, vcc=False); o3.V("o1", "o1", GND, ph["vbias"], is_input=True)
    _tone_net(o3, ph, x); _opamp2(o3, "UB", "t", "n2", "o2", ph, ph["oa_vos_b"], linear=linear_opamps); o3.out("o2", "o2")
    if linear_opamps:
        # The cheap mode for the engine (PEDAL.md 8): while an op-amp's input difference is a few millivolts and its
        # internal node is clear of the rails, its tanh is a straight line and its clamp carries nothing, so both
        # ports can be folded into the matrices. The two guard outputs let the engine check that and redo the sample
        # with the full cluster (same states) when it does not hold.
        o2.devs = [d for d in o2.devs if d[1] == K_DPAIR]
        cc = ph["oa_cc"]; o2.G("UA_g1", GND, "UA_x", "p1", "n1", 2 * math.pi * ph["oa_gbw"] * cc)
        o2.out("vd", "p1", "n1"); o2.out("x", "UA_x"); o3.out("vd", "t", "n2"); o3.out("x", "UB_x")
    # linear copy of what O4 hangs on o2 (it matters only while this op-amp is saturated and its output is 100 ohm)
    fl = taper(ph["level_taper"], x("level"), ph); rmin = ph["pot_rmin"]
    ie = (ph["vbias"] - 0.6) / (ph["r_e2"] + ph["r_b2"] / (ph["q_beta"] + 1.0))
    o3.C("Cc2", "o2", "x", ph["c_c2"]); o3.R("Rlv", "x", "lvt", ph["r_lv"])
    o3.R("Plevel_a", "lvt", "lw", max(ph["p_level"] * (1 - fl), rmin)); o3.R("Plevel_b", "lw", "vb", max(ph["p_level"] * fl, rmin))
    o3.R("Rsw", "lw", "vb", ph["r_sw"]); o3.C("Cc3", "lw", "b2", ph["c_c3"])
    o3.R("Rb2", "b2", "vb", 1.0 / (1.0 / ph["r_b2"] + 1.0 / ((ph["q_beta"] + 1.0) * (Q_BE["nvt"] / ie + ph["r_e2"] / 2.0))))
    o4 = Net("O4"); _rails(o4, ph); o4.V("o2", "o2", GND, ph["vbias"], is_input=True)
    _stage_out(o4, ph, x); o4.out("out", "out"); o4.out("e2", "e2")
    for n in (o1, o2, o3, o4): n.ph = ph
    return [(o1, "in", None), (o2, "p1", "p1"), (o3, "o1", "o1"), (o4, "o2", "o2")]


@njit(cache=True, error_model="numpy")
def dev_eval(kind, P, u):
    """Current and conductance of a scalar device at control voltage u."""
    if kind == 0:
        a = u / P[1]; b = -u / P[3]
        if a > EXP_MAX: ea = math.exp(EXP_MAX) * (1.0 + a - EXP_MAX); da = math.exp(EXP_MAX)
        else: ea = math.exp(a); da = ea
        if b > EXP_MAX: eb = math.exp(EXP_MAX) * (1.0 + b - EXP_MAX); dbb = math.exp(EXP_MAX)
        else: eb = math.exp(b); dbb = eb
        return P[0] * (ea - 1.0) - P[2] * (eb - 1.0), P[0] * da / P[1] + P[2] * dbb / P[3]
    if kind == 1:
        t = math.tanh((u + P[2]) / P[1])
        return P[0] * t, P[0] * (1.0 - t * t) / P[1]
    if kind == 2:
        a = (u - P[2]) / P[1]; b = (P[3] - u) / P[1]
        if a > EXP_MAX: ea = math.exp(EXP_MAX) * (1.0 + a - EXP_MAX); da = math.exp(EXP_MAX)
        else: ea = math.exp(a); da = ea
        if b > EXP_MAX: eb = math.exp(EXP_MAX) * (1.0 + b - EXP_MAX); dbb = math.exp(EXP_MAX)
        else: eb = math.exp(b); dbb = eb
        return P[0] * (ea - eb), P[0] * (da + dbb) / P[1]
    a = u / P[1]
    if a > EXP_MAX: return P[0] * math.exp(EXP_MAX) * (1.0 + a - EXP_MAX), P[0] * math.exp(EXP_MAX) / P[1]
    e = math.exp(a)
    return P[0] * e, P[0] * e / P[1]


@njit(cache=True, error_model="numpy")
def newton2(p, K, v, kind, par, cur, tol, maxit):   # n <= 7: the three small work arrays live on the stack in the engine
    """Solve v = p + K i(v) in place for n scalar ports. Steps into junction conduction are limited. Returns the
    number of iterations (negative if the cap was hit)."""
    n = p.shape[0]
    M = np.empty((n, n)); F = np.empty(n); g = np.empty(n)
    for it in range(maxit):
        for a in range(n):
            cur[a], g[a] = dev_eval(kind[a], par[a], v[a])
        for a in range(n):
            acc = p[a] - v[a]
            for b in range(n):
                acc += K[a, b] * cur[b]; M[a, b] = -K[a, b] * g[b]
            M[a, a] += 1.0; F[a] = acc
        for a in range(n):                           # Gaussian elimination with partial pivoting, n <= 7
            q = a; big = abs(M[a, a])
            for b in range(a + 1, n):
                if abs(M[b, a]) > big: big = abs(M[b, a]); q = b
            if q != a:
                for cc in range(n):
                    tt = M[a, cc]; M[a, cc] = M[q, cc]; M[q, cc] = tt
                tt = F[a]; F[a] = F[q]; F[q] = tt
            for b in range(a + 1, n):
                f = M[b, a] / M[a, a]
                if f != 0.0:
                    for cc in range(a, n): M[b, cc] -= f * M[a, cc]
                    F[b] -= f * F[a]
        for a in range(n - 1, -1, -1):
            acc = F[a]
            for cc in range(a + 1, n): acc -= M[a, cc] * F[cc]
            F[a] = acc / M[a, a]
        # Step limits per device (the whole step is scaled, which keeps its direction): junctions may move 0.15 / 0.10 V,
        # the saturating tanh three of its own scales, a clamp half a volt. After 10 iterations the step is halved,
        # after 25 quartered: under-relaxation breaks the two-cycles that a saturating device can fall into.
        sc = 1.0; nd = 0.0
        if it >= 25: sc = 0.25
        elif it >= 10: sc = 0.5
        for a in range(n):
            lim = 1e9
            if kind[a] == 0: lim = 0.15
            elif kind[a] == 3: lim = 0.10
            elif kind[a] == 2: lim = 0.5
            elif kind[a] == 1: lim = 3.0 * par[a, 1]
            if abs(F[a]) > lim: sc = min(sc, lim / abs(F[a]))
        for a in range(n):
            v[a] += sc * F[a]
            if abs(sc * F[a]) > nd: nd = abs(sc * F[a])
        if nd < tol:
            for a in range(n): cur[a], g[a] = dev_eval(kind[a], par[a], v[a])
            return it + 1
    return -maxit


@njit(cache=True, error_model="numpy")
def run2(u, uu, in_idx, s, v, Pv, Ps, K, Sv, Ss, Si, Yv, Ys, Yi, kind, par, tol, maxit):
    """Per sample: p = Pv u + Ps s; solve v = p + K i(v); y = Yv u + Ys s + Yi i; s <- Ss s + Sv u + Si i.
    Plain loops with no allocation, as the engine will do it. Only the input source changes from sample to sample,
    so the constant sources' share of p, y and s is folded into offsets once."""
    N = u.shape[0]; n = K.shape[0]; ny = Yv.shape[0]; ns = s.shape[0]; nu = uu.shape[0]
    y = np.empty((N, ny)); cur = np.zeros(n); vprev = v.copy(); p = np.empty(n); s2 = np.empty(ns); pw = np.empty(n)
    p0 = np.zeros(n); y0 = np.zeros(ny); s0 = np.zeros(ns)
    for k in range(nu):
        if k != in_idx:
            for a in range(n): p0[a] += Pv[a, k] * uu[k]
            for a in range(ny): y0[a] += Yv[a, k] * uu[k]
            for a in range(ns): s0[a] += Sv[a, k] * uu[k]
    iters = 0; fails = 0
    pl = np.empty(n)                                     # p of the last accepted sample
    for a in range(n):
        acc = p0[a] + Pv[a, in_idx] * uu[in_idx]
        for b in range(ns): acc += Ps[a, b] * s[b]
        pl[a] = acc
    for t in range(N):
        x = u[t]
        for a in range(n):
            acc = p0[a] + Pv[a, in_idx] * x
            for b in range(ns): acc += Ps[a, b] * s[b]
            p[a] = acc
            gss = 2.0 * v[a] - vprev[a]; vprev[a] = v[a]; v[a] = gss
        it = newton2(p, K, v, kind, par, cur, tol, maxit)
        if it < 0:
            # as in the head (PLAN 3.3 rule 6): go back to the last accepted solution and walk p there in 8 equal steps
            iters += maxit; ok = True
            for a in range(n): v[a] = vprev[a]
            for k in range(1, 9):
                for a in range(n): pw[a] = pl[a] + (p[a] - pl[a]) * k / 8.0
                it = newton2(pw, K, v, kind, par, cur, tol, maxit)
                iters += abs(it)
                if it < 0: ok = False
            if not ok: fails += 1
        else:
            iters += it
        for a in range(n): pl[a] = p[a]
        for a in range(ny):
            acc = y0[a] + Yv[a, in_idx] * x
            for b in range(ns): acc += Ys[a, b] * s[b]
            for b in range(n): acc += Yi[a, b] * cur[b]
            y[t, a] = acc
        for a in range(ns):
            acc = s0[a] + Sv[a, in_idx] * x
            for b in range(ns): acc += Ss[a, b] * s[b]
            for b in range(n): acc += Si[a, b] * cur[b]
            s2[a] = acc
        for a in range(ns): s[a] = s2[a]
    uu[in_idx] = u[N - 1] if N > 0 else uu[in_idx]
    return y, iters, fails


def system2(c: Net, fs: float, prewarp_hz=None):
    """K-method matrices of a Net. prewarp_hz: the capacitors named in PREWARP_CAPS are scaled by
    (w T/2)/tan(w T/2) at that frequency, so that the trapezoidal rule places their reactance exactly there instead
    of at 0 Hz. The scale is within 2 % of one at the rates used, far inside a capacitor's tolerance, and it is
    applied to the discrete model only (PEDAL.md 6)."""
    pw = c.ph.get("prewarp_hz", 0.0) if prewarp_hz is None else prewarp_hz
    if pw and pw > 0:
        k = (math.pi * pw / fs) / math.tan(math.pi * pw / fs)
        cc = Net(c.title); cc.__dict__.update({a: b for a, b in c.__dict__.items() if a not in ("elements",)})
        from .circuit import Element
        cc.elements = [Element(e.kind, e.name, e.nodes, e.value * k if (e.kind == "C" and e.name in PREWARP_CAPS) else e.value, e.extra) for e in c.elements]
        c = cc
    if c.raw: raise ValueError("SPICE-only device lines present")
    S = dk.build(c, fs, extra_caps=False)
    ix = lambda n: -1 if n == GND else S.idx[n]
    for _, a, b, cp, cn, gm in c.vccs:
        for M in (S.G, S.Gdc):
            for node, sg in ((ix(a), 1.0), (ix(b), -1.0)):
                if node < 0: continue
                if ix(cp) >= 0: M[node, ix(cp)] += sg * gm
                if ix(cn) >= 0: M[node, ix(cn)] -= sg * gm
    nd = len(c.devs); Nv = np.zeros((nd, S.nx)); Bi = np.zeros((S.nx, nd))
    for k, (_, kind, ctrl, inj, par) in enumerate(c.devs):
        for node, co in ctrl:
            if ix(node) >= 0: Nv[k, ix(node)] += co
        for node, co in inj:
            if ix(node) >= 0: Bi[ix(node), k] += co
    S.Nv, S.Bi = Nv, Bi
    S.kind = np.array([d[1] for d in c.devs], dtype=np.int64); S.par = np.array([d[4] for d in c.devs], dtype=np.float64).reshape(nd, 6)
    A = dk.refined_inverse(S.G); gs = S.gs; ns = len(gs); S.A = A
    S.K = Nv @ A @ Bi; S.Pv = Nv @ A @ S.Bv; S.Ps = Nv @ A @ S.Bs
    Z = S.Ns @ A
    S.Sv = 2.0 * gs[:, None] * (Z @ S.Bv); S.Ss = 2.0 * gs[:, None] * (Z @ S.Bs) - np.eye(ns); S.Si = 2.0 * gs[:, None] * (Z @ Bi)
    S.Yv = S.No @ A @ S.Bv; S.Ys = S.No @ A @ S.Bs; S.Yi = S.No @ A @ Bi
    return S


def dc2(S, steps=12):
    """Operating point by source stepping. Returns the full solution x and the port voltages."""
    Adc = dk.refined_inverse(S.Gdc); Kdc = S.Nv @ Adc @ S.Bi; Pdc = S.Nv @ Adc @ S.Bv
    n = len(S.kind); v = np.zeros(n); cur = np.zeros(n)
    for k in range(1, steps + 1):
        it = newton2(Pdc @ (S.vdc * k / steps), Kdc, v, S.kind, S.par, cur, 1e-11, 400)
        if it < 0: raise RuntimeError("dc2 did not converge")
    return Adc @ (S.Bv @ S.vdc + S.Bi @ cur), v


def response2(S, x0, v0, freqs, in_name):
    """Small-signal response of a Net's system from one input source to every output (as linear.response)."""
    n = len(S.kind); g = np.array([dev_eval(S.kind[a], S.par[a], v0[a])[1] for a in range(n)])
    J = np.diag(g); M = np.linalg.inv(np.eye(n) - S.K @ J); JM = J @ M
    ku = S.vsrc_names.index(in_name)
    A = S.Ss + S.Si @ JM @ S.Ps; B = S.Sv[:, ku] + S.Si @ JM @ S.Pv[:, ku]
    Cm = S.Ys + S.Yi @ JM @ S.Ps; D = S.Yv[:, ku] + S.Yi @ JM @ S.Pv[:, ku]
    I = np.eye(A.shape[0])
    return np.array([Cm @ np.linalg.solve(np.exp(2j * np.pi * f / S.fs) * I - A, B) + D for f in freqs])


class Sim2:
    """One Net: build, operating point, run blocks with state kept."""

    def __init__(self, net, fs, tol=1e-7, maxit=60, prewarp_hz=None):
        self.c = net; self.fs = fs; self.tol = tol; self.maxit = maxit
        self.S = system2(net, fs, prewarp_hz)
        self.x0, self.v = dc2(self.S); self.v0 = self.v.copy()
        self.s = dk.init_state(self.S, self.x0)
        self.uu = self.S.vdc.copy(); self.iters = 0; self.fails = 0; self.samples = 0

    def node_dc(self, name):
        return self.x0[self.S.idx[name]]

    def run(self, u):
        S = self.S; u = np.ascontiguousarray(u, dtype=np.float64)
        y, it, fl = run2(u, self.uu, int(S.in_idx[0]), self.s, self.v, S.Pv, S.Ps, S.K, S.Sv, S.Ss, S.Si, S.Yv, S.Ys, S.Yi, S.kind, S.par, self.tol, self.maxit)
        self.iters += it; self.fails += fl; self.samples += len(u)
        return y

    def response(self, freqs):
        return response2(self.S, self.x0, self.v0, freqs, self.c.inputs[0])


class SplitSim:
    """The four clusters in sequence. Inputs between clusters are absolute node voltages; out is the pedal's output."""

    def __init__(self, ctl=None, phys=None, fs=288000.0, tol=1e-7, prewarp_hz=None):
        self.parts = []; prev = None
        for net, src, drive in split(ctl, phys):
            if prev is not None:                         # start each cluster from the operating point of the one before
                for e in net.elements:
                    if e.kind == "V" and e.name == src: e.value = float(prev.node_dc(drive))
            prev = Sim2(net, fs, tol, prewarp_hz=prewarp_hz); self.parts.append((prev, drive))
        self.fs = fs

    def run(self, u, probes=("out",)):
        res = {}; x = np.asarray(u, dtype=np.float64)
        for sim, drive in self.parts:
            y = sim.run(x)
            for k, nm in enumerate(sim.S.out_names): res[nm] = y[:, k]
            x = y[:, 0]                                  # each cluster's first output drives the next
        return np.stack([res[p] for p in probes], 1)

    @property
    def fails(self): return sum(s.fails for s, _ in self.parts)

    @property
    def iters_per_sample(self): return [s.iters / max(s.samples, 1) for s, _ in self.parts]


def spice_deck2(c: Net, input_exprs=None):
    """ngspice deck of a Net: linear elements and controlled sources as before, each scalar device as a B-source
    with the same expression as dev_eval (without the overflow guard, which ngspice never reaches here)."""
    L = [spice.export(c, input_exprs).rstrip("\n")]
    for name, a, b, cp, cn, gm in c.vccs:
        L.append(f"G{name} {a} {b} {cp} {cn} {gm:.12g}")
    for name, kind, ctrl, inj, P in c.devs:
        u = "(" + "+".join(f"({co:.12g})*v({n})" for n, co in ctrl if n != GND) + ")"
        if kind == K_DPAIR: ex = f"{P[0]:.9g}*(exp({u}/{P[1]:.9g})-1)-{P[2]:.9g}*(exp(-{u}/{P[3]:.9g})-1)"
        elif kind == K_TANH: ex = f"{P[0]:.9g}*tanh(({u}+({P[2]:.9g}))/{P[1]:.9g})"
        elif kind == K_CLAMP: ex = f"{P[0]:.9g}*(exp(({u}-{P[2]:.9g})/{P[1]:.9g})-exp(({P[3]:.9g}-{u})/{P[1]:.9g}))"
        else: ex = f"{P[0]:.9g}*exp({u}/{P[1]:.9g})"
        for k, (n, co) in enumerate(inj):                # current co*i injected into node n
            L.append(f"B{name}_{k} 0 {n} I = ({co:.12g})*({ex})")
    L += c.raw
    return "\n".join(L) + "\n"
