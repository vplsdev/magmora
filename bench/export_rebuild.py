"""Export what a knob move has to recompute, for bench/rebuild_bench.cpp: per cluster the nodal matrix G, the
source / state / device incidence matrices and the selectors, plus the matrices the prototype derives from them
(for checking). usage: export_rebuild.py out.bin [fs] [ctl-json]"""
import json, struct, sys
import numpy as np
from blockamp.partition import PartSim, CLUSTERS_SPLIT

out = sys.argv[1]; fs = float(sys.argv[2]) if len(sys.argv) > 2 else 192000.0
ctl = json.loads(sys.argv[3]) if len(sys.argv) > 3 else None
ps = PartSim(ctl, fs=fs, clusters=CLUSTERS_SPLIT)
with open(out, "wb") as f:
    w_i = lambda *a: f.write(struct.pack(f"<{len(a)}q", *a))
    w_a = lambda a: f.write(np.ascontiguousarray(a, dtype="<f8").tobytes())
    w_i(len(ps.cl))
    for c in ps.cl:
        S = c["S"]; nx = S.nx; B = np.hstack([S.Bv, S.Bs, S.Bi]); N = np.vstack([S.Nv, 2.0 * S.gs[:, None] * S.Ns, S.No])
        ref = N @ np.linalg.solve(S.G, B)
        w_i(nx, B.shape[1], N.shape[0], S.Bv.shape[1], S.Bs.shape[1], S.Bi.shape[1], S.Nv.shape[0], S.Ns.shape[0], S.No.shape[0])
        w_a(S.G); w_a(B); w_a(N); w_a(ref)
        print(f"  cluster {c['name']}: {nx} unknowns, {B.shape[1]} right-hand sides, {N.shape[0]} selector rows")
