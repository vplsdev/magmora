"""Gate G1 for the two-rate recipe (clusters up to --cut at 2 x the base rate, the rest at the base rate).

    source scripts/env.sh && $BLOCKAMP_PY eval/check_spice_tworate.py --cut D [check_spice.py options, --osf 4]

Runs eval/check_spice.py unchanged with the partitioned backend replaced by blockamp.multirate.TwoRateSim. The test
signals are generated on the reference grid and interpolated 2x for the fast block (in the product the host
oversampler delivers that rate directly). Probes in the slow block are advanced by the decimator's group delay.
"""
import runpy, sys
from pathlib import Path
import numpy as np
from scipy.signal import resample_poly, firwin
from blockamp import partition, multirate

CUT = "D"
if "--cut" in sys.argv:
    i = sys.argv.index("--cut"); CUT = sys.argv[i + 1]; del sys.argv[i:i + 2]
_Orig = partition.PartSim


class _Adapter:
    def __init__(self, setting, fs, probes, clusters):
        self.t = multirate.TwoRateSim(setting, fs_base=fs, cut_after=CUT, probes=probes, clusters=clusters)
        self.out_index = self.t.out_index
        self.h = firwin(2 * 48 * 2 + 1, 0.5, window=("kaiser", 14.0))

    def run(self, x, outs):
        d = int(self.t.delay_base)
        xf = resample_poly(np.concatenate([x, np.full(d + 1, x[-1])]), 2, 1, window=self.h)
        y = self.t.run(xf, outs)
        out = np.empty((len(x), len(outs)))
        for k, o in enumerate(outs):
            slow = self.out_index[o][0] >= self.t.nf
            out[:, k] = y[d:d + len(x), k] if slow else y[:len(x), k]
        if self.t.fails.sum():
            print("  solver fallbacks:", self.t.fails)
        return out


def _factory(setting=None, phys=None, fs=384000.0, probes=(), clusters=None, **kw):
    if clusters is None:
        return _Orig(setting, phys, fs=fs, probes=probes, **kw)
    return _Adapter(setting, fs, probes, clusters)


partition.PartSim = _factory
print(f"two-rate backend: clusters up to {CUT} at 2x the base rate")
runpy.run_path(str(Path(__file__).with_name("check_spice.py")), run_name="__main__")
