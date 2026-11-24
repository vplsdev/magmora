"""Export the partitioned prototype (matrices, device tables, wiring, initial state) to a flat binary
file that bench/kernel.cpp loads. Used to measure what the per-sample kernel costs in C++.

usage: export_clusters.py out.bin [fs] [ctl-json]
"""
import json, struct, sys
import numpy as np
from blockamp.partition import PartSim, CLUSTERS_SPLIT

out = sys.argv[1]
fs = float(sys.argv[2]) if len(sys.argv) > 2 else 192000.0
ctl = json.loads(sys.argv[3]) if len(sys.argv) > 3 else None
ps = PartSim(ctl, fs=fs, clusters=CLUSTERS_SPLIT)
with open(out, "wb") as f:
    w_i = lambda *a: f.write(struct.pack(f"<{len(a)}q", *a))
    w_a = lambda a: f.write(np.ascontiguousarray(a, dtype="<f8").tobytes())
    nc = len(ps.cl)
    w_i(nc, ps.out_index[("P", "spk")][0], ps.out_index[("P", "spk")][1])
    for c in range(nc):
        L = ps.L
        npo, ns, nu, ny = L["K"][c].shape[0], L["s"][c].shape[0], L["vdc"][c].shape[0], L["Yv"][c].shape[0]
        nd, nin = len(ps.dev_type[c]), len(ps.in_idx[c])
        w_i(npo, ns, nu, ny, nd, nin, int(ps.extrap[c]))
        for k in ("Pv", "Ps", "K", "Sv", "Ss", "Si", "Yv", "Ys", "Yi", "s", "v", "vdc", "ylast"):
            w_a(L[k][c])
        w_i(*ps.dev_type[c]); w_i(*ps.dev_port[c]); w_a(ps.dev_par[c])
        w_i(*ps.in_idx[c]); w_i(*ps.in_src[c].ravel()); w_a(ps.in_scale[c])
print("exported", nc, "clusters at", fs)
