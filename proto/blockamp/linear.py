"""Small-signal frequency response of a discretised circuit, linearised at its DC operating point."""
import numpy as np
from . import dk


def response(S, freqs, in_name=None, x0=None, v0=None):
    """Complex H[f, output] from the input source `in_name` (default: first input) to every output."""
    if v0 is None:
        x0, v0 = dk.dc_op(S)
    n = S.Nv.shape[0]
    i = np.zeros(n); J = np.zeros((n, n))
    dk.eval_devices(v0, S.dev_type, S.dev_port, S.dev_par, i, J, np.zeros(12))
    ku = S.in_idx[0] if in_name is None else S.vsrc_names.index(in_name)
    M = np.linalg.inv(np.eye(n) - S.K @ J) if n else np.zeros((0, 0))
    JM = J @ M
    A = S.Ss + S.Si @ JM @ S.Ps
    B = S.Sv[:, ku] + S.Si @ JM @ S.Pv[:, ku]
    Cm = S.Ys + S.Yi @ JM @ S.Ps
    D = S.Yv[:, ku] + S.Yi @ JM @ S.Pv[:, ku]
    H = np.empty((len(freqs), Cm.shape[0]), dtype=complex)
    I = np.eye(A.shape[0])
    for k, f in enumerate(freqs):
        z = np.exp(2j * np.pi * f / S.fs)
        H[k] = Cm @ np.linalg.solve(z * I - A, B) + D
    return H
