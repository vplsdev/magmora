"""The amplifier netlist. Reference designators follow the 1992 factory drawing so that every
element can be checked against docs/research/CIRCUIT.md. Values marked EST are not on the drawing.

build(ctl, sections=...) returns a Circuit containing the requested sections. With all sections it
is the monolithic amp (what SPICE simulates). Cluster builds for the real-time partition add
ideal sources at the cut nodes (see partition.py).
"""
from __future__ import annotations
import math
from .circuit import Circuit, GND
from .models import T12AX7 as _T12AX7, P6L6GC as _P6L6GC, D1N4007

DEFAULTS = dict(
    channel="lead",      # "lead" | "rhythm"
    crunch=False, bright=False,
    input="high",        # "high" | "normal"
    pre_rhythm=5.0, pre_lead=5.0, low=5.0, mid=5.0, high=5.0,
    post_rhythm=5.0, post_lead=5.0, resonance=5.0, presence=5.0,
)

# Component/calibration values that are NOT printed on the drawing (estimates, see CIRCUIT.md 8).
PHYS = dict(
    vb0=487.0, rsrc=160.0,          # EST: Thevenin model of PT+bridge; gives ~475 V idle
    vbias=-63.35, rbias=3.0e3,       # EST: set for the measured idle current (16 mA per tube); bias-supply resistance
    raa=2450.0,                     # OT primary impedance plate-to-plate for the nominal load: turns count of a
                                    # tear-down, 1348 : 109 on the 16 ohm tap (DEVICES.md 5); range 2.2 to 3.0 k
    rload=16.0,                     # nominal load on the matching tap
    lm=50.0,                        # OT primary inductance (H, plate to plate): same tear-down, 60 Hz / 100 V
    lleak=6e-3,                     # EST: OT leakage inductance referred to full primary (H); 3 to 10 mH
    rpri_half=33.0,                 # measured primary DCR per half (29.5 / 37.5 ohm reported)
    rsec=0.35,                      # secondary DCR on the 16 ohm tap: comparable 100 W parts, 0.23 to 0.42 ohm
    cpri=300e-12,                   # EST: primary shunt capacitance per half (nothing published)
    taper_a_mid=0.10,               # EST: "A" (audio) taper: resistance fraction at half rotation
    taper_5a_mid=0.05,              # "5%A" taper as printed for the gain and resonance pots
    # speaker / cabinet load on the 16 ohm tap. Voice coil: Re plus two semi-inductive sections fitted to the
    # median of published 12-inch impedance curves of the reference driver's maker (rise ~ f^0.5; DEVICES.md 6). Resonance branch EST.
    spk_re=12.9, spk_le=1.245e-3, spk_rle=11.11, spk_le2=0.5784e-3, spk_rle2=80.59,
    spk_rp=85.0, spk_lp=31e-3, spk_cp=68e-6,
    load="reactive",                # "reactive" | "resistive"
    t_scale=1.0, t_vg0=0.0, cgp_stray=0.7e-12, p_scale=1.0,     # tube spread, see D7
    k1_swap=0.0,                    # 0: relay K1 closed on the rhythm channel (drawing); 1: test reading only
    # Lead-channel input coupling. The 5 Feb 1992 pre-production drawing shows R29 470k // C17 2.2 nF and C2 22 nF.
    # Two captured block-letter heads need what the maker's later drawing of the same lead channel shows instead:
    # C17 470 pF alone in series (no R29) and C2 1 nF (docs/MODEL.md section 2; "drawing" values: 2.2e-9, 470e3, 22e-9).
    c17=470e-12, r29=1e9, c2=1e-9,
)


def taper_log(x, mid):
    """Audio taper through (0,0), (0.5,mid), (1,1): f = (b^x - 1)/(b - 1), b = (1/mid - 1)^2."""
    x = min(max(x, 0.0), 1.0)
    b = (1.0 / mid - 1.0) ** 2
    return (b ** x - 1.0) / (b - 1.0)


def _x(ctl, key):
    return min(max(ctl[key] / 10.0, 0.0), 1.0)


ALL = ("input", "v1a", "chan", "v1b", "ic2", "v2a", "v2b", "i5b", "v5b", "i5a", "v5a", "tone", "cf", "loop", "v3a",
       "ipi", "pi", "power", "supply")


def build(ctl=None, phys=None, sections=ALL, vin_name="in", title="amp"):
    k = dict(DEFAULTS); k.update(ctl or {})
    ph = dict(PHYS); ph.update(phys or {})
    lead = k["channel"] == "lead"
    # Relay K1 (V1B cathode bypass + V2A grid shunt): which channel closes it is an interpretation of the
    # drawing (CIRCUIT.md section 10). k1_swap = 1 gives the opposite reading, for testing against captures.
    k1_closed = (not lead) != bool(ph["k1_swap"])
    c = Circuit(title)
    # tube spread (bounded fit parameters): current scale, grid offset, stray capacitance
    T12AX7 = dict(_T12AX7); P6L6GC = dict(_P6L6GC)
    T12AX7["kg1"] = _T12AX7["kg1"] / ph["t_scale"]; T12AX7["vg0"] = ph["t_vg0"]; T12AX7["cgp_stray"] = ph["cgp_stray"]
    P6L6GC["kg1"] = _P6L6GC["kg1"] / ph["p_scale"]; P6L6GC["kv"] = _P6L6GC["kv"] * ph["p_scale"]
    A, A5 = ph["taper_a_mid"], ph["taper_5a_mid"]
    has = lambda s: s in sections

    if has("input"):
        c.V(vin_name, "in", GND, 0.0, is_input=True)
        if k["input"] == "high":           # both 22k in parallel (normal-jack switch closed)
            c.R("R27", "in", "nB", 22e3); c.R("R30", "in", "nB", 22e3)
        else:                              # normal input: R27 is grounded by the unused high jack
            c.R("R30", "in", "nB", 22e3); c.R("R27", "nB", GND, 22e3)
        c.C("C20", "nB", GND, 39e-12)
        c.C("C15", "nB", "nC", 0.1e-6)
        c.R("R25", "nC", GND, 1e6)
        c.R("R22", "nC", "g1a", 68e3)
    if has("v1a"):
        c.triode("V1A", "p1a", "g1a", "k1a", T12AX7)
        c.R("R2", "v1", "p1a", 220e3)
        c.R("R12", "k1a", GND, 1.82e3); c.C("C3", "k1a", GND, 1e-6)
    if has("chan"):
        c.C("C6", "p1a", "nD", 22e-9)
        # lead network (always connected to nD)
        c.R("R29", "nD", "nLt", ph["r29"]); c.C("C17", "nD", "nLt", ph["c17"])   # r29 = 1e9: not fitted
        c.pot("VR2", "nLt", "nLw", GND, 1e6, taper_log(_x(k, "pre_lead"), A5))
        c.R("R41", "nLt", GND, 1e6)
        c.C("C19", "nLt", "nLw", 1e-9); c.R("R40", "nLt", "nLw", 1e6)
        # rhythm network; K4 (crunch relay) picks the series element and the 39k shunt
        if k["crunch"]:
            c.R("R43", "nD", "nG", 2.2e6); c.C("C13", "nD", "nG", 470e-12)
        else:
            c.R("R19", "nD", "nG", 470e3); c.C("C51", "nD", "nG", 270e-12)
            c.R("R23", "nJ", "n63", 39e3); c.C("C63", "n63", GND, 0.1e-6)
        c.R("R21", "nG", "nJ", 1e6)
        c.C("C10", "nG", "nK", 470e-12)
        c.R("R75", "nJ", "nK", 1.0 if k["bright"] else 2.2e6)   # bright switch shorts R75
        c.pot("VR1", "nJ", "nW1", GND, 1e6, taper_log(_x(k, "pre_rhythm"), A5))
        c.C("C27", "nJ", "nW1", 180e-12)
        c.R("R82", "nLw" if lead else "nW1", "g1b", 470e3)        # K2A
    if has("v1b"):
        c.triode("V1B", "p1b", "g1b", "k1b", T12AX7)
        c.R("R3", "v1", "p1b", 100e3)
        c.R("R17", "k1b", GND, 1.82e3); c.C("C4", "k1b", GND, 1e-6)
        c.C("C26", "k1b", "n26", 22e-6)
        c.R("R18", "n26", GND, 1.0 if k1_closed else 47e3)       # K1A grounds C26
    if has("ic2"):
        c.C("C2", "p1b", "n2", ph["c2"])
        c.R("R6", "n2", "g2a", 470e3)
        c.R("R9", "g2a", GND, 1e6)
        if k1_closed:
            c.R("R20", "g2a", GND, 220e3)                        # K1B
    if has("v2a"):
        c.triode("V2A", "p2a", "g2a", "k2a", T12AX7)
        c.R("R1", "v1", "p2a", 100e3)
        c.R("R15", "k2a", GND, 39e3)
        c.C("C1", "p2a", "n1", 22e-9)
        c.C("C48", "n1", GND, 1e-9); c.R("R11", "n1", GND, 330e3)
        c.R("R7", "n1", "g2b", 220e3)
    if has("v2b"):
        c.triode("V2B", "p2b", "g2b", "k2b", T12AX7)
        c.R("R4", "v2", "p2b", 220e3)
        c.R("R13", "k2b", GND, 1.82e3); c.C("C5", "k2b", GND, 1e-6)
    if has("i5b"):
        c.R("R32", "p2b", "n32", 1e6)
        c.C("C57", "n32", "g5b", 22e-9)
        c.R("R101", "g5b", GND, 100e3)
    if has("v5b"):
        c.triode("V5B", "p5b", "g5b", "k5b", T12AX7)
        c.R("R96", "v2", "p5b", 220e3)
        c.R("R102", "k5b", GND, 2.2e3)
    if has("i5a"):
        c.R("R87", "p5b", "n87", 1e6)
        c.C("C56", "n87", "g5a", 22e-9)
        c.R("R91", "g5a", GND, 1e6)
        c.R("R88", "g5a", "nX", 1e6)
    if has("v5a"):
        c.triode("V5A", "p5a", "g5a", "k5a", T12AX7)
        c.R("R86", "v2", "p5a", 100e3)
        c.R("R93", "k5a", GND, 2.2e3)
        c.C("C58", "p5a", "nX", 22e-9)
    if has("tone"):
        c.R("R89", "nX", "nT", 470e3)
        c.R("R24", "nT", GND, 33e3)
        c.C("C59", "nT", "n59", 10e-9); c.R("R94", "n59", GND, 47e3)
        c.R("R28", "nT", "nU", 47e3)
        c.C("C14", "nT", "nTT", 470e-12); c.C("C60", "nT", "nTT", 100e-12)
        c.pot("VR5", "nTT", "nTW", "nW", 250e3, _x(k, "high"))
        c.C("C18", "nU", "nW", 22e-9)
        c.R("VR3", "nW", "nY", max(1e6 * taper_log(_x(k, "low"), A), 1.0))
        c.R("R95", "nY", GND, 47e3)
        c.pot("VR4", "nY", "nMW", GND, 50e3, _x(k, "mid"))
        c.C("C21", "nU", "nMW", 22e-9)
        c.pot("VR6", "nTW", "nPR", GND, 1e6, taper_log(_x(k, "post_rhythm"), A))
        c.pot("VR7", "nTW", "nPL", GND, 1e6, taper_log(_x(k, "post_lead"), A))
        pg = "nPL" if lead else "nPR"                            # K2B
        c.R("R81", pg, GND, 2.2e6)
        c.C("C52", pg, "g3b", 22e-9)
    if has("cf"):
        c.triode("V3B", "v2", "g3b", "k3b", T12AX7)
        c.R("R79", "g3b", "nb3", 1e6)
        c.R("R80", "k3b", "nb3", 1e3)
        c.R("R16", "nb3", GND, 100e3)
    if has("loop"):
        c.C("C53", "k3b", "nM", 0.33e-6); c.R("R103", "nM", GND, 100e3)
        c.C("C65", "nM", "nN", 0.33e-6); c.R("R33", "nN", GND, 100e3)
        c.R("R85", "nN", GND, 220e3)                             # loop bypassed: send = return
        c.C("C11", "nN", "g3a", 47e-9)
        c.R("R26", "g3a", GND, 1e6)
    if has("v3a"):
        c.triode("V3A", "p3a", "g3a", "k3a", T12AX7)
        c.R("R5", "v2", "p3a", 68e3)
        c.R("R14", "k3a", GND, 3.3e3); c.C("C61", "k3a", GND, 4.7e-9)
    if has("ipi"):
        c.C("C49", "p3a", "nP", 22e-9)
        c.R("R104", "nP", "n104", 100e3); c.C("C66", "n104", "npo", 0.1e-6); c.R("R100", "npo", GND, 1e6)
        c.R("R50", "nP", "nQ", 1e6)
        c.R("R48", "nP", "g4b", 100e3)
    if has("pi"):
        c.triode("V4B", "p4b", "g4b", "kpi", T12AX7)
        c.triode("V4A", "p4a", "g4a", "kpi", T12AX7)
        c.R("R52", "kpi", "nQ", 470.0)
        c.R("R51", "nQ", "nR", 10e3)
        c.R("R57", "nR", GND, 4.7e3)
        c.R("R53", "g4a", "nQ", 1e6)
        c.C("C31", "g4a", "nR", 47e-9)
        c.R("R46", "v3", "p4b", 82e3)
        c.R("R58", "v3", "p4a", 100e3)
        c.C("C30", "nR", "nS", 22e-6)
        # feedback network: 39k, resonance (1M rheostat || 6.8n), presence (0.133u + 10k rheostat)
        c.R("R56", "fb", "n34", 39e3)
        c.R("VR8", "n34", "nS", max(1e6 * taper_log(_x(k, "resonance"), A5), 1.0))
        c.C("C12", "n34", "nS", 6.8e-9)
        c.C("C8", "nS", "nprs", 0.1e-6); c.C("C62", "nS", "nprs", 0.033e-6)
        c.R("VR9", "nprs", GND, max(10e3 * (1.0 - taper_log(_x(k, "presence"), A)), 1.0))
        c.C("C28", "p4b", "ga", 47e-9); c.R("R54", "ga", "bias", 220e3)
        c.C("C29", "p4a", "gb", 47e-9); c.R("R55", "gb", "bias", 220e3)
    if has("power"):
        # two 6L6GC per side lumped into one device (n_parallel=2); per-tube parts halved
        c.C("C202", "ga", GND, 100e-12); c.C("C201", "gb", GND, 100e-12)
        c.R("Rgsa", "ga", "g6a", 1.1e3); c.R("Rgsb", "gb", "g6b", 1.1e3)
        c.R("Rsa", "sc", "s6a", 50.0); c.R("Rsb", "sc", "s6b", 50.0)
        c.pentode("V6A", "pa", "s6a", "g6a", GND, P6L6GC, n_parallel=2)
        c.pentode("V6B", "pb", "s6b", "g6b", GND, P6L6GC, n_parallel=2)
        c.diode("CR200", GND, "pa", D1N4007); c.diode("CR201", GND, "pb", D1N4007)
        n = math.sqrt(ph["raa"] / ph["rload"])           # full primary : load winding
        # leakage and copper resistance in series with each half primary, then the ideal transformer
        c.R("Rpa", "pa", "pa1", ph["rpri_half"]); c.L("Lla", "pa1", "pa2", ph["lleak"] / 2)
        c.R("Rpb", "pb", "pb1", ph["rpri_half"]); c.L("Llb", "pb1", "pb2", ph["lleak"] / 2)
        c.C("Cpa", "pa", GND, ph["cpri"]); c.C("Cpb", "pb", GND, ph["cpri"])
        c.xfmr("T2", [("pa2", "b", n / 2), ("b", "pb2", n / 2), ("sec0", GND, 1.0), ("fb", GND, 1.0 / math.sqrt(2.0))])
        c.L("Lm", "pa2", "pb2", ph["lm"])
        c.R("Rsec", "sec0", "spk", ph["rsec"])
        if ph["load"] == "reactive":
            c.R("Rspk_e", "spk", "sp1", ph["spk_re"])
            c.L("Lspk_e", "sp1", "sp1b", ph["spk_le"]); c.R("Rspk_le", "sp1", "sp1b", ph["spk_rle"])
            if ph["spk_le2"] > 0.0:
                c.L("Lspk_e2", "sp1b", "sp2", ph["spk_le2"]); c.R("Rspk_le2", "sp1b", "sp2", ph["spk_rle2"])
            else:                                        # single-section load (a typical reactive load box)
                c.R("Rspk_j", "sp1b", "sp2", 1e-3)
            c.R("Rspk_p", "sp2", GND, ph["spk_rp"]); c.L("Lspk_p", "sp2", GND, ph["spk_lp"]); c.C("Cspk_p", "sp2", GND, ph["spk_cp"])
        else:
            c.R("Rload", "spk", GND, ph["rload"])
        c.V("bias0", "bias_s", GND, ph["vbias"]); c.R("Rbias", "bias_s", "bias", ph["rbias"]); c.C("C41", "bias", GND, 10e-6)
    if has("supply"):
        c.V("b0", "b_s", GND, ph["vb0"]); c.R("Rsrc", "b_s", "b", ph["rsrc"])
        c.C("C34_38", "b", GND, 165e-6)                  # 2 x 330 uF in series
        c.R("R210", "b", "sc", 400.0)
        c.C("C35_39", "sc", GND, 50e-6)                  # 2 x 100 uF in series
        c.R("R59", "sc", "v3", 4e3); c.C("C36", "v3", GND, 47e-6)
        c.R("R60", "v3", "v2", 22e3); c.C("C37", "v2", GND, 22e-6)
        c.R("R61", "v2", "v1", 10e3); c.C("C33", "v1", GND, 22e-6)
    return c
