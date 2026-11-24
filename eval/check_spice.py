"""Gate G1: the engine against the independent ngspice reference (scripts/gen_reference.py).

    source scripts/env.sh && $BLOCKAMP_PY eval/check_spice.py [--ref DIR] [--osf 4|8] [--mono] [--only PATTERN] [--json OUT]

Backends: the Python prototype (default) or, once it exists, the C++ probe tool (--cli PATH: dsp/tools/magmora-probe).
For every transient case it reports, per probe:
  h_db    worst error of any harmonic within 60 dB of the strongest one and below 20 kHz (sine cases)
  nrmse   waveform error after sub-sample time alignment, dB re the reference RMS
and for the special cases the recovery (burst) and supply (chord) errors. Exit status 1 if a gate fails.
"""
import argparse, fnmatch, json, os, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from blockamp import amp, signals, partition
from blockamp.sim import Sim

# Gates for the High quality (internal rate 384 kHz). At Standard (192 kHz) the trapezoidal rule's frequency
# warping alone costs about 1 dB at 20 kHz (see the AC gate), so the harmonic and waveform gates are wider there.
GATES = dict(h_db=1.0, nrmse_db=-20.0, recovery_db=1.0, supply_v=1.5, stage_nrmse_db=-30.0, ac_db_10k=0.5, ac_db_20k=1.5)
GATES_STANDARD = dict(GATES, h_db=1.5, nrmse_db=-12.0)
CHECK = ["nX", "p3a", "spk"]


def align_nrmse(a, b):
    """NRMSE (dB) of a against b after the best sub-sample shift of a (FFT phase ramp), mean removed."""
    a = a - a.mean(); b = b - b.mean()
    n = len(a); A = np.fft.rfft(a); B = np.fft.rfft(b)
    w = np.fft.rfftfreq(n) * 2 * np.pi
    best = 1e9
    for d in np.linspace(-1.5, 1.5, 61):
        e = np.fft.irfft(A * np.exp(-1j * w * d), n) - b
        best = min(best, np.sqrt(np.mean(e[64:-64] ** 2)))
    return 20 * np.log10(best / (b.std() + 1e-20) + 1e-12)


def harmonics(x, fs, f0, nmax=40):
    per = fs / f0
    n = int(round(int(len(x) / per) * per))
    X = np.abs(np.fft.rfft(x[-n:] * np.hanning(n))) / (n / 4)
    f = np.fft.rfftfreq(n, 1 / fs)
    out = []
    for k in range(1, nmax + 1):
        if k * f0 > 20000: break
        i = int(round(k * f0 * n / fs)); out.append(X[max(i - 2, 0):i + 3].max())
    return np.array(out)


def harm_err(a, b, fs, f0):
    ha, hb = harmonics(a, fs, f0), harmonics(b, fs, f0)
    m = hb > hb.max() * 1e-3
    return float(np.max(np.abs(20 * np.log10((ha[m] + 1e-15) / (hb[m] + 1e-15))))), float(20 * np.log10(ha[0] / hb[0]))


def tone_level(x, fs, f, frame):
    n = int(frame * fs); k = len(x) // n
    t = np.arange(n) / fs
    c = np.exp(-2j * np.pi * f * t) * np.hanning(n)
    return 20 * np.log10(np.abs(x[:k * n].reshape(k, n) @ c) / n * 4 + 1e-12)


class ProtoBackend:
    def __init__(self, osf, mono):
        self.osf, self.mono = osf, mono

    def run(self, setting, sig, fs_ref, probes):
        kind, kw = sig
        fs = fs_ref * self.osf // 4          # reference grid is 192 kHz = "4x"; 8x runs at 384 kHz
        x = getattr(signals, kind)(fs, **kw)
        step = max(fs // fs_ref, 1)            # below the reference rate the caller thins the reference instead
        if self.mono:
            allp = list(probes) + ["spk"]
            c = amp.build(setting); [c.out(p, p) for p in allp]
            return {p: y for p, y in zip(allp, Sim(c, fs).run(x)[::step].T)}
        ps = partition.PartSim(setting, fs=fs, probes=probes, clusters=partition.CLUSTERS_SPLIT)
        outs = [k for k in ps.out_index if k[1].startswith("probe_") or k == ("P", "spk")]
        y = ps.run(x, outs)[::step]
        return {(k[1][6:] if k[1].startswith("probe_") else "spk"): y[:, i] for i, k in enumerate(outs)}


def stage_rows(ref, man, fs, only):
    """Each cluster alone against ngspice on the same cluster netlist (ideal sources at the cuts)."""
    from blockamp import dk
    rows = []
    ps = partition.PartSim(man["settings"]["lead_default"], fs=fs, companion=False)       # same construction as gen_reference.run_stage
    for name, case in man["stage"].items():
        if not fnmatch.fnmatch(name, only) or not (ref / "stage" / f"{name}.in.f64").exists():
            continue
        cl = next(c for c in ps.cl if c["name"] == case["cluster"]); c = cl["circuit"]
        src = c.inputs[0] if case["cluster"] == "A" else "cut_in"
        x = np.fromfile(ref / "stage" / f"{name}.in.f64", dtype="<f8")
        sim = Sim(c, fs)
        u = np.tile(sim.S.vdc[sim.S.in_idx], (len(x), 1)); u[:, c.inputs.index(src)] = x
        y = sim.run(u)
        row = dict(case="stage " + name)
        for k, o in enumerate(c.outputs):
            if o[1] == "#i" or o[0].startswith("v_v") or o[0] in ("v_b", "v_sc"): continue      # rails are quasi-DC here
            r = np.fromfile(ref / "stage" / f"{name}.{o[0]}.f32", dtype="<f4").astype(float)
            n = min(len(r), len(y)); tail = slice(n // 2, n)
            if r[tail].std() < 1e-6: continue
            row[f"{o[0]}_nrmse_db"] = float(align_nrmse(y[tail, k], r[tail]))
        row["fail"] = [k for k, v in row.items() if k.endswith("_nrmse_db") and v > GATES["stage_nrmse_db"]]
        rows.append(row)
        print(row["case"], " ".join(f"{k}={v:.1f}" for k, v in row.items() if isinstance(v, float)), "FAIL" if row["fail"] else "ok", flush=True)
    return rows


def ac_rows(ref, man, osf, only):
    """Small-signal responses of the monolithic prototype at the internal rate against ngspice .ac."""
    from blockamp import dk, linear
    rows = []
    fs = man["fs"] * osf // 4
    for name in man["settings"]:
        f_csv = ref / "ac" / f"{name}.csv"
        if not fnmatch.fnmatch(name, only) or not f_csv.exists():
            continue
        a = np.genfromtxt(f_csv, delimiter=",", names=True); f = a["freq_hz"]
        probes = [n[:-3] for n in a.dtype.names if n.endswith("_db")]
        c = amp.build(man["settings"][name]); [c.out(p, p) for p in probes]
        H = linear.response(dk.build(c, fs), f)
        d = np.array([20 * np.log10(np.abs(H[:, k]) + 1e-30) - a[p + "_db"] for k, p in enumerate(probes)])
        valid = np.array([a[p + "_db"] for p in probes]) > -200
        lo = (f >= 20) & (f <= 10000); hi = (f > 10000) & (f <= 20000)
        row = dict(case="ac " + name, ac_db_10k=float(np.max(np.abs(d[:, lo])[valid[:, lo]])), ac_db_20k=float(np.max(np.abs(d[:, hi])[valid[:, hi]])))
        row["fail"] = [k for k in ("ac_db_10k", "ac_db_20k") if row[k] > GATES[k] * (1.0 if osf <= 4 else 0.4)]
        rows.append(row)
        print(row["case"], f"max |diff| to 10 kHz {row['ac_db_10k']:.3f} dB, 10-20 kHz {row['ac_db_20k']:.3f} dB", "FAIL" if row["fail"] else "ok", flush=True)
    return rows


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default=os.path.join(os.environ.get("BLOCKAMP_PRIVATE", "."), "reference"))
    ap.add_argument("--osf", type=int, default=4); ap.add_argument("--mono", action="store_true")
    ap.add_argument("--only", default="*"); ap.add_argument("--json", default=None)
    a = ap.parse_args()
    ref = Path(a.ref); man = json.loads((ref / "manifest.json").read_text()); fs = man["fs"]
    if a.osf <= 4:
        GATES.update(GATES_STANDARD)
    be = ProtoBackend(a.osf, a.mono)
    rstep = max(4 // a.osf, 1); fs_cmp = fs // rstep      # the 192 kHz reference is point-sampled, so it may be thinned
    rows, fail = [], 0
    dc = json.loads((ref / "dc_op.json").read_text())
    print(f"dc operating point: max |ngspice - engine| = {dc['max_abs_diff_v']*1e3:.1f} mV (gate 50 mV)")
    fail += dc["max_abs_diff_v"] > 0.05
    for r in ac_rows(ref, man, a.osf, a.only) + (stage_rows(ref, man, fs, a.only) if a.osf == 4 else []):
        rows.append(r); fail += bool(r["fail"])
    for name, case in man["tran"].items():
        if not fnmatch.fnmatch(name, a.only) or not (ref / "tran" / f"{name}.spk.f32").exists():
            continue
        probes = CHECK + (["b", "sc", "v3", "v2"] if name.startswith("chord") else [])
        got = be.run(man["settings"][case["setting"]], case["sig"], fs, [p for p in probes if p != "spk"])
        row = dict(case=name)
        for p in probes:
            r = np.fromfile(ref / "tran" / f"{name}.{p}.f32", dtype="<f4").astype(float)[::rstep]
            g = got[p]; n = min(len(r), len(g)); r, g = r[:n], g[:n]
            if p in ("b", "sc", "v3", "v2"):
                row[f"{p}_v"] = float(np.max(np.abs(r - g))); continue
            tail = slice(n // 2, n)
            if case["sig"][0] == "sine":
                f0 = case["sig"][1]["f0"]
                hd, g1 = harm_err(g[tail], r[tail], fs_cmp, f0)
                row[f"{p}_h_db"] = hd; row[f"{p}_gain_db"] = g1
            row[f"{p}_nrmse_db"] = float(align_nrmse(g[tail], r[tail]))
            if name.startswith("burst") and p == "spk":
                la, lb = tone_level(g, fs_cmp, 440.0, 0.01), tone_level(r, fs_cmp, 440.0, 0.01)
                k0 = int(0.12 / 0.01) + 1
                row["recovery_db"] = float(np.max(np.abs(la[k0:] - lb[k0:])))
        bad = []
        for k, v in row.items():
            if k.endswith("_h_db") and v > GATES["h_db"]: bad.append(k)
            if k.endswith("_nrmse_db") and v > GATES["nrmse_db"]: bad.append(k)
            if k == "recovery_db" and v > GATES["recovery_db"]: bad.append(k)
            if k.endswith("_v") and v > GATES["supply_v"]: bad.append(k)
        row["fail"] = bad; fail += bool(bad); rows.append(row)
        print(name, " ".join(f"{k}={v:.2f}" for k, v in row.items() if isinstance(v, float)), "FAIL " + ",".join(bad) if bad else "ok", flush=True)
    print(f"{len(rows)} cases, {fail} failing; backend=prototype osf={a.osf} {'monolithic' if a.mono else 'partitioned'}")
    if a.json:
        Path(a.json).write_text(json.dumps(dict(gates=GATES, rows=rows), indent=1))
    sys.exit(1 if fail else 0)
