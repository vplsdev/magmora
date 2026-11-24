"""Generate SPICE ground-truth data for the amp netlist with ngspice.

    source scripts/env.sh && $BLOCKAMP_PY scripts/gen_reference.py [--out DIR] [--only PATTERN] [--jobs N]

Writes into DIR (default $BLOCKAMP_PRIVATE/reference, a few hundred MB):
  manifest.json            every case: settings, signal, sample rate, probes, file names
  dc_op.json               operating point from ngspice and from the prototype solver
  ac/<setting>.csv         small-signal responses (ngspice .ac), dB and degrees per probe
  tran/<case>.in.f64       input samples (float64 LE) at FS_REF
  tran/<case>.<probe>.f32  ngspice transient resampled by ngspice onto the FS_REF grid (float32 LE)
  stage/<cluster>_<case>.* the same for each real-time cluster driven alone by ideal sources
The small golden subset used by unit tests is copied to reference/ in the repo by --golden.
"""
import argparse, fnmatch, json, os, shutil, sys, time
from multiprocessing import Pool
from pathlib import Path
import numpy as np
from blockamp import amp, dk, ngspice, signals, partition

FS_REF = 192000
PROBES = ["p1a", "g1b", "p1b", "p2a", "p2b", "p5b", "nX", "nTW", "k3b", "p3a", "ga", "pa", "spk", "b", "sc", "v3", "v2"]
SETTINGS = {
    "lead_default": dict(channel="lead"),
    "lead_hot": dict(channel="lead", pre_lead=8, post_lead=7, low=7, mid=3, high=6, presence=7, resonance=7),
    "lead_low": dict(channel="lead", pre_lead=2, post_lead=3),
    "rhythm_clean": dict(channel="rhythm", pre_rhythm=4, post_rhythm=6),
    "rhythm_bright": dict(channel="rhythm", bright=True, pre_rhythm=4, post_rhythm=6),
    "crunch": dict(channel="rhythm", crunch=True, pre_rhythm=7, post_rhythm=5),
    "crunch_bright": dict(channel="rhythm", crunch=True, bright=True, pre_rhythm=7, post_rhythm=5),
    "normal_input": dict(channel="lead", input="normal"),
}
for knob in ("low", "mid", "high", "presence", "resonance", "pre_lead", "post_lead"):
    for val in (0, 10):
        SETTINGS[f"lead_{knob}_{val}"] = dict(channel="lead", **{knob: val})


def tran_cases():
    c = {}
    for f0 in (110, 440, 2000):
        for a in (1e-4, 1e-3, 1e-2, 0.1, 0.5):
            c[f"sine_lead_{f0}Hz_{a:g}V"] = dict(setting="lead_default", sig=("sine", dict(f0=f0, amp=a, dur=0.06)))
    for s in ("rhythm_clean", "crunch", "crunch_bright", "lead_hot"):
        for a in (0.01, 0.1, 0.5):
            c[f"sine_{s}_440Hz_{a:g}V"] = dict(setting=s, sig=("sine", dict(f0=440, amp=a, dur=0.06)))
    c["burst_lead"] = dict(setting="lead_default", sig=("burst", {}))
    c["burst_crunch"] = dict(setting="crunch", sig=("burst", {}))
    c["burst_lead_hot"] = dict(setting="lead_hot", sig=("burst", {}))
    c["chord_lead_hot"] = dict(setting="lead_hot", sig=("chord", {}))
    c["chug_lead"] = dict(setting="lead_default", sig=("chug", {}))
    c["chug_lead_hot"] = dict(setting="lead_hot", sig=("chug", {}))
    return c


def make_signal(sig):
    kind, kw = sig
    return getattr(signals, kind)(FS_REF, **kw)


def run_tran(args):
    name, case, out = args
    t0 = time.time()
    c = amp.build(SETTINGS[case["setting"]])
    x = make_signal(case["sig"]); t = np.arange(len(x)) / FS_REF
    res, _ = ngspice.tran(c, (t, x), FS_REF, [f"v({p})" for p in PROBES], tmax_step=1 / FS_REF / 8)
    d = Path(out) / "tran"; d.mkdir(parents=True, exist_ok=True)
    x.astype("<f8").tofile(d / f"{name}.in.f64")
    for p in PROBES:
        res[f"v({p})"].astype("<f4").tofile(d / f"{name}.{p}.f32")
    return name, len(x), time.time() - t0


def stage_cases():
    """Each real-time cluster alone: its input source swings around the DC value at three amplitudes."""
    swing = {"A": (0.001, 0.03, 0.5), "B": (0.5, 10.0, 60.0), "C": (5.0, 50.0, 110.0), "D": (5.0, 50.0, 110.0),
             "T": (5.0, 50.0, 90.0), "P": (0.5, 5.0, 40.0)}
    return {f"{cl}_{k}": dict(cluster=cl, amp=a) for cl, amps in swing.items() for k, a in enumerate(amps)}


def run_stage(args):
    name, case, out = args
    t0 = time.time()
    ps = partition.PartSim(SETTINGS["lead_default"], fs=FS_REF, companion=False)          # sets DC values of the cut sources
    cl = next(c for c in ps.cl if c["name"] == case["cluster"])
    c = cl["circuit"]
    src = c.inputs[0] if case["cluster"] == "A" else "cut_in"
    dc = next(e.value for e in c.elements if e.name == src)
    x = dc + signals.sine(FS_REF, 440.0, case["amp"], 0.05)
    t = np.arange(len(x)) / FS_REF
    probes = [f"v({o[1]})" for o in c.outputs if o[1] != "#i"]
    res, _ = ngspice.tran(c, (t, x), FS_REF, probes, tmax_step=1 / FS_REF / 8, input_name=src)
    d = Path(out) / "stage"; d.mkdir(parents=True, exist_ok=True)
    x.astype("<f8").tofile(d / f"{name}.in.f64")
    for o in c.outputs:
        if o[1] != "#i":
            res[f"v({o[1]})"].astype("<f4").tofile(d / f"{name}.{o[0]}.f32")
    return name, len(x), time.time() - t0


def run_ac(args):
    name, out = args
    c = amp.build(SETTINGS[name])
    probes = [p for p in PROBES if p not in ("b", "sc", "v3", "v2")]
    f, H = ngspice.ac(c, probes, 10.0, 40e3, 40)
    d = Path(out) / "ac"; d.mkdir(parents=True, exist_ok=True)
    cols = [f] + [v for p in probes for v in (20 * np.log10(np.abs(H[p]) + 1e-30), np.degrees(np.angle(H[p])))]
    hdr = "freq_hz," + ",".join(f"{p}_db,{p}_deg" for p in probes)
    np.savetxt(d / f"{name}.csv", np.column_stack(cols), delimiter=",", header=hdr, comments="", fmt="%.6g")
    return name


def dc_op(out):
    c = amp.build(SETTINGS["lead_default"])
    S = dk.build(c, FS_REF); x, v = dk.dc_op(S)
    nodes = [n for n in S.nodes if not n.startswith(("T2", "V"))]
    sp = ngspice.op(c, nodes)
    i = np.zeros(len(v)); J = np.zeros((len(v), len(v))); dk.eval_devices(v, S.dev_type, S.dev_port, S.dev_par, i, J, np.zeros(12))
    cur = {}
    for n, p, t in zip(S.nl_names, S.dev_port, S.dev_type):
        cur[n] = [float(a) for a in i[p:p + (2, 3, 1)[t]]]
    d = dict(note="volts; currents in amperes as [plate, (screen,) grid]; 6L6 devices are two tubes in parallel",
             ngspice={n: sp[n] for n in nodes}, prototype={n: float(x[S.idx[n]]) for n in nodes}, device_currents=cur,
             max_abs_diff_v=float(max(abs(sp[n] - x[S.idx[n]]) for n in nodes)))
    Path(out).mkdir(parents=True, exist_ok=True)
    (Path(out) / "dc_op.json").write_text(json.dumps(d, indent=1))
    return d


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.environ.get("BLOCKAMP_PRIVATE", "."), "reference"))
    ap.add_argument("--only", default="*"); ap.add_argument("--jobs", type=int, default=10)
    ap.add_argument("--golden", action="store_true", help="copy the small golden subset into ./reference")
    ap.add_argument("--values", default="prototype", help="value set of dsp/src/circuit/design/value_sets.json: "
                    "'prototype' is amp.PHYS as it stands; any other set applies its overrides and uses ./reference/<set>")
    a = ap.parse_args()
    out = Path(a.out)
    if a.values != "prototype":
        sets = json.loads((Path(__file__).resolve().parent.parent / "dsp/src/circuit/design/value_sets.json").read_text())["sets"]
        amp.PHYS.update(next(s for s in sets if s["name"] == a.values)["phys"])
    if a.golden:
        g = Path(__file__).resolve().parent.parent / "reference"
        if a.values != "prototype":
            g = g / a.values
        g.mkdir(parents=True, exist_ok=True)
        d = dc_op(g); print("dc_op: max |ngspice - prototype| =", d["max_abs_diff_v"], "V")      # recomputed, not copied
        (g / "ac").mkdir(parents=True, exist_ok=True); (g / "tran").mkdir(exist_ok=True); (g / "stage").mkdir(exist_ok=True)
        shutil.copy(out / "manifest.json", g)
        for f in (out / "ac").glob("*.csv"): shutil.copy(f, g / "ac")
        keep = ["sine_lead_440Hz_0.001V", "sine_lead_440Hz_0.1V", "sine_lead_110Hz_0.5V", "sine_crunch_440Hz_0.1V", "burst_lead"]
        for k in keep:
            for p in ("in.f64", "spk.f32", "nX.f32", "p3a.f32"):
                shutil.copy(out / "tran" / f"{k}.{p}", g / "tran")
        for f in (out / "stage").glob("*"): shutil.copy(f, g / "stage")
        print("golden subset copied to", g); sys.exit(0)
    d = dc_op(out); print("dc_op: max |ngspice - prototype| =", d["max_abs_diff_v"], "V")
    tc = {k: v for k, v in tran_cases().items() if fnmatch.fnmatch(k, a.only)}
    sc = {k: v for k, v in stage_cases().items() if fnmatch.fnmatch(k, a.only)}
    acs = [k for k in SETTINGS if fnmatch.fnmatch(k, a.only)]
    man = dict(fs=FS_REF, probes=PROBES, settings=SETTINGS, tran=tran_cases(), stage=stage_cases(),
               ngspice="reltol=1e-5, trapezoidal, max step 1/(8*fs), output linearised onto the fs grid",
               signals="see proto/blockamp/signals.py; inputs are also stored as .in.f64")
    (out / "manifest.json").write_text(json.dumps(man, indent=1))
    with Pool(a.jobs) as pool:
        for n in pool.imap_unordered(run_ac, [(k, str(out)) for k in acs]): print("ac", n, flush=True)
        for n, ln, el in pool.imap_unordered(run_stage, [(k, v, str(out)) for k, v in sc.items()]): print(f"stage {n}: {ln} samples {el:.0f}s", flush=True)
        for n, ln, el in pool.imap_unordered(run_tran, [(k, v, str(out)) for k, v in tc.items()]): print(f"tran {n}: {ln} samples {el:.0f}s", flush=True)
