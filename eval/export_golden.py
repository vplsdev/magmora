"""Write golden data for the C++ port (WP1): cluster matrices, DC points and device-law samples.

    source scripts/env.sh && $BLOCKAMP_PY eval/export_golden.py [--out reference/golden]

For each (setting, internal rate) the file <setting>_<rate>.json holds, per cluster: element list,
node order, source order (u), state order (s), nonlinear port order, the matrices Pv Ps K Sv Ss Si Yv Ys Yi
(row-major), the DC solution and initial states, and the wiring between clusters. devices.json holds
function values and Jacobians of the tube laws on a grid. The C++ unit tests must reproduce the matrices to
1e-9 relative (Frobenius) and the device values to 1e-12 relative.
"""
import argparse, json
from pathlib import Path
import numpy as np
from blockamp import devices, models, partition

SETTINGS = {"lead_default": dict(channel="lead"),
            "crunch_bright": dict(channel="rhythm", crunch=True, bright=True, pre_rhythm=7, post_rhythm=5),
            "lead_extreme": dict(channel="lead", pre_lead=10, post_lead=10, low=0, mid=10, high=10, presence=10, resonance=0)}
RATES = (96000, 192000)

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default=None)
    ap.add_argument("--values", default="prototype", help="value set of value_sets.json; other than 'prototype' goes to reference/<set>/golden")
    a = ap.parse_args()
    if a.values != "prototype":
        from blockamp import amp
        sets = json.loads((Path(__file__).resolve().parent.parent / "dsp/src/circuit/design/value_sets.json").read_text())["sets"]
        amp.PHYS.update(next(s for s in sets if s["name"] == a.values)["phys"])
    out = Path(a.out or ("reference/golden" if a.values == "prototype" else f"reference/{a.values}/golden")); out.mkdir(parents=True, exist_ok=True)
    for name, ctl in SETTINGS.items():
        for fs in RATES:
            ps = partition.PartSim(ctl, fs=fs, clusters=partition.CLUSTERS_SPLIT)
            doc = dict(setting=ctl, fs=fs, clusters=[])
            for ci, c in enumerate(ps.cl):
                S = c["S"]
                doc["clusters"].append(dict(
                    name=c["name"],
                    elements=[dict(kind=e.kind, name=e.name, nodes=list(e.nodes), value=(e.value if not isinstance(e.value, dict) else None))
                              for e in c["circuit"].elements],
                    nodes=S.nodes, sources=S.vsrc_names, source_dc=S.vdc.tolist(), inputs=c["circuit"].inputs,
                    states=S.state_names, devices=S.nl_names, device_type=S.dev_type.tolist(), device_port=S.dev_port.tolist(),
                    outputs=S.out_names, wiring_src=ps.in_src[ci].tolist(), wiring_scale=ps.in_scale[ci].tolist(),
                    **{k: np.asarray(getattr(S, k)).tolist() for k in ("Pv", "Ps", "K", "Sv", "Ss", "Si", "Yv", "Ys", "Yi")},
                    v0=c["v0"].tolist(), s0=ps.L["s"][ci].tolist(), y0=ps.L["ylast"][ci].tolist()))
            (out / f"{name}_{fs}.json").write_text(json.dumps(doc))
            print("wrote", name, fs)
    o = np.zeros(12); rows = dict(triode=[], pentode=[])
    T = np.array(models.triode_vec(models.T12AX7)); P = np.array(models.pentode_vec(models.P6L6GC, 2))
    for vp in (-5.0, 0.0, 1.0, 20.0, 150.0, 300.0, 450.0):
        for vg in (-60.0, -8.0, -3.0, -1.5, -0.5, 0.0, 0.3, 1.0, 5.0):
            devices.triode(vp, vg, T, o); rows["triode"].append([vp, vg] + o[:6].tolist())
    for vp in (-20.0, -0.5, 0.0, 0.5, 5.0, 30.0, 100.0, 475.0, 900.0):
        for vs in (300.0, 470.0):
            for vg in (-120.0, -60.0, -30.0, -5.0, 0.0, 2.0, 10.0):
                devices.pentode(vp, vs, vg, P, o); rows["pentode"].append([vp, vs, vg] + o[:12].tolist())
    (out / "devices.json").write_text(json.dumps(dict(triode_params=T.tolist(), pentode_params=P.tolist(),
        triode_columns="vpk vgk ip ig dip/dvpk dip/dvgk dig/dvpk dig/dvgk",
        pentode_columns="vpk vsk vgk ip is ig then 3x3 Jacobian row-major wrt (vpk vsk vgk)", **rows)))
    print("wrote devices.json")
