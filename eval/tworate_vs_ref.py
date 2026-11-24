"""Accuracy of the two-rate recipe, independent of the stored ngspice data: the cases and metrics of gate G1
(eval/check_spice.py) evaluated against the single-rate prototype at 384 kHz (which is itself the solver that
passes G1 at High). Printed side by side: today's Standard (everything at 192 kHz) and the two-rate recipe
(clusters up to --cut at 384 kHz, the rest at 192 kHz).

    source scripts/env.sh && $BLOCKAMP_PY eval/tworate_vs_ref.py [--cut D] [--only PATTERN] [--json OUT]
"""
import argparse, fnmatch, json, os
from pathlib import Path
import numpy as np
from blockamp import signals, partition, multirate
import check_spice as cs

FS = 192000
PROBES = ["nX", "p3a"]


def run_single(setting, kind, kw, fs):
    ps = partition.PartSim(setting, fs=fs, probes=PROBES, clusters=partition.CLUSTERS_SPLIT)
    outs = [("D", "probe_nX"), ("T", "probe_p3a"), ("P", "spk")]
    return ps.run(getattr(signals, kind)(fs, **kw), outs)[::fs // FS]


def run_two(setting, kind, kw, cut, **opt):
    t = multirate.TwoRateSim(setting, fs_base=FS, cut_after=cut, probes=PROBES, **opt)
    outs = [("D", "probe_nX"), ("T", "probe_p3a"), ("P", "spk")]
    R = t.ratio
    x = getattr(signals, kind)(R * FS, **kw)
    d = int(t.delay_base)
    y = t.run(np.concatenate([x, np.full(R * d + R, x[-1])]), outs)
    n = len(x) // R
    out = np.empty((n, 3))
    for k, o in enumerate(outs):
        out[:, k] = y[d:d + n, k] if t.out_index[o][0] >= t.nf else y[:n, k]
    return out, int(t.fails.sum())


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--cut", default="D"); ap.add_argument("--only", default="*"); ap.add_argument("--json", default=None)
    ap.add_argument("--ntaps", type=int, default=129); ap.add_argument("--fstop", type=float, default=28000.0); ap.add_argument("--no-replica", action="store_true"); ap.add_argument("--corr", type=float, default=1.0)
    ap.add_argument("--ratio", type=int, default=2, help="1: single rate with a band-limiting low-pass at the cut (--lp)")
    ap.add_argument("--lp", type=float, nargs=3, default=None, metavar=("F_PASS", "F_STOP", "NTAPS"))
    ap.add_argument("--manifest", default=str(Path(__file__).resolve().parents[1] / "reference" / "manifest.json"))
    a = ap.parse_args()
    man = json.loads(Path(a.manifest).read_text())
    opt = dict(h=multirate.design_decimator(a.ntaps, fs_fast=2.0 * FS, f_stop=a.fstop, w_stop=3.0), corr=a.corr, ratio=a.ratio)
    if a.ratio == 1:
        opt["h"] = multirate.design_cut_lowpass(FS, a.lp[0], a.lp[1], int(a.lp[2])) if a.lp else np.array([1.0])
    from scipy.signal import firwin, lfilter
    h20 = firwin(255, 20000, fs=FS)
    if a.no_replica: opt["r_rep"] = 0.0
    rows = []; worst = {}
    for name, case in man["tran"].items():
        if not fnmatch.fnmatch(name, a.only): continue
        setting = man["settings"][case["setting"]]; kind, kw = case["sig"]
        ref = run_single(setting, kind, kw, 2 * FS)
        cands = {"single192": (run_single(setting, kind, kw, FS), 0)}
        cands["tworate"] = run_two(setting, kind, kw, a.cut, **opt)
        row = dict(case=name)
        for cn, (y, fails) in cands.items():
            n = min(len(y), len(ref)); tail = slice(n // 2, n)
            for k, p in enumerate(("nX", "p3a", "spk")):
                row[f"{cn}_{p}_nrmse"] = float(cs.align_nrmse(y[tail, k], ref[tail, k]))
                if p == "spk":      # the same after both are limited to the audio band
                    row[f"{cn}_spk_nrmse20k"] = float(cs.align_nrmse(lfilter(h20, 1, y[:n, k])[tail], lfilter(h20, 1, ref[:n, k])[tail]))
                if kind == "sine":
                    row[f"{cn}_{p}_h"] = cs.harm_err(y[tail, k], ref[tail, k], FS, kw["f0"])[0]
            row[f"{cn}_fails"] = fails
        rows.append(row)
        for k, v in row.items():
            if isinstance(v, float): worst[k] = max(worst.get(k, -999), v)
        print(name, " ".join(f"{k}={v:.2f}" for k, v in row.items() if isinstance(v, float) and "spk" in k or k.endswith("fails") and v), flush=True)
    print("WORST over cases (h: dB harmonic error, nrmse: dB re signal; reference = single-rate 384 kHz):")
    for k in sorted(worst): print(f"  {k:28s} {worst[k]:8.2f}")
    if a.json:
        Path(a.json).write_text(json.dumps(dict(rows=rows, worst=worst), indent=1))
