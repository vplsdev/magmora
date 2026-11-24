"""Gate G-P1: the pedal prototype against ngspice on the same netlist, and the X3 study.

    source scripts/env.sh
    $BLOCKAMP_PY eval/check_pedal_spice.py --part op|ac|closed|harm|x3|all [--rate standard|high|both] [--workers 3]

Parts:
  op      operating point of every node, prototype against ngspice (gate 5 mV)
  ac      small-signal response, Drive x Tone grid of 9, 20 Hz to 20 kHz (gate 0.25 dB Standard, 0.1 dB High)
  closed  the hand-written stage equations (pedal.closed_form) against ngspice's .ac of the netlist (gate 0.25 dB)
  harm    sine drives at 110 / 440 / 2000 Hz from 1 mV to 3 V peak: every harmonic within 60 dB of the strongest and
          below 20 kHz (gate 1.0 dB Standard, 0.5 dB High)
  x3      SPICE only: the shipped model against full device models (ngspice Gummel-Poon followers, tanh input
          stage that limits the slew rate), to decide at 0.5 dB

Rates: "standard" is 264.6 kHz and "high" 352.8 kHz, the lowest internal rates the product runs those qualities at
(6x and 8x of 44.1 kHz), so the figures are the pessimistic ones. SPICE runs are cached under
$BLOCKAMP_PRIVATE/build/xp1-pedal/cache (keyed by the deck text). Exit status 1 if a gate fails.
"""
import argparse, hashlib, json, os, sys, time
from multiprocessing import Pool
from pathlib import Path
import numpy as np
from blockamp import pedal, linear, ngspice

RATES = dict(standard=264600.0, high=352800.0)
GATES = dict(op_v=5e-3, ac_db=dict(standard=0.25, high=0.1), closed_db=0.25, h_db=dict(standard=1.0, high=0.5), x3_db=0.5)
OPTS = ".options reltol=1e-5 vntol=1e-9 abstol=1e-14 method=trap maxord=2 itl4=100 gmin=1e-15 temp=25 tnom=25\n"
CACHE = Path(os.environ.get("BLOCKAMP_PRIVATE", ".")) / "build" / "xp1-pedal" / "cache"
# ngspice writes its result on a uniform grid by interpolation ("linearize"). On a 384 kHz grid that interpolation
# itself put 0.9 dB of error on the even harmonics 59 dB down (found by convergence: the prototype at 0.7 to 2.8 MHz
# and ngspice on a 1.536 MHz grid agree within 0.02 dB there). The grid is therefore 1.536 MHz, with the same largest
# internal step as before.
FS_SPICE = 1536000.0
T_TOTAL, T_ANA = 0.30, 0.10          # seconds simulated / analysed (0.1 s holds whole periods of all three tones)
GRID9 = [dict(drive=d, tone=t, level=10.0) for d in (0.0, 5.0, 10.0) for t in (0.0, 5.0, 10.0)]
HARM_SETTINGS = {"boost (drive 0, tone 5, level 10)": dict(drive=0.0, tone=5.0, level=10.0),
                 "noon (5, 5, 5)": dict(drive=5.0, tone=5.0, level=5.0),
                 "drive 10 (tone 5, level 5)": dict(drive=10.0, tone=5.0, level=5.0),
                 "all at 10": dict(drive=10.0, tone=10.0, level=10.0)}
FREQS = (110.0, 440.0, 2000.0)
LEVELS = (1e-3, 1e-2, 0.1, 0.3, 1.0, 3.0)
PROBES = ("out", "o1")


def spice_ac(c, probes, f_lo=20.0, f_hi=20e3, per_dec=30):
    deck = pedal.spice_deck2(c, {"in": "dc 0 ac 1"}) + OPTS
    deck += f".control\nac dec {per_dec} {f_lo} {f_hi}\nwrdata o.data " + " ".join(f"v({p})" for p in probes) + "\n.endc\n.end\n"
    log, out = ngspice.run_deck(deck, timeout=120)
    if "o" not in out: raise RuntimeError(log[-2000:])
    a = out["o"]
    return a[:, 0], {p: a[:, 3 * i + 1] + 1j * a[:, 3 * i + 2] for i, p in enumerate(probes)}


def spice_op(c, nodes):
    deck = pedal.spice_deck2(c) + OPTS + ".control\nop\nprint " + " ".join(f"v({n})" for n in nodes) + "\n.endc\n.end\n"
    log, _ = ngspice.run_deck(deck, timeout=120)
    res = {}
    for line in log.splitlines():
        if line.startswith("v(") and "=" in line:
            k, v = line.split("="); res[k.strip()[2:-1]] = float(v)
    if len(res) < len(nodes): raise RuntimeError(log[-2000:])
    return {n: res[n.lower()] for n in nodes}


def spice_tran(args):
    """One sine case in ngspice. Cached. Returns {probe: samples of the last T_ANA seconds on the FS_SPICE grid}."""
    ctl, variant, f0, amp, probes = args
    c = pedal.build2(ctl, variant)
    step = 1.0 / FS_SPICE
    deck = pedal.spice_deck2(c, {"in": f"sin(0 {amp:.9g} {f0:.9g})"}) + OPTS
    if c.raw:        # ngspice's transistor model needs a starting point for its operating point: the product model's own
        ref = pedal.Sim2(pedal.build2(ctl), RATES["standard"])
        deck += ".nodeset " + " ".join(f"v({n})={ref.node_dc(n):.6f}" for n in ref.c.node_names()) + "\n"
    deck += (f".control\ntran {step:.9e} {T_TOTAL:.9e} {T_TOTAL - T_ANA - step:.9e} {step:.9e}\nlinearize\nwrdata o.data "
             + " ".join(f"v({p})" for p in probes) + "\n.endc\n.end\n")
    key = hashlib.sha256(deck.encode()).hexdigest()[:24]
    fn = CACHE / f"{key}.npz"
    if fn.exists():
        z = np.load(fn); return {p: z[p] for p in probes}
    log, out = ngspice.run_deck(deck, timeout=160)
    if "o" not in out: raise RuntimeError(log[-2000:])
    a = out["o"]; n = int(round(T_ANA * FS_SPICE)); res = {p: a[-n:, 2 * i + 1].astype(np.float64) for i, p in enumerate(probes)}
    CACHE.mkdir(parents=True, exist_ok=True); np.savez(fn, **res)
    return res


def proto_tran(ctl, fs, f0, amp):
    sim = pedal.SplitSim(ctl, None, fs) if SPLIT else pedal.Sim2(pedal.build2(ctl), fs)
    n = int(round(T_TOTAL * fs)); t = np.arange(n) / fs
    if SPLIT:
        y = sim.run(amp * np.sin(2 * np.pi * f0 * t), PROBES)
        return {p: y[-int(round(T_ANA * fs)):, i] for i, p in enumerate(PROBES)}, sim.fails, float(np.mean(sim.iters_per_sample))
    y = sim.run(amp * np.sin(2 * np.pi * f0 * t))
    names = sim.S.out_names
    return {p: y[-int(round(T_ANA * fs)):, names.index(p)] for p in PROBES}, sim.fails, sim.iters / sim.samples


def harmonics(x, fs, f0):
    """Amplitudes of the harmonics of f0 below 20 kHz. The window holds a whole number of periods at every rate used
    here, so each harmonic is one FFT bin; a linear drift (the output capacitor still settling) is taken out first,
    estimated from the means of the first and the last period."""
    n = len(x); per = fs / f0; cycles = n / per
    assert abs(cycles - round(cycles)) < 1e-6 and n > 2 * per, (n, fs, f0)
    m = int(round(per))
    slope = (x[-m:].mean() - x[:m].mean()) / (n - m)
    X = np.abs(np.fft.rfft(x - slope * (np.arange(n) - n / 2))) / (n / 2)
    return X[int(round(cycles)) * np.arange(1, int(20000.0 // f0) + 1)]


def harm_err(a, fa, b, fb, f0):
    """Worst dB difference of a against reference b over the harmonics within 60 dB of b's strongest; and which."""
    ha, hb = harmonics(a, fa, f0), harmonics(b, fb, f0)
    m = np.where(hb > hb.max() * 1e-3)[0]
    d = 20 * np.log10((ha[m] + 1e-18) / (hb[m] + 1e-18))
    j = int(np.argmax(np.abs(d)))
    return float(abs(d[j])), int(m[j] + 1), float(20 * np.log10(hb[m[j]] / hb.max()))


def part_op(rates):
    worst = 0.0
    for ctl in (GRID9[0], GRID9[4], GRID9[8]):
        c = pedal.build2(ctl); sim = pedal.Sim2(c, RATES["standard"])
        nodes = [n for n in c.node_names()]
        sp = spice_op(c, nodes)
        d = {n: abs(sim.node_dc(n) - sp[n]) for n in nodes}
        n = max(d, key=d.get); worst = max(worst, d[n])
        print(f"  op   drive {ctl['drive']:4.1f} tone {ctl['tone']:4.1f}: {len(nodes)} nodes, worst {d[n] * 1e3:.4f} mV at {n}")
    ok = worst <= GATES["op_v"]
    print(f"op: worst {worst * 1e3:.4f} mV, gate {GATES['op_v'] * 1e3:.0f} mV: {'pass' if ok else 'FAIL'}")
    return ok, dict(op_worst_mV=worst * 1e3)


def part_ac(rates):
    ok = True; res = {}
    for rate in rates:
        fs = RATES[rate]; worst = (0.0, None); w10 = 0.0
        for ctl in GRID9:
            c = pedal.build2(ctl)
            f, H = spice_ac(c, PROBES)
            sim = pedal.Sim2(c, fs)
            P = sim.response(f)
            for i, p in enumerate(PROBES):
                d = np.abs(20 * np.log10(np.abs(P[:, sim.S.out_names.index(p)]) / np.abs(H[p])))
                j = int(np.argmax(d)); w10 = max(w10, float(d[f <= 10000.0].max()))
                if d[j] > worst[0]: worst = (float(d[j]), f"drive {ctl['drive']:.0f} tone {ctl['tone']:.0f} probe {p} at {f[j]:.0f} Hz")
        g = GATES["ac_db"][rate]; good = worst[0] <= g; ok &= good
        print(f"ac ({rate}, {fs / 1e3:.1f} kHz): worst {worst[0]:.3f} dB ({worst[1]}), gate {g} dB: {'pass' if good else 'FAIL'}; up to 10 kHz the worst is {w10:.3f} dB")
        res[f"ac_{rate}_dB"] = worst[0]; res[f"ac_{rate}_10k_dB"] = w10
    return ok, res


def part_closed(rates):
    worst = (0.0, None)
    for ctl in GRID9 + [dict(drive=5.0, tone=5.0, level=5.0), dict(drive=10.0, tone=5.0, level=0.5)]:
        f, H = spice_ac(pedal.build2(ctl), PROBES)
        for p in PROBES:
            d = np.abs(20 * np.log10(np.abs(pedal.closed_form(f, ctl, probe=p)) / np.abs(H[p])))
            j = int(np.argmax(d))
            if d[j] > worst[0]: worst = (float(d[j]), f"drive {ctl['drive']:.0f} tone {ctl['tone']:.0f} level {ctl['level']:.1f} probe {p} at {f[j]:.0f} Hz")
    good = worst[0] <= GATES["closed_db"]
    print(f"closed form against the netlist in ngspice: worst {worst[0]:.3f} dB ({worst[1]}), gate {GATES['closed_db']} dB: {'pass' if good else 'FAIL'}")
    return good, dict(closed_dB=worst[0])


def _cases(settings):
    return [(name, ctl, f0, a) for name, ctl in settings.items() for f0 in FREQS for a in LEVELS]


def part_harm(rates, workers, budget=100.0):
    t0 = time.time(); base = {}
    cases = _cases(HARM_SETTINGS)
    with Pool(workers) as pool:
        it = pool.imap(spice_tran, [(ctl, base, f0, a, PROBES) for _, ctl, f0, a in cases])
        ref = []
        for _ in cases:
            if time.time() - t0 > budget and len(ref) < len(cases):
                try: ref.append(it.next(timeout=max(1.0, budget + 15 - (time.time() - t0))))
                except Exception: break
            else: ref.append(it.next())
    if len(ref) < len(cases):
        print(f"harm: {len(ref)} of {len(cases)} SPICE cases cached within the time budget; run again to continue"); return False, {}
    ok = True; res = {}
    for rate in rates:
        fs = RATES[rate]; worst = (0.0, None); nfail = 0; fails = 0; rows = []
        for (name, ctl, f0, a), r in zip(cases, ref):
            y, fl, itr = proto_tran(ctl, fs, f0, a); fails += fl
            for p in PROBES:
                e, h, lvl = harm_err(y[p], fs, r[p], FS_SPICE, f0)
                rows.append((e, name, f0, a, p, h, lvl))
                if e > GATES["h_db"][rate]: nfail += 1
                if e > worst[0]: worst = (e, f"{name}, {f0:.0f} Hz, {a:g} V, probe {p}, harmonic {h} at {lvl:.0f} dB")
        g = GATES["h_db"][rate]; good = worst[0] <= g; ok &= good
        print(f"harm ({rate}, {fs / 1e3:.1f} kHz): {len(rows)} probe-cases, worst {worst[0]:.3f} dB ({worst[1]}), gate {g} dB: "
              f"{'pass' if good else 'FAIL'}; {nfail} over the gate; solver failures {fails}")
        for e, name, f0, a, p, h, lvl in sorted(rows, reverse=True)[:5]:
            print(f"    {e:6.3f} dB  {name}, {f0:.0f} Hz, {a:g} V, {p}, harmonic {h} ({lvl:.0f} dB re strongest)")
        res[f"harm_{rate}_dB"] = worst[0]; res[f"harm_{rate}_over"] = nfail
    return ok, res


X3_VARIANTS = {"the product model (junction law with the Early term, tanh input stages)": {},
               "the same without the Early term": dict(q_vaf=0.0)}
X3_FULL = dict(spice_q=True)             # ngspice's own Gummel-Poon transistors (Early voltage, junction capacitances)
SPLIT = False                            # --split: run the prototype as the four clusters instead of one


def part_x3(workers, budget=120.0):
    t0 = time.time(); cases = _cases(HARM_SETTINGS)
    jobs = [(ctl, v, f0, a, PROBES) for v in [X3_FULL] + list(X3_VARIANTS.values()) for _, ctl, f0, a in cases]
    out = []
    with Pool(workers) as pool:
        it = pool.imap(spice_tran, jobs)
        for _ in jobs:
            try: out.append(it.next(timeout=max(1.0, budget - (time.time() - t0))))
            except Exception: break
    if len(out) < len(jobs):
        print(f"x3: {len(out)} of {len(jobs)} SPICE cases cached within the time budget; run again to continue"); return False, {}
    n = len(cases); full = out[:n]; res = {}
    print("x3: each model against ngspice's own transistor model in the followers, SPICE on both sides")
    for vi, name in enumerate(X3_VARIANTS):
        mine = out[n * (vi + 1):n * (vi + 2)]; rows = []
        for (sname, ctl, f0, a), r, y in zip(cases, full, mine):
            e, h, lvl = harm_err(y["out"], FS_SPICE, r["out"], FS_SPICE, f0)
            rows.append((e, sname, f0, a, h, lvl))
        rows.sort(reverse=True); over = sum(1 for r in rows if r[0] > GATES["x3_db"])
        by_level = {a: max(r[0] for r in rows if r[3] == a) for a in LEVELS}
        print(f"  {name}: worst {rows[0][0]:.2f} dB ({rows[0][1]}, {rows[0][2]:.0f} Hz, {rows[0][3]:g} V, harmonic {rows[0][4]} at {rows[0][5]:.0f} dB); "
              f"{over} of {len(rows)} cases over {GATES['x3_db']} dB")
        print("      worst by input level: " + ", ".join(f"{a:g} V {v:.2f}" for a, v in by_level.items()))
        res[name] = dict(worst_dB=rows[0][0], over=over, by_level=by_level)
    return True, res


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--part", default="all", choices=["op", "ac", "closed", "harm", "x3", "all"])
    ap.add_argument("--rate", default="both", choices=["standard", "high", "both"])
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--json"); ap.add_argument("--split", action="store_true")
    a = ap.parse_args(); SPLIT = a.split
    rates = ["standard", "high"] if a.rate == "both" else [a.rate]
    parts = ["op", "ac", "closed", "harm"] if a.part == "all" else [a.part]
    ok = True; summary = {}
    for p in parts:
        good, r = (part_op(rates) if p == "op" else part_ac(rates) if p == "ac" else part_closed(rates) if p == "closed"
                   else part_harm(rates, min(a.workers, 3)) if p == "harm" else part_x3(min(a.workers, 3)))
        if p != "x3": ok &= good
        summary.update({k: v for k, v in r.items()})
    if a.json:
        Path(a.json).write_text(json.dumps(summary, indent=1, default=float))
    sys.exit(0 if ok else 1)
