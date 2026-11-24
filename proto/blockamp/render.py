"""Offline render through the partitioned prototype with polyphase oversampling."""
import numpy as np
from scipy.signal import resample_poly
from .partition import PartSim, CLUSTERS_SPLIT


OUTPUTS = {"spk": ("P", "spk"), "preamp": ("P", "probe_npo"), "send": ("T", "probe_nN")}


def render(x48, ctl=None, phys=None, in_gain_db=0.0, osf=4, fs=48000, settle=0.05, sim=None, return_sim=False,
           window=("kaiser", 5.0), taps_per_phase=10, out="spk"):
    """x48: mono float array at fs. Returns the speaker-terminal voltage at fs (volts); out="preamp" returns the
    voltage at the PREAMP OUT jack instead (the power amplifier still runs: it loads the jack's source node),
    out="send" the effects send. Those two exist for scoring captures that were taken from those jacks."""
    ctl = dict(ctl or {}); out = ctl.pop("out", out)        # a fit file may carry the jack name with the knob settings
    x = np.asarray(x48, dtype=np.float64) * 10 ** (in_gain_db / 20)
    pad = int(settle * fs)
    x = np.concatenate([np.zeros(pad), x])
    from scipy.signal import firwin
    h = firwin(2 * taps_per_phase * osf + 1, 1.0 / osf, window=window) if osf > 1 else None
    up = resample_poly(x, osf, 1, window=h) if osf > 1 else x
    sim = sim or PartSim(ctl, phys, fs=fs * osf, clusters=CLUSTERS_SPLIT, probes=("npo", "nN"))
    y = sim.run(up, outs=(OUTPUTS[out],))[:, 0]
    y = resample_poly(y, 1, osf, window=h) if osf > 1 else y
    y = y[pad:pad + len(x48)]
    return (y, sim) if return_sim else y
