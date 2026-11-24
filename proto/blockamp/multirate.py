"""Experimental runners for the aliasing work (WP9 task 4). Nothing here changes the verified solver in
partition.py / dk.py; this module only wraps it.

sub_sim(names, ...): a PartSim made of a contiguous subset of the clusters. If the first cluster of the subset
has an upstream cut, that cut voltage becomes the external input of run() (absolute volts, DC included), and
the last cluster's load-current input is left at its DC value (open cut).
"""
from __future__ import annotations
import numpy as np
from . import partition as _pt
from .partition import PartSim, CLUSTERS_SPLIT


def sub_sim(names, ctl=None, phys=None, fs=192000.0, clusters=CLUSTERS_SPLIT, **kw):
    sub = [c for c in clusters if c[0] in names]
    orig = _pt.build_clusters

    def patched(ctl_, phys_, clusters_, probes_=()):
        out, wiring = orig(ctl_, phys_, clusters_, probes_)
        nm = [c[0] for c in clusters_]
        keep = []
        for (dst, iname, src, label, scale) in wiring:
            if dst < 0 or src < 0:
                continue
            if iname.startswith("rail_") and not any("supply" in c[1] for c in clusters_):
                continue                                   # no supply cluster in the subset: rails stay at DC
            keep.append((dst, iname, src, label, scale))
        return out, keep
    _pt.build_clusters = patched
    try:
        sim = PartSim(ctl, phys, fs=fs, clusters=sub, **kw)
    finally:
        _pt.build_clusters = orig
    return sim


# ---------------------------------------------------------------------------------------------------------
# Two-rate runner: the clusters upstream of one cut run at 2 x the base rate, the rest at the base rate.
#
#   fast block (clusters 0 .. nf-1)  --v_out-->  decimating FIR (2:1)  --cut_in-->  slow block (nf .. end)
#        ^------------- load current and rail voltages of the slow block, held ----------------'
#
# Per base sample: two fast steps (the slow block's load current and the rails are held at their last values),
# one FIR output, one slow step (rail sink currents of the fast clusters are averaged over the two fast steps).
# The cut must be one whose one-sample loop gain is tiny (partition.py), because the FIR's group delay now sits
# inside that loop: nX (V5A's fed-back output into 470 k, loop gain 0.010) qualifies; V3A's plate into the phase
# inverter (0.25 to 0.68) does not.
from numba import njit
from numba.typed import List
from . import dk


def design_decimator(ntaps=17, fs_fast=384000.0, f_pass=20000.0, f_stop=None, w_stop=30.0):
    """Linear-phase 2:1 decimation low-pass for the cut. Pass band: audio. Stop band: everything that folds onto the
    audio band of the base rate or near it. (N-1)/2 must be even so that the delay is an integer number of base samples."""
    from scipy.signal import remez
    assert (ntaps - 1) % 4 == 0
    f_stop = f_stop or (fs_fast / 2 - 2.2 * f_pass)
    h = remez(ntaps, [0, f_pass, f_stop, fs_fast / 2], [1, 0], weight=[1, w_stop], fs=fs_fast)
    return h / h.sum()


@njit(error_model="numpy", cache=True)
def _mv(A, x, y, add):
    for a in range(A.shape[0]):
        acc = y[a] if add else 0.0
        for b in range(A.shape[1]):
            acc += A[a, b] * x[b]
        y[a] = acc


@njit(error_model="numpy")
def _advance(c, uu, Pv, Ps, K, Sv, Ss, Si, Yv, Ys, Yi, s, v, ylast, yprev, vprev, plast, pb, sb, ib, Jb, Wb, tmp,
             dev_type, dev_port, dev_par, tol, maxit, iters, fails):
    """One sample of one cluster; identical to the body of partition.run_clusters."""
    p = pb[c]
    _mv(Pv[c], uu, p, False); _mv(Ps[c], s[c], p, True)
    vc = v[c]; vp = vprev[c]
    for a in range(vc.shape[0]):
        g = 2.0 * vc[a] - vp[a]; vp[a] = vc[a]; vc[a] = g
    it = dk.newton(p, K[c], vc, dev_type[c], dev_port[c], dev_par[c], ib[c], Jb[c], tmp, tol, maxit, Wb[c])
    if it < 0:
        for a in range(vc.shape[0]): vc[a] = vp[a]
        it = dk.newton(p, K[c], vc, dev_type[c], dev_port[c], dev_par[c], ib[c], Jb[c], tmp, tol, 4 * maxit, Wb[c])
        iters[c] += maxit
    if it < 0:
        pl = plast[c]; ph = np.zeros(vc.shape[0])
        for a in range(vc.shape[0]): vc[a] = vp[a]
        ok = True
        for st in range(1, 9):
            for a in range(vc.shape[0]): ph[a] = pl[a] + (p[a] - pl[a]) * st / 8.0
            it = dk.newton(ph, K[c], vc, dev_type[c], dev_port[c], dev_par[c], ib[c], Jb[c], tmp, tol, 2 * maxit, Wb[c])
            iters[c] += 4
            if it < 0: ok = False
        if not ok:
            fails[c] += 1
    else:
        iters[c] += it
    pl2 = plast[c]
    for a in range(p.shape[0]): pl2[a] = p[a]
    yl = ylast[c]; yp = yprev[c]
    for a in range(yl.shape[0]): yp[a] = yl[a]
    _mv(Yv[c], uu, yl, False); _mv(Ys[c], s[c], yl, True); _mv(Yi[c], ib[c], yl, True)
    sn = sb[c]
    _mv(Ss[c], s[c], sn, False); _mv(Sv[c], uu, sn, True); _mv(Si[c], ib[c], sn, True)
    sc_ = s[c]
    for a in range(sn.shape[0]): sc_[a] = sn[a]


@njit(error_model="numpy")
def run_two_rate(u, sel, nf, R, h, Pv, Ps, K, Sv, Ss, Si, Yv, Ys, Yi, s, v, vdc, ylast, dev_type, dev_port, dev_par,
                 in_idx, in_src, in_scale, tol, maxit, iters, fails, extrap, hold_extrap, g_rep, kv, corr):
    """u: input at the FAST rate (2 samples per base sample). Returns the selected outputs at the BASE rate."""
    N = u.shape[0] // R; nc = len(K)
    y = np.empty((N, sel.shape[0]))
    ib = List(); Jb = List(); vprev = List(); Wb = List(); pb = List(); sb = List(); yprev = List(); plast = List(); yacc = List()
    for c in range(nc):
        n = K[c].shape[0]
        ib.append(np.zeros(n)); Jb.append(np.zeros((n, n))); vprev.append(v[c].copy())
        Wb.append(np.zeros((n + 4, max(n, 1)))); pb.append(np.zeros(n)); sb.append(np.zeros(s[c].shape[0]))
        yprev.append(ylast[c].copy()); yacc.append(ylast[c].copy())
        pl0 = np.zeros(n)
        _mv(Pv[c], vdc[c], pl0, False); _mv(Ps[c], s[c], pl0, True)
        plast.append(pl0)
    tmp = np.zeros(12)
    nh = h.shape[0]
    # FIR delay line on every output of the boundary cluster nf-1 (only v_out is used, but it keeps the code generic)
    nyb = ylast[nf - 1].shape[0]
    dl = np.empty((nh, nyb))
    for k in range(nh):
        for a in range(nyb): dl[k, a] = ylast[nf - 1][a]
    dpos = 0
    ydec = ylast[nf - 1].copy()
    for t in range(N):
        for sub in range(R):
            for c in range(nf):
                uu = vdc[c]; src = in_src[c]; sc = in_scale[c]; idx = in_idx[c]
                for k in range(idx.shape[0]):
                    q = src[k, 0]
                    if q < 0:
                        uu[idx[k]] = u[R * t + sub]
                    elif q < c:
                        uu[idx[k]] = sc[k] * ylast[q][src[k, 1]]
                    elif q < nf:
                        val = ylast[q][src[k, 1]]
                        if extrap[q]:
                            val = 2.0 * val - yprev[q][src[k, 1]]
                        uu[idx[k]] = sc[k] * val
                    else:                       # from the slow block: held (optionally extrapolated half / one base step)
                        val = sc[k] * ylast[q][src[k, 1]]
                        if q == nf and c == nf - 1:
                            # load current of the slow block. The boundary cluster carries a replica resistor (g_rep) that
                            # draws the bulk of it implicitly and without delay; only the remainder is fed back late.
                            val = corr * (val - g_rep * ydec[kv])
                        elif hold_extrap:
                            val += sc[k] * (ylast[q][src[k, 1]] - yprev[q][src[k, 1]]) * (sub + 1) / R
                        uu[idx[k]] = val
                _advance(c, uu, Pv, Ps, K, Sv, Ss, Si, Yv, Ys, Yi, s, v, ylast, yprev, vprev, plast, pb, sb, ib, Jb, Wb, tmp,
                         dev_type, dev_port, dev_par, tol, maxit, iters, fails)
                ya = yacc[c]; yl = ylast[c]
                for a in range(yl.shape[0]):
                    ya[a] = yl[a] if sub == 0 else 0.5 * (ya[a] + yl[a])      # R is 1 or 2
            for a in range(nyb): dl[dpos, a] = ylast[nf - 1][a]
            dpos = (dpos + 1) % nh
        for a in range(nyb):
            acc = 0.0
            for k in range(nh):
                acc += h[k] * dl[(dpos + k) % nh, a]
            ydec[a] = acc
        for c in range(nf, nc):
            uu = vdc[c]; src = in_src[c]; sc = in_scale[c]; idx = in_idx[c]
            for k in range(idx.shape[0]):
                q = src[k, 0]
                if q < 0:
                    uu[idx[k]] = u[R * t + R - 1]
                elif q == nf - 1 and q < c:
                    uu[idx[k]] = sc[k] * ydec[src[k, 1]]
                elif q < nf:                    # rail currents of fast clusters into the supply: two-sample mean
                    uu[idx[k]] = sc[k] * yacc[q][src[k, 1]]
                elif q < c:
                    uu[idx[k]] = sc[k] * ylast[q][src[k, 1]]
                else:
                    val = ylast[q][src[k, 1]]
                    if extrap[q]:
                        val = 2.0 * val - yprev[q][src[k, 1]]
                    uu[idx[k]] = sc[k] * val
            _advance(c, uu, Pv, Ps, K, Sv, Ss, Si, Yv, Ys, Yi, s, v, ylast, yprev, vprev, plast, pb, sb, ib, Jb, Wb, tmp,
                     dev_type, dev_port, dev_par, tol, maxit, iters, fails)
        for k in range(sel.shape[0]):
            y[t, k] = ylast[sel[k, 0]][sel[k, 1]]
    return y


class TwoRateSim:
    """Clusters before `cut_after` (inclusive) at 2 x fs_base, the rest at fs_base. run(u_fast) -> outputs at fs_base.
    Group delay of the decimator: (len(h) - 1) / 4 base samples, forward path only."""

    def __init__(self, ctl=None, phys=None, fs_base=192000.0, cut_after="D", h=None, clusters=CLUSTERS_SPLIT, tol=1e-6,
                 maxit=40, probes=(), hold_extrap=False, r_rep=None, corr=1.0, ratio=2):
        # replica of the slow block's input resistance at the cut, placed in the boundary cluster. For the cut after D
        # it is R89 + R24 = 503 k (exact at DC, knob independent; the tone stack only shunts the 33 k further).
        names = [c[0] for c in clusters]
        if r_rep is None:
            r_rep = {"D": 503e3}.get(cut_after, 0.0)
        self.r_rep = r_rep; self.corr = corr
        orig = _pt.build_clusters

        def patched(ctl_, phys_, clusters_, probes_=()):
            out, wiring = orig(ctl_, phys_, clusters_, probes_)
            if r_rep:
                cb = clusters_[names.index(cut_after)]
                out[names.index(cut_after)]["circuit"].R("Rreplica", cb[3], "0", r_rep)
            return out, wiring
        _pt.build_clusters = patched
        try:
            self.fast = PartSim(ctl, phys, fs=ratio * fs_base, clusters=clusters, tol=tol, maxit=maxit, probes=probes)
        finally:
            _pt.build_clusters = orig
        self.slow = PartSim(ctl, phys, fs=fs_base, clusters=clusters, tol=tol, maxit=maxit, probes=probes)
        self.nf = names.index(cut_after) + 1
        self.ratio = ratio
        self.h = np.ascontiguousarray(design_decimator(fs_fast=2 * fs_base) if h is None else h, dtype=np.float64)
        self.delay_base = (len(self.h) - 1) / (2.0 * ratio)
        self.fs_base = fs_base; self.tol = tol; self.maxit = maxit; self.hold_extrap = hold_extrap
        self.out_index = self.slow.out_index; self._names = names
        self.L = {}
        for k in self.slow.L:
            l = List()
            for c in range(len(names)):
                l.append((self.fast if c < self.nf else self.slow).L[k][c])
            self.L[k] = l
        self.iters = np.zeros(len(names), dtype=np.int64); self.fails = np.zeros(len(names), dtype=np.int64)

    def run(self, u_fast, outs=(("P", "spk"),)):
        sel = np.array([self.out_index[o] for o in outs], dtype=np.int64)
        L = self.L; S = self.slow
        return run_two_rate(np.ascontiguousarray(u_fast, dtype=np.float64), sel, self.nf, self.ratio, self.h, L["Pv"], L["Ps"], L["K"], L["Sv"],
                            L["Ss"], L["Si"], L["Yv"], L["Ys"], L["Yi"], L["s"], L["v"], L["vdc"], L["ylast"], S.dev_type,
                            S.dev_port, S.dev_par, S.in_idx, S.in_src, S.in_scale, self.tol, self.maxit, self.iters, self.fails,
                            S.extrap, self.hold_extrap, (1.0 / self.r_rep if self.r_rep else 0.0),
                            self.out_index[(self._names[self.nf - 1], "v_out")][1], self.corr)


def design_cut_lowpass(fs=192000.0, f_pass=20000.0, f_stop=32000.0, ntaps=49, w_stop=3.0):
    """Linear-phase low-pass applied to the cut voltage at nX when ratio = 1 (single rate): band-limits what the
    stages after the tone stack are asked to clip. A numerical device, not part of the circuit: flat to f_pass."""
    from scipy.signal import remez
    assert ntaps % 2 == 1
    h = remez(ntaps, [0, f_pass, f_stop, fs / 2], [1, 0], weight=[1, w_stop], fs=fs)
    return h / h.sum()
