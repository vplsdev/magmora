"""Reproduce the kernel CPU table: the reference kernel (bench/kernel.cpp, the prototype's algorithm) against the
fast kernel (bench/kernel_fast.cpp), on three cases, with every fast variant validated against a converged reference.

    source scripts/env.sh && $BLOCKAMP_PY bench/run_bench.py [--levers] [--rebuild] [--rounds 5] [--fit fit.json]

typical : the DI named in eval/local_assets.json (busiest 3 s) at the settings fitted to the main capture (needs the private assets)
hot     : the same DI, pre 8, post 7 (output stage clipping)
torture : full-scale white noise into the amp with every knob at 10
Internal rate 192 kHz (Standard quality at a 48 kHz host rate). Kernel only: no resampling, no parameter updates.

Columns: best-of-N wall time per internal sample (the machine is rarely idle: variants are run interleaved and the
minimum is kept; the load average is printed), share of one core, instructions per sample (from /usr/bin/time -l,
independent of load), error against the reference (closed-form devices, tolerance 1e-6, step-controlled
continuation so that every sample converges), and the number of samples whose Newton solve did not converge.

--levers  also builds one variant per design lever (off) and the rejected one-sample screen delay
--rebuild also measures the cost of rebuilding the cluster matrices for a knob move (bench/rebuild_bench.cpp)
Everything is written under $BLOCKAMP_PRIVATE/build/wp9-bench/run.
"""
import argparse, json, os, re, subprocess, sys
import numpy as np
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "eval"))
import common as C

# Settings used as "typical" until a fit file is given (WP9, model v2).
TYPICAL = dict(channel="lead", pre_lead=7.8, post_lead=5.0, low=4.3, mid=1.3, high=5.9, resonance=5.2, presence=1.6)
TYPICAL_GAIN_DB = 8.0
FS, SECONDS = 192000.0, 3.0

ap = argparse.ArgumentParser()
ap.add_argument("--levers", action="store_true"); ap.add_argument("--rebuild", action="store_true")
ap.add_argument("--rounds", type=int, default=5); ap.add_argument("--fit", default=None)
a = ap.parse_args()
B = C.PRIVATE / "build" / "wp9-bench" / "run"; B.mkdir(parents=True, exist_ok=True)
PY = sys.executable
CXX = ["clang++", "-O3", "-std=c++17", "-Wno-array-bounds"]


def sh(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, capture_output=True, text=True, **kw)


# ---- inputs and exports ---------------------------------------------------------------------------------------
ctl, gain = TYPICAL, TYPICAL_GAIN_DB
if a.fit:
    fit = json.loads(open(a.fit).read()); ctl, gain = fit["ctl"], fit["in_gain_db"]
di = C.read(C.asset("di")); s0 = C.busiest_window(di, SECONDS)
xd = resample_poly(di[s0:s0 + int(SECONDS * C.SR)] * 10 ** (gain / 20), int(FS // C.SR), 1)
cases = {"typical": (ctl, xd), "hot": (dict(ctl, pre_lead=8.0, post_lead=7.0), xd),
         "torture": (dict(channel="lead", pre_lead=10, post_lead=10, low=10, mid=10, high=10, presence=10, resonance=10),
                     np.random.default_rng(1).uniform(-1, 1, int(SECONDS * FS)) * 2.0)}
N = len(xd)
exports = {"dense": ["--no-fold", "--no-minimal"], "std": [], "modal": ["--modal"]}
for k, (c, x) in cases.items():
    x.astype("<f8").tofile(B / f"in_{k}.f64")
    sh([PY, os.path.join(HERE, "export_clusters.py"), B / f"ref_{k}.bin", FS, json.dumps(c)])
    for e, fl in exports.items():
        (B / e).mkdir(exist_ok=True)
        sh([PY, os.path.join(HERE, "export_fast.py"), B / e / f"{k}.bin", B / e / "cluster_sizes.h", FS, json.dumps(c)] + fl)

# ---- binaries -------------------------------------------------------------------------------------------------
ref_src, fast_src = os.path.join(HERE, "kernel.cpp"), os.path.join(HERE, "kernel_fast.cpp")
sh(CXX + ["-DADAPTIVE_FALLBACK", ref_src, "-o", B / "k_truth"])          # converged reference for validation
sh(CXX + ["-DUSE_TABLES", ref_src, "-o", B / "k_before"])                # what D6 measured (prototype algorithm, tables)
variants = [("fast", "std", []), ("fast + modal state update", "modal", ["-DMODAL"])]
if a.levers:
    variants += [("fast, generic layout: no state reduction, no diode fold", "dense", []),
                 ("fast, all rows in every step (no row sets)", "std", ["-DNO_ACTIVE_SET"]),
                 ("fast, second factorisation kept (no early accept)", "std", ["-DNO_FAST_ACCEPT"]),
                 ("fast, closed-form devices", "std", ["-DCLOSED_FORM"]),
                 ("fast, prototype fallbacks", "std", ["-DREF_FALLBACK"]),
                 ("REJECTED: screen voltage delayed one sample", "std", ["-DDELAY_SCREEN"])]
for i, (name, e, fl) in enumerate(variants):
    sh(CXX + [f"-I{B / e}"] + fl + [fast_src, "-o", B / f"k_fast{i}"])


def run(cmd):
    r = subprocess.run(["/usr/bin/time", "-l"] + [str(c) for c in cmd], capture_output=True, text=True)
    ns = float(r.stdout.split("ns/sample")[0].split()[-1])
    ins = int(re.search(r"(\d+)\s+instructions retired", r.stderr).group(1))
    fails = sum(int(m) for m in re.findall(r"fails (\d+)", r.stdout))
    return ns, ins, fails


def db(y, ref):
    d = y - ref
    return 20 * np.log10(d.std() / ref.std() + 1e-300), np.abs(d).max()


print(f"internal rate {FS/1000:.0f} kHz, {N} samples per case, best of {a.rounds} interleaved rounds, load average at start {os.getloadavg()[0]:.1f}")
for k in cases:
    run([B / "k_truth", B / f"ref_{k}.bin", B / f"in_{k}.f64", B / f"truth_{k}.f64", "1e-6"])
    ref = np.fromfile(B / f"truth_{k}.f64")
    rows = [("reference kernel, tables, tol 1e-4 (before)", [B / "k_before", B / f"ref_{k}.bin", B / f"in_{k}.f64", B / "o_before.f64", "1e-4"], B / "o_before.f64", 1)]
    for i, (name, e, fl) in enumerate(variants):
        rows.append((name + ", tol 1e-4", [B / f"k_fast{i}", B / e / f"{k}.bin", B / f"in_{k}.f64", B / f"o_fast{i}.f64", "1e-4", "3"], B / f"o_fast{i}.f64", 3))
    best = {}
    for rnd in range(a.rounds):
        for name, cmd, o, reps in rows:
            ns, ins, fails = run(cmd)
            if name not in best or ns < best[name][0]:
                best[name] = (ns, ins / N / reps, fails)
    print(f"\n[{k}]  output rms {ref.std():.1f} V, peak {np.abs(ref).max():.0f} V   (load average now {os.getloadavg()[0]:.1f})")
    for name, cmd, o, reps in rows:
        ns, ins, fails = best[name]; e_db, e_max = db(np.fromfile(o), ref)
        print(f"  {name:62s} {ns:7.0f} ns  {ns * 1e-9 * FS * 100:5.1f} %  {ins:7.0f} instr  err {e_db:7.1f} dB (max {e_max:.1e} V)  unconverged {fails}")

if a.rebuild:
    sh([PY, os.path.join(HERE, "export_rebuild.py"), B / "rebuild.bin", FS, json.dumps(ctl)])
    sh(CXX + [os.path.join(HERE, "rebuild_bench.cpp"), "-o", B / "rebuild_bench"])
    print("\nmatrix rebuild for a knob move (LU of G + all solves + selector rows), best of 5:")
    print(sh([B / "rebuild_bench", B / "rebuild.bin", "1000"]).stdout.rstrip())
