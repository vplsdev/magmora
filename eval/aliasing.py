"""Gate G4: aliasing of the whole chain at the host rate.

    source scripts/env.sh && $BLOCKAMP_PY eval/aliasing.py [--osf 2 4 8] [--json OUT]

A pure sine (frequencies chosen so no harmonic lands on another's alias) is rendered at 48 kHz; every
spectral peak that is not at a harmonic of the input is aliasing (or solver noise). Reported: the strongest
such component below 20 kHz, in dB relative to the strongest harmonic.
Also reported: the summed non-harmonic power re the summed harmonic power (below 20 kHz), crest factor and THD.
--two-rate D measures the two-rate recipe (clusters A to D at twice the rate of the rest, blockamp.multirate).
Backend: the Python prototype with scipy's polyphase resampler standing in for the product's half-band
FIRs (here a 96-taps-per-phase Kaiser-14 design, stop band beyond 120 dB, so that the number measures the
circuit model and not the filter; with --cli the real engine is measured).
"""
import argparse, json, sys
from pathlib import Path
import numpy as np
from blockamp.render import render

SR = 48000
FREQS = [1244.5, 2637.0, 4186.0, 6271.9, 9956.1]
LEVELS = [0.01, 0.1, 0.5]
CTL = dict(channel="lead", pre_lead=7, post_lead=7, low=7, mid=7, high=7, presence=7, resonance=7)


def measure(y, f0):
    n = len(y); w = np.blackman(n)
    Y = np.abs(np.fft.rfft(y * w)) / (w.sum() / 2)
    f = np.fft.rfftfreq(n, 1 / SR)
    mask = np.ones(len(f), bool)
    mask[f < 30] = False
    k = 1
    while k * f0 < SR / 2:
        mask[np.abs(f - k * f0) < 12 * SR / n] = False     # main lobe of each harmonic
        k += 1
    harm = Y[~mask & (f >= 30)].max()
    alias = Y[mask & (f < 20000)].max()
    return 20 * np.log10(alias / harm + 1e-15), float(f[mask & (f < 20000)][np.argmax(Y[mask & (f < 20000)])])


def measure2(y, f0, sr=SR):
    """Both metrics plus saturation descriptors. alias_db: strongest non-harmonic bin below 20 kHz re the strongest
    harmonic. alias_sum_db: summed non-harmonic power re summed harmonic power, both below 20 kHz (more robust).
    crest_db and thd_db (harmonics 2.. re fundamental, below 20 kHz) describe how hard the chain is clipping, so that
    simulators can be compared at an equal degree of saturation and not only at an equal input level."""
    n = len(y); w = np.blackman(n)
    Y = np.abs(np.fft.rfft((y - np.mean(y)) * w)) / (w.sum() / 2)
    f = np.fft.rfftfreq(n, 1 / sr)
    lobe = 12 * sr / n
    harm = np.zeros(len(f), bool); k = 1
    while k * f0 < sr / 2:
        harm |= np.abs(f - k * f0) < lobe; k += 1
    band = (f >= 30) & (f < 20000)
    non = band & ~harm
    hp = Y[harm & band]; npk = Y[non]
    fund = np.sum(Y[np.abs(f - f0) < lobe] ** 2)
    ph = np.sum(hp ** 2)
    return dict(alias_db=float(20 * np.log10(npk.max() / hp.max() + 1e-15)), at_hz=float(f[non][np.argmax(npk)]),
                alias_sum_db=float(10 * np.log10(np.sum(npk ** 2) / ph + 1e-30)),
                thd_db=float(10 * np.log10(max(ph - fund, 1e-30) / fund)),
                crest_db=float(20 * np.log10(np.max(np.abs(y)) / np.sqrt(np.mean(y ** 2)))))


def _fir(osf):
    from scipy.signal import firwin
    return firwin(2 * 48 * osf + 1, 1.0 / osf, window=("kaiser", 14.0))


def render_two_rate(x, ctl, osf=4, cut="D", ntaps=129, f_stop=28000.0, settle=0.05):
    """Two-rate recipe (blockamp.multirate): clusters up to `cut` at 2*osf*SR, the rest at osf*SR."""
    from scipy.signal import resample_poly
    from blockamp import multirate
    pad = int(settle * SR)
    xx = np.concatenate([np.zeros(pad), x, np.zeros(ntaps)])
    sim = multirate.TwoRateSim(ctl, fs_base=SR * osf, cut_after=cut, h=multirate.design_decimator(ntaps, fs_fast=2.0 * SR * osf, f_stop=f_stop, w_stop=3.0))
    y = sim.run(resample_poly(xx, 2 * osf, 1, window=_fir(2 * osf)))[:, 0]
    d = int(sim.delay_base)
    y = resample_poly(y[d:], 1, osf, window=_fir(osf))
    return y[pad:pad + len(x)], int(sim.fails.sum())


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--osf", type=int, nargs="+", default=[2, 4, 8]); ap.add_argument("--json", default=None)
    ap.add_argument("--two-rate", default=None, metavar="CUT", help="clusters up to CUT at twice the rate given by --osf")
    ap.add_argument("--ntaps", type=int, default=129); ap.add_argument("--fstop", type=float, default=28000.0); ap.add_argument("--levels", type=float, nargs="+", default=LEVELS)
    a = ap.parse_args()
    rows = []
    for osf in a.osf:
        worst = -999; worst_sum = -999
        for f0 in FREQS:
            for lv in a.levels:
                t = np.arange(int(1.2 * SR)) / SR
                x = lv * np.sin(2 * np.pi * f0 * t)
                if a.two_rate:
                    y, fails = render_two_rate(x, CTL, osf, a.two_rate, a.ntaps, a.fstop)
                else:
                    y = render(x, CTL, in_gain_db=0.0, osf=osf, window=("kaiser", 14.0), taps_per_phase=48); fails = 0
                m = measure2(y[int(0.2 * SR):], f0)
                m.update(osf=osf, f0=f0, level=lv, two_rate=a.two_rate, fails=fails); rows.append(m)
                worst = max(worst, m["alias_db"]); worst_sum = max(worst_sum, m["alias_sum_db"])
                print(f"osf {osf}{'+2x to ' + a.two_rate if a.two_rate else ''} f0 {f0:7.1f} Hz level {lv:5.3f} V: peak {m['alias_db']:7.1f} dB at {m['at_hz']:7.0f} Hz"
                      f"  sum {m['alias_sum_db']:7.1f} dB  crest {m['crest_db']:.2f} thd {m['thd_db']:.1f}" + (f"  FALLBACKS {fails}" if fails else ""), flush=True)
        print(f"== osf {osf}: worst peak {worst:.1f} dB, worst sum {worst_sum:.1f} dB")
    if a.json:
        Path(a.json).write_text(json.dumps(rows, indent=1))
