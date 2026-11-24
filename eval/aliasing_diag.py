"""Where does the aliasing come from? Non-harmonic content at each cluster output, measured at the INTERNAL rate
(no decimation involved), so anything that is not a harmonic of the input was folded by the solver itself.

    source scripts/env.sh && $BLOCKAMP_PY eval/aliasing_diag.py [--osf 4]
"""
import argparse, json
import numpy as np
from scipy.signal import resample_poly, firwin
from blockamp.partition import PartSim, CLUSTERS_SPLIT
from aliasing import FREQS, LEVELS, CTL, measure2, SR

OUTS = (("A", "v_out"), ("B", "v_out"), ("C", "v_out"), ("D", "v_out"), ("T", "v_out"), ("P", "spk"))

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--osf", type=int, default=4); ap.add_argument("--json", default=None)
    a = ap.parse_args(); osf = a.osf; fs = SR * osf
    h = firwin(2 * 48 * osf + 1, 1.0 / osf, window=("kaiser", 14.0))
    rows = []
    for f0 in FREQS:
        for lv in LEVELS:
            t = np.arange(int(1.25 * SR)) / SR
            up = resample_poly(lv * np.sin(2 * np.pi * f0 * t), osf, 1, window=h)
            sim = PartSim(CTL, fs=fs, clusters=CLUSTERS_SPLIT)
            y = sim.run(up, outs=OUTS)[int(0.25 * fs):]
            r = dict(f0=f0, level=lv)
            for k, (c, _) in enumerate(OUTS):
                m = measure2(y[:, k], f0, sr=fs)          # at the internal rate: band 30 Hz .. 20 kHz
                r[c] = (round(m["alias_db"], 1), round(m["alias_sum_db"], 1))
            rows.append(r)
            print(f"f0 {f0:7.1f} lv {lv:4.2f} " + "  ".join(f"{c}:{r[c][0]:6.1f}/{r[c][1]:6.1f}" for c, _ in OUTS), flush=True)
    if a.json:
        open(a.json, "w").write(json.dumps(rows, indent=1))
