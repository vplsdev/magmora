"""Convenience wrapper: build, find the DC point, run blocks, keep state between calls."""
import numpy as np
from . import dk


class Sim:
    def __init__(self, circuit, fs, tol=1e-7, maxit=50):
        self.c = circuit; self.fs = fs; self.tol = tol; self.maxit = maxit
        self.S = dk.build(circuit, fs)
        self.x0, self.v = dk.dc_op(self.S)
        self.s = dk.init_state(self.S, self.x0)
        self.iters = 0; self.fails = 0; self.samples = 0

    def node_dc(self, name):
        return self.x0[self.S.idx[name]]

    def run(self, u):
        S = self.S
        u = np.asarray(u, dtype=np.float64)
        if u.ndim == 1:
            u = u[:, None]
        y, it, fl = dk.run(u, S.vdc.copy(), S.in_idx, self.s, self.v, S.Pv, S.Ps, S.K, S.Sv, S.Ss, S.Si,
                           S.Yv, S.Ys, S.Yi, S.dev_type, S.dev_port, S.dev_par, self.tol, self.maxit)
        self.iters += it; self.fails += fl; self.samples += len(u)
        return y
