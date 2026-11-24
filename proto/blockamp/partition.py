"""Real-time partition of the amp into small clusters, and a multi-cluster runner.

Cut rule: cut only at a node driven from a low impedance and loaded by a
much higher one. The downstream cluster is driven by an ideal voltage source that copies the
upstream node voltage of the SAME sample; the upstream cluster sees the current the downstream
cluster actually drew, as a current source delayed by ONE oversampled sample. Supply rails are
treated the same way (rail voltage from the power cluster, rail currents fed back one sample late).
"""
from __future__ import annotations
import numpy as np
from numba import njit
from numba.typed import List
from . import amp, dk
from .circuit import GND

NO_EXTRAP = ("P",)

# name, sections, input cut node (voltage source), output cut node (load current sink), rails used
CLUSTERS = [
    ("A", ("input", "v1a", "chan", "v1b"), None, "p1b", ("v1",)),
    ("B", ("ic2", "v2a", "v2b"), "p1b", "p2b", ("v1", "v2")),
    ("C", ("i5b", "v5b"), "p2b", "p5b", ("v2",)),
    ("D", ("i5a", "v5a"), "p5b", "nX", ("v2",)),
    ("T", ("tone", "cf", "loop", "v3a"), "nX", "p3a", ("v2",)),
    ("P", ("ipi", "pi", "power", "supply"), "p3a", None, ()),
]

# Variant with the supply ladder as its own (purely linear) cluster: the power cluster loses six
# reservoir-capacitor states. Rails are stiff at the sample time scale, so the one-sample coupling is harmless.
CLUSTERS_SPLIT = CLUSTERS[:5] + [
    ("P", ("ipi", "pi", "power"), "p3a", None, ("v3", "sc", "b")),
    ("S", ("supply",), None, None, ()),
]


COMPANION = True     # coupling of the signal cuts : True = companion coupling with a
                     # first-order hold on every cut (the C++ engine's default); False = delayed load currents


def build_clusters(ctl=None, phys=None, clusters=CLUSTERS, probes=(), fs=None, companion=False, dc=None):
    """Returns list of dicts(name, circuit, in_names) and wiring list (dst, input name, src cluster, label, scale)."""
    out, wiring = [], []
    names = [c[0] for c in clusters]
    for ci, (name, sections, cin, cout, rails) in enumerate(clusters):
        c = amp.build(ctl, phys, sections=sections, title=f"cluster {name}")
        if cin is not None:
            c.V("cut_in", cin, GND, 0.0, is_input=True)
            c.out_i("i_in", "cut_in")
            wiring.append((ci, "cut_in", ci - 1, "v_out", 1.0))
            wiring.append((ci - 1, "load", ci, "i_in", -1.0))      # source branch current is minus the current drawn
        if cout is not None:
            c.I("load", cout, GND, 0.0, is_input=True)
            c.out("v_out", cout)
        for r in rails:
            c.V("rail_" + r, r, GND, 0.0, is_input=True)
            c.out_i("i_" + r, "rail_" + r)                           # <0 when the cluster draws current
            wiring.append((ci, "rail_" + r, len(clusters) - 1, "v_" + r, 1.0))
        if "supply" in sections:
            for r in sorted({r for cl in clusters for r in cl[4]}):
                c.out("v_" + r, r)
            for cj, cl in enumerate(clusters):
                for r in cl[4]:
                    c.I(f"sink_{r}_{cl[0]}", r, GND, 0.0, is_input=True)
                    wiring.append((ci, f"sink_{r}_{cl[0]}", cj, "i_" + r, -1.0))
        if "power" in sections:
            c.out("spk", "spk")
        for p in probes:
            if p in c.node_names():
                c.out("probe_" + p, p)
        out.append(dict(name=name, circuit=c))
    if companion:
        # Companion coupling. Within one sample the load a downstream cluster presents at its cut
        # is, to first order, a fixed conductance g: its trapezoidal companion network with the tubes linearised at
        # the operating point. That part goes into the upstream cluster as a resistor, where it is solved implicitly.
        # What remains, i_drawn - g * v_cut, is formed by the runner from the downstream cluster's present state and
        # its previous tube currents and inputs (output row "i_res"); only the departure of the tubes from their
        # operating-point slopes is therefore one sample late. g is read off the downstream matrices, so clusters are
        # visited from the output towards the input (each g includes the resistor already added behind it).
        for ci in range(len(clusters) - 1, 0, -1):
            cin = clusters[ci][2]
            if cin is None or clusters[ci - 1][3] != cin:
                continue
            c = out[ci]["circuit"]
            for e in c.elements:
                if e.kind == "V" and (e.name == "cut_in" or e.name.startswith("rail_")):
                    e.value = float(dc[e.nodes[0]])
            S = dk.build(c, fs)
            _, v0 = dk.dc_op(S)
            n = S.K.shape[0]
            i0 = np.zeros(n); J = np.zeros((n, n))
            dk.eval_devices(v0, S.dev_type, S.dev_port, S.dev_par, i0, J, np.zeros(12))
            k, col = S.out_names.index("i_in"), S.vsrc_names.index("cut_in")
            g2 = -float(S.Yi[k] @ J @ np.linalg.solve(np.eye(n) - S.K @ J, S.Pv[:, col]))   # the tubes' share of g
            g = -float(S.Yv[k, col]) + g2
            c.outputs.append(("i_res", "#res", ("cut_in", cin, g)))
            out[ci - 1]["circuit"].R("Rcomp", cin, GND, 1.0 / g)
            out[ci]["g_in"] = g; out[ci]["g_tubes"] = g2
            wiring[:] = [(d, i, s_, ("i_res" if (d == ci - 1 and i == "load") else lab), sc) for (d, i, s_, lab, sc) in wiring]
    return out, wiring


class PartSim:
    def __init__(self, ctl=None, phys=None, fs=384000.0, tol=1e-6, maxit=40, probes=(), clusters=CLUSTERS, extrap=True,
                 companion=None, no_extrap=None):
        self.fs = fs
        mono = dk.build(amp.build(ctl, phys), fs)
        x0, _ = dk.dc_op(mono)
        self.mono_dc = {n: x0[mono.idx[n]] for n in mono.nodes}
        companion = COMPANION if companion is None else companion
        if no_extrap is None:   # the delayed coupling may not extrapolate the last cut (D4); the companion remainder is
            no_extrap = () if companion else NO_EXTRAP   # held on every cut (D13: un-held on the last cut is 2.4 dB off)
        cl, wiring = build_clusters(ctl, phys, clusters, probes, fs=fs, companion=True, dc=self.mono_dc) if companion else build_clusters(ctl, phys, clusters, probes)
        self.cl = cl
        # DC values for sources: cut voltages and rails from the monolithic operating point
        for c in cl:
            for e in c["circuit"].elements:
                if e.kind == "V" and (e.name == "cut_in" or e.name.startswith("rail_")):
                    e.value = float(self.mono_dc[e.nodes[0]])
        # fixed-point on the DC load / sink currents (they are tiny or zero except the rail sinks)
        for _ in range(6):
            sys_ = []
            for c in cl:
                S = dk.build(c["circuit"], fs)
                x, v = dk.dc_op(S)
                c.update(S=S, x0=x, v0=v)
                sys_.append(S)
            for (dst, iname, src, label, scale) in wiring:
                e = next(e for e in cl[dst]["circuit"].elements if e.name == iname)
                if e.kind == "I":
                    S = cl[src]["S"]
                    k = S.out_names.index(label)
                    e.value = float(scale * (S.No[k] @ cl[src]["x0"]))
        n = len(cl)
        self.L = dict()
        mk = lambda: List()
        keys = ["Pv", "Ps", "K", "Sv", "Ss", "Si", "Yv", "Ys", "Yi", "s", "v", "vdc", "ylast"]
        for k in keys: self.L[k] = mk()
        self.dev_type, self.dev_port, self.dev_par, self.in_idx = mk(), mk(), mk(), mk()
        self.in_src, self.in_scale = mk(), mk(); self.pred = mk(); self.predg = mk(); self.predcut = mk()
        self.out_index = {}
        for ci, c in enumerate(cl):
            S = c["S"]
            for k in ("Pv", "Ps", "K", "Sv", "Ss", "Si", "Yv", "Ys", "Yi"):
                self.L[k].append(np.ascontiguousarray(getattr(S, k)))
            self.L["s"].append(dk.init_state(S, c["x0"])); self.L["v"].append(c["v0"].copy())
            self.L["vdc"].append(S.vdc.copy()); self.L["ylast"].append(S.No @ c["x0"])
            self.dev_type.append(S.dev_type); self.dev_port.append(S.dev_port); self.dev_par.append(S.dev_par)
            names = c["circuit"].inputs
            self.in_idx.append(S.in_idx)
            src = np.full((len(names), 2), -1, dtype=np.int64); sc = np.ones(len(names))
            for (dst, iname, s_, label, scale) in wiring:
                if dst == ci:
                    k = names.index(iname)
                    src[k, 0] = s_; src[k, 1] = cl[s_]["S"].out_names.index(label); sc[k] = scale
            self.in_src.append(src); self.in_scale.append(sc)
            self.pred.append(np.array([any(d == ci and i == nm and lab == "i_res" for (d, i, s_, lab, sc_) in wiring) for nm in names], dtype=np.bool_))
            nxt = cl[ci + 1] if ci + 1 < len(cl) else None       # the cluster that loads this one, if coupled by companion
            self.predg.append(np.array([(nxt["g_tubes"] if (self.pred[-1][k] and nxt is not None and "g_tubes" in nxt) else 0.0) for k in range(len(names))]))
            self.predcut.append(np.array([(nxt["S"].vsrc_names.index("cut_in") if (self.pred[-1][k] and nxt is not None and "g_tubes" in nxt) else 0) for k in range(len(names))], dtype=np.int64))
            for k, nm in enumerate(S.out_names):
                self.out_index[(c["name"], nm)] = (ci, k)
        self.tol, self.maxit = tol, maxit
        # Extrapolating a delayed load current triples the loop gain at Nyquist. That is safe where the worst-case
        # loop gain is far below 1/3 (every cut except V3A's plate into the phase inverter: with V3A cut off and the
        # PI grid conducting it reaches 0.68). Values coming from the clusters named in NO_EXTRAP are used as they are.
        self.extrap = np.array([bool(extrap) and c["name"] not in no_extrap for c in cl])
        self.iters = np.zeros(n, dtype=np.int64); self.fails = np.zeros(n, dtype=np.int64); self.samples = 0

    def run(self, u, outs=(("P", "spk"),)):
        sel = np.array([self.out_index[o] for o in outs], dtype=np.int64)
        L = self.L
        y = run_clusters(np.ascontiguousarray(u, dtype=np.float64), sel, L["Pv"], L["Ps"], L["K"], L["Sv"], L["Ss"], L["Si"],
                         L["Yv"], L["Ys"], L["Yi"], L["s"], L["v"], L["vdc"], L["ylast"], self.dev_type, self.dev_port,
                         self.dev_par, self.in_idx, self.in_src, self.in_scale, self.tol, self.maxit, self.iters, self.fails, self.extrap, self.pred, self.predg, self.predcut)
        self.samples += len(u)
        return y


@njit(error_model="numpy")
def _mv(A, x, y, add):
    """y (+)= A x without allocating."""
    for a in range(A.shape[0]):
        acc = y[a] if add else 0.0
        for b in range(A.shape[1]):
            acc += A[a, b] * x[b]
        y[a] = acc


@njit(error_model="numpy")
def run_clusters(u, sel, Pv, Ps, K, Sv, Ss, Si, Yv, Ys, Yi, s, v, vdc, ylast, dev_type, dev_port, dev_par,
                 in_idx, in_src, in_scale, tol, maxit, iters, fails, extrap, pred, predg, predcut):
    N = u.shape[0]; nc = len(K)
    ibp = List(); uprev = List()
    y = np.empty((N, sel.shape[0]))
    ib = List(); Jb = List(); vprev = List(); Wb = List(); pb = List(); sb = List(); yprev = List(); plast = List()
    tmp0 = np.zeros(12)
    for c in range(nc):
        n = K[c].shape[0]
        # The companion remainder reads a downstream cluster's tube currents before that cluster has run in this sample:
        # they start at the operating point (zeros made a 19 V bump at the speaker on the first sample).
        i0 = np.zeros(n); J0 = np.zeros((n, n))
        if n:
            dk.eval_devices(v[c], dev_type[c], dev_port[c], dev_par[c], i0, J0, tmp0)
        ib.append(i0); ibp.append(i0.copy()); uprev.append(vdc[c].copy())
        Jb.append(np.zeros((n, n))); vprev.append(v[c].copy())
        Wb.append(np.zeros((n + 4, max(n, 1)))); pb.append(np.zeros(n)); sb.append(np.zeros(s[c].shape[0]))
        yprev.append(ylast[c].copy())
        pl0 = np.zeros(n)
        _mv(Pv[c], vdc[c], pl0, False); _mv(Ps[c], s[c], pl0, True)
        plast.append(pl0)
    tmp = np.zeros(12)
    for t in range(N):
        for c in range(nc):
            uu = vdc[c]
            up_ = uprev[c]
            for a in range(uu.shape[0]): up_[a] = uu[a]
            src = in_src[c]; sc = in_scale[c]; idx = in_idx[c]
            for k in range(idx.shape[0]):
                q = src[k, 0]
                if q < 0:
                    uu[idx[k]] = u[t]
                elif q < c:      # upstream cluster: already advanced to this sample
                    uu[idx[k]] = sc[k] * ylast[q][src[k, 1]]
                elif pred[c][k]:  # companion remainder: exact from the downstream cluster's present state; only its
                    r = src[k, 1]  # tube currents (and rails) are one sample old
                    val = 0.0
                    Ysq = Ys[q]; sq = s[q]
                    for b_ in range(sq.shape[0]): val += Ysq[r, b_] * sq[b_]
                    Yvq = Yv[q]; uq = vdc[q]
                    for b_ in range(uq.shape[0]): val += Yvq[r, b_] * uq[b_]
                    Yiq = Yi[q]; iq = ib[q]
                    for b_ in range(iq.shape[0]): val += Yiq[r, b_] * iq[b_]
                    if extrap[q]:  # first-order hold on the tubes' departure from their operating-point slopes
                        ip = ibp[q]; d = 0.0
                        for b_ in range(iq.shape[0]): d += Yiq[r, b_] * (iq[b_] - ip[b_])
                        val += d + predg[c][k] * (uq[predcut[c][k]] - uprev[q][predcut[c][k]])
                    uu[idx[k]] = sc[k] * val
                else:            # downstream cluster: one sample old, optionally linearly extrapolated
                    val = ylast[q][src[k, 1]]
                    if extrap[q]:
                        val = 2.0 * val - yprev[q][src[k, 1]]
                    uu[idx[k]] = sc[k] * val
            p = pb[c]
            _mv(Pv[c], uu, p, False); _mv(Ps[c], s[c], p, True)
            vc = v[c]; vp = vprev[c]
            ic_ = ib[c]; ip_ = ibp[c]
            for a in range(vc.shape[0]):
                g = 2.0 * vc[a] - vp[a]; vp[a] = vc[a]; vc[a] = g; ip_[a] = ic_[a]
            it = dk.newton(p, K[c], vc, dev_type[c], dev_port[c], dev_par[c], ib[c], Jb[c], tmp, tol, maxit, Wb[c])
            if it < 0:
                # Recovery (the same rule as the C++ engine's): go back to the previous solution, which the circuit
                # actually visited, and walk the excitation p from last sample's value to this one's. A sub-step that
                # fails is halved, one that converges is doubled. The two fixed fallbacks used before (a retry, then
                # eight equal steps) left wrong samples behind whenever one of the eight steps failed.
                iters[c] += maxit
                pl = plast[c]; ph = np.zeros(vc.shape[0]); acc = np.zeros(vc.shape[0])
                for a in range(vc.shape[0]): vc[a] = vp[a]
                done = 0.0; step = 0.25; ok = False
                for attempt in range(64):
                    nxt = min(1.0, done + step)
                    for a in range(vc.shape[0]):
                        ph[a] = pl[a] + (p[a] - pl[a]) * nxt; acc[a] = vc[a]
                    it = dk.newton(ph, K[c], vc, dev_type[c], dev_port[c], dev_par[c], ib[c], Jb[c], tmp, tol, 20, Wb[c])
                    iters[c] += 20 if it < 0 else it
                    if it < 0:
                        for a in range(vc.shape[0]): vc[a] = acc[a]
                        step *= 0.5
                        if step < 1.0 / 1024.0: break
                    else:
                        done = nxt; step *= 2.0
                        if done >= 1.0:
                            ok = True
                            break
                if not ok:
                    for a in range(vc.shape[0]): vc[a] = vp[a]
                    dk.newton(p, K[c], vc, dev_type[c], dev_port[c], dev_par[c], ib[c], Jb[c], tmp, tol, maxit, Wb[c])
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
        for k in range(sel.shape[0]):
            y[t, k] = ylast[sel[k, 0]][sel[k, 1]]
    return y
