"""Run ngspice in batch mode on a generated deck and read back wrdata tables."""
import os, subprocess, tempfile
import numpy as np
from . import spice

NGSPICE = os.environ.get("NGSPICE", "ngspice")
OPTS = ".options reltol=1e-5 method=trap maxord=2 itl4=100\n"


def run_deck(deck, timeout=3600):
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "deck.cir")
        open(f, "w").write(deck)
        r = subprocess.run([NGSPICE, "-b", f], capture_output=True, text=True, cwd=d, timeout=timeout)
        out = {}
        for name in os.listdir(d):
            if name.endswith(".data"):
                out[name[:-5]] = np.loadtxt(os.path.join(d, name))
        return r.stdout + r.stderr, out


def op(c, nodes):
    deck = spice.export(c) + OPTS + ".control\nop\nprint " + " ".join(f"v({n})" for n in nodes) + "\n.endc\n.end\n"
    log, _ = run_deck(deck)
    res = {}
    for line in log.splitlines():
        if line.startswith("v(") and "=" in line:
            k, v = line.split("=")
            res[k.strip()[2:-1]] = float(v)
    if len(res) < len(nodes):
        raise RuntimeError(log[-3000:])
    return {n: res[n.lower()] for n in nodes}      # ngspice reports node names in lower case


def tran(c, pwl, fs_out, probes, tmax_step=None, input_name="in", uic=False):
    """pwl: (t, v) arrays for the input source. Returns dict probe -> samples on a uniform fs_out grid."""
    t, v = pwl
    with tempfile.TemporaryDirectory() as d:
        dur = float(t[-1])
        step = 1.0 / fs_out
        ms = tmax_step or step / 4
        pts = [f"+ {a:.9e} {b:.9e}" for a, b in zip(t, v)]     # inline PWL (no XSPICE filesource needed)
        deck = spice.export(c, {input_name: "pwl(\n" + "\n".join(pts) + "\n+ )"}) + OPTS
        deck += f".control\ntran {step:.9e} {dur:.9e} 0 {ms:.9e}\nlinearize\nwrdata {d}/o.data " + " ".join(probes) + "\n.endc\n.end\n"
        f = os.path.join(d, "deck.cir"); open(f, "w").write(deck)
        r = subprocess.run([NGSPICE, "-b", f], capture_output=True, text=True, cwd=d)
        if not os.path.exists(os.path.join(d, "o.data")):
            raise RuntimeError((r.stdout + r.stderr)[-3000:])
        a = np.loadtxt(os.path.join(d, "o.data"))
    return {p: a[:, 2 * i + 1] for i, p in enumerate(probes)}, a[:, 0]


def ac(c, probes, f_lo=10.0, f_hi=30e3, per_dec=30, input_name="in"):
    """Small-signal .ac sweep. Returns (freqs, {probe: complex response})."""
    with tempfile.TemporaryDirectory() as d:
        deck = spice.export(c, {input_name: "dc 0 ac 1"}) + OPTS
        deck += f".control\nac dec {per_dec} {f_lo} {f_hi}\nwrdata {d}/o.data " + " ".join(f"v({p})" for p in probes) + "\n.endc\n.end\n"
        f = os.path.join(d, "deck.cir"); open(f, "w").write(deck)
        r = subprocess.run([NGSPICE, "-b", f], capture_output=True, text=True, cwd=d)
        if not os.path.exists(os.path.join(d, "o.data")):
            raise RuntimeError((r.stdout + r.stderr)[-3000:])
        a = np.loadtxt(os.path.join(d, "o.data"))
    return a[:, 0], {p: a[:, 3 * i + 1] + 1j * a[:, 3 * i + 2] for i, p in enumerate(probes)}
