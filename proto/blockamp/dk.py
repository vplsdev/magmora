"""Nodal K-method ("DK") discretisation of a Circuit, and a numba runner.

Unknown vector x = [node voltages, extra branch currents]. Capacitors and inductors are replaced by
trapezoidal companion models with one state each. With all linear parts eliminated once per
parameter set, each sample needs only

    p = Pu u + Ps s                (affine prediction of the nonlinear port voltages)
    solve  v = p + K i(v)          (Newton, dimension = number of tube/diode port voltages)
    s <- Ss s + Su u + Si i(v)     (state update)
    y = Yu u + Ys s + Yi i(v)      (outputs)

which is what the C++ engine has to do per oversampled sample.
"""
from __future__ import annotations
import numpy as np
from numba import njit
from .circuit import Circuit, GND
from . import devices, models

T_TRI, T_PEN, T_DIO = 0, 1, 2
GRID_STEP = 3.0      # volts per Newton step beyond the conduction threshold
GRID_ON = -0.5
DIODE_STEP = 0.3
DIODE_ON = 0.4
# Conductance that stands in for a capacitor in the DC solve. With 1 pS the transient started with that current
# missing in the 22 uF and 0.33 uF coupling paths, a 0.1 V bump at the speaker at t = 0 (found by the C++ port).
DC_CAP_LEAK = 1e-18
_SPLIT = 134217729.0


def _two_prod(a, b):
    """a*b as (product, rounding error), element-wise (Dekker)."""
    p_ = a * b
    c = _SPLIT * a; ah = c - (c - a); al = a - ah
    c = _SPLIT * b; bh = c - (c - b); bl = b - bh
    return p_, ((ah * bh - p_) + ah * bl + al * bh) + al * bl


def refined_inverse(G):
    """inv(G) with one step of iterative refinement whose residual I - G A is accumulated in twice the working
    precision. The companion conductances span more than ten decades, so the plain inverse loses about eight digits;
    the C++ engine refines its solves the same way, and the two then agree to rounding."""
    A = np.linalg.inv(G)
    n = G.shape[0]
    acc = np.eye(n); comp = np.zeros((n, n))
    for k in range(n):
        p_, e = _two_prod(G[:, k][:, None], A[k][None, :])
        t = acc - p_
        bb = t - acc
        comp += (acc - (t - bb)) + (-p_ - bb) - e          # two-sum error of acc + (-p_), minus the product error
        acc = t
    return A + A @ (acc + comp)


class System:
    pass


def build(c: Circuit, fs: float, extra_caps=True) -> System:
    nodes = c.node_names()
    # tube capacitances are added as explicit C elements here (same rule in spice export via expand())
    els = expand(c) if extra_caps else list(c.elements)
    for e in els:
        for n in e.nodes:
            if n != GND and n not in nodes:
                nodes.append(n)
    idx = {n: i for i, n in enumerate(nodes)}
    nn = len(nodes)
    # count extra unknowns
    extra = []
    for e in els:
        if e.kind in ("V", "L"):
            extra.append((e.name, 1))
        elif e.kind == "X":
            extra.append((e.name, len(e.value) + 1))
    xoff = {}
    nx = nn
    for name, k in extra:
        xoff[name] = nx
        nx += k
    T = 1.0 / fs
    G = np.zeros((nx, nx))
    Gdc = np.zeros((nx, nx))
    vsrc = [e for e in els if e.kind in ("V", "I")]
    nu_all = len(vsrc)
    Bv = np.zeros((nx, nu_all))        # all voltage sources (dc + inputs)
    states = [e for e in els if e.kind in ("C", "L")]
    ns = len(states)
    Bs = np.zeros((nx, ns))
    Ns = np.zeros((ns, nx))
    gs = np.zeros(ns)
    nl = [e for e in els if e.kind in ("T", "P", "D")]
    nports = sum({"T": 2, "P": 3, "D": 1}[e.kind] for e in nl)
    Nv = np.zeros((nports, nx))
    Bi = np.zeros((nx, nports))

    def stamp_g(M, a, b, g):
        ia = idx.get(a, -1) if a != GND else -1
        ib = idx.get(b, -1) if b != GND else -1
        if ia >= 0: M[ia, ia] += g
        if ib >= 0: M[ib, ib] += g
        if ia >= 0 and ib >= 0:
            M[ia, ib] -= g; M[ib, ia] -= g

    def row(a, b):
        r = np.zeros(nx)
        if a != GND: r[idx[a]] += 1.0
        if b != GND: r[idx[b]] -= 1.0
        return r

    si = 0
    vi = 0
    pi = 0
    dev_type, dev_port, dev_par = [], [], []
    for e in els:
        if e.kind == "R":
            stamp_g(G, *e.nodes, 1.0 / e.value); stamp_g(Gdc, *e.nodes, 1.0 / e.value)
        elif e.kind == "C":
            gc = 2.0 * e.value / T
            stamp_g(G, *e.nodes, gc)
            stamp_g(Gdc, *e.nodes, DC_CAP_LEAK)       # open at DC (tiny leak keeps Gdc regular)
            r = row(*e.nodes)
            Bs[:, si] = r                             # companion source injects +s into node a
            Ns[si] = r; gs[si] = gc
            si += 1
        elif e.kind == "L":
            j = xoff[e.name]; r = row(*e.nodes)
            rl = 2.0 * e.value / T
            for M, rr in ((G, rl), (Gdc, 1e-9)):
                M[:nx, j] += r                        # KCL: current j leaves a, enters b
                M[j, :] += r                          # v_a - v_b - rr*j = -s
                M[j, j] -= rr
            Bs[j, si] = -1.0
            Ns[si, j] = 1.0; gs[si] = rl
            si += 1
        elif e.kind == "V":
            j = xoff[e.name]; r = row(*e.nodes)
            for M in (G, Gdc):
                M[:nx, j] += r; M[j, :] += r
            Bv[j, vi] = 1.0
            vi += 1
        elif e.kind == "I":
            Bv[:, vi] = -row(*e.nodes)                # current leaves node a, enters node b
            vi += 1
        elif e.kind == "X":
            j0 = xoff[e.name]; turns = e.value; je = j0 + len(turns)   # je: volts-per-turn unknown
            for k, n in enumerate(turns):
                r = row(e.nodes[2 * k], e.nodes[2 * k + 1])
                for M in (G, Gdc):
                    M[:nx, j0 + k] += r               # winding current
                    M[j0 + k, :] += r                 # v_k - n_k * e = 0
                    M[j0 + k, je] -= n
                    M[je, j0 + k] += n                # sum n_k i_k = 0
            for M in (G, Gdc):
                M[je, je] += 1e-12
        elif e.kind in ("T", "P", "D"):
            if e.kind == "T":
                p, g, k = e.nodes
                ports = [(p, k), (g, k)]; par = models.triode_vec(e.value); t = T_TRI
            elif e.kind == "P":
                p, s2, g, k = e.nodes
                ports = [(p, k), (s2, k), (g, k)]
                par = models.pentode_vec(e.value, e.extra.get("n_parallel", 1)); t = T_PEN
            else:
                ports = [e.nodes]; par = models.diode_vec(e.value); t = T_DIO
            dev_type.append(t); dev_port.append(pi); dev_par.append(par + [0.0] * (12 - len(par)))
            for (a, b) in ports:
                r = row(a, b)
                Nv[pi] = r
                Bi[:, pi] = -r                        # device current flows a -> b inside the device
                pi += 1
    S = System()
    S.fs = fs; S.nodes = nodes; S.idx = idx; S.nx = nx; S.xoff = xoff
    S.vsrc_names = [e.name for e in vsrc]
    S.vdc = np.array([e.value for e in vsrc])
    S.in_idx = np.array([S.vsrc_names.index(n) for n in c.inputs], dtype=np.int64)
    S.state_names = [e.name for e in states]
    S.nl_names = [e.name for e in nl]
    S.dev_type = np.array(dev_type, dtype=np.int64); S.dev_port = np.array(dev_port, dtype=np.int64)
    S.dev_par = np.array(dev_par, dtype=np.float64).reshape(len(dev_type), 12)
    S.G, S.Gdc, S.Bv, S.Bs, S.Ns, S.gs, S.Nv, S.Bi = G, Gdc, Bv, Bs, Ns, gs, Nv, Bi
    No = np.zeros((len(c.outputs), nx))
    for k, (_, a, b) in enumerate(c.outputs):
        if a == "#i":
            No[k, xoff[b]] = 1.0
        elif a == "#res":                              # branch current of source b[0] plus b[2] times v(node b[1])
            No[k, xoff[b[0]]] = 1.0
            No[k] += b[2] * row(b[1], GND)
        else:
            No[k] = row(a, b)
    S.No = No; S.out_names = [o[0] for o in c.outputs]
    A = refined_inverse(G)
    S.A = A
    S.K = Nv @ A @ Bi
    S.Pv = Nv @ A @ Bv; S.Ps = Nv @ A @ Bs
    Z = Ns @ A
    S.Sv = 2.0 * gs[:, None] * (Z @ Bv)
    S.Ss = 2.0 * gs[:, None] * (Z @ Bs) - np.eye(ns)
    S.Si = 2.0 * gs[:, None] * (Z @ Bi)
    S.Yv = No @ A @ Bv; S.Ys = No @ A @ Bs; S.Yi = No @ A @ Bi
    S.Xv = A @ Bv; S.Xs = A @ Bs; S.Xi = A @ Bi     # full solution, for probing any node
    return S


def expand(c: Circuit):
    """Elements plus the inter-electrode capacitors implied by each tube model."""
    from .circuit import Element
    els = list(c.elements)
    for e in c.elements:
        if e.kind == "T":
            p, g, k = e.nodes; m = e.value
            els.append(Element("C", e.name + "_cgp", (g, p), m["cgp"] + m.get("cgp_stray", 0.0)))
            els.append(Element("C", e.name + "_cgk", (g, k), m["cgk"] + m.get("cgk_stray", 0.0)))
            els.append(Element("C", e.name + "_cpk", (p, k), m["cpk"]))
        elif e.kind == "P":
            p, s2, g, k = e.nodes; m = e.value; n = e.extra.get("n_parallel", 1)
            els.append(Element("C", e.name + "_cgp", (g, p), n * m["cgp"]))
            els.append(Element("C", e.name + "_cg1", (g, k), n * m["cg1"]))
            els.append(Element("C", e.name + "_cpk", (p, k), n * m["cpk"]))
    return els


@njit(error_model="numpy")
def eval_devices(v, dev_type, dev_port, dev_par, i, J, tmp):
    """Fill currents i and the (block-diagonal) Jacobian J = di/dv."""
    for d in range(dev_type.shape[0]):
        p = dev_port[d]
        if dev_type[d] == 0:
            devices.triode(v[p], v[p + 1], dev_par[d], tmp)
            i[p] = tmp[0]; i[p + 1] = tmp[1]
            J[p, p] = tmp[2]; J[p, p + 1] = tmp[3]; J[p + 1, p] = tmp[4]; J[p + 1, p + 1] = tmp[5]
        elif dev_type[d] == 1:
            devices.pentode(v[p], v[p + 1], v[p + 2], dev_par[d], tmp)
            for a in range(3):
                i[p + a] = tmp[a]
                for b in range(3):
                    J[p + a, p + b] = tmp[3 + 3 * a + b]
        else:
            devices.diode(v[p], dev_par[d], tmp)
            i[p] = tmp[0]; J[p, p] = tmp[1]


@njit(error_model="numpy")
def _lu(M, piv, n):
    for a in range(n):
        q = a; big = abs(M[a, a])
        for b in range(a + 1, n):
            if abs(M[b, a]) > big:
                big = abs(M[b, a]); q = b
        piv[a] = q
        if q != a:
            for cc in range(n):
                tt = M[a, cc]; M[a, cc] = M[q, cc]; M[q, cc] = tt
        inv = 1.0 / M[a, a]
        for b in range(a + 1, n):
            f = M[b, a] * inv
            M[b, a] = f
            if f != 0.0:
                for cc in range(a + 1, n):
                    M[b, cc] -= f * M[a, cc]


@njit(error_model="numpy")
def _lu_solve(M, piv, x, n):
    for a in range(n):                 # all row interchanges first (the factor rows were swapped whole)
        q = piv[a]
        if q != a:
            tt = x[a]; x[a] = x[q]; x[q] = tt
    for a in range(n):
        for b in range(a + 1, n):
            x[b] -= M[b, a] * x[a]
    for a in range(n - 1, -1, -1):
        acc = x[a]
        for cc in range(a + 1, n):
            acc -= M[a, cc] * x[cc]
        x[a] = acc / M[a, a]


@njit(error_model="numpy")
def newton(p, K, v, dev_type, dev_port, dev_par, i, J, tmp, tol, maxit, W):
    """Solve v = p + K i(v) in place, starting from v. W is an (n+4, n) work array.

    Damped Newton with Deuflhard's natural monotonicity test: a step of length lam is accepted when the
    NEXT Newton correction computed with the SAME Jacobian is shorter than the current one. The plain
    residual norm is useless as a merit function here (it rises 30-fold along paths that converge
    quadratically, because grid conduction is exponential). Steps into junction conduction are also
    limited so the exponentials stay in range. Returns iterations used (negative if not converged).
    """
    n = p.shape[0]
    M = W[:n]; F = W[n]; vt = W[n + 1]; dv = W[n + 2]; db = W[n + 3]
    piv = np.empty(n, dtype=np.int64)
    eval_devices(v, dev_type, dev_port, dev_par, i, J, tmp)
    for a in range(n):
        acc = p[a] - v[a]
        for b in range(n):
            acc += K[a, b] * i[b]
        F[a] = acc
    for it in range(maxit):
        for a in range(n):
            for b in range(n):
                M[a, b] = 0.0
            M[a, a] = 1.0
            dv[a] = F[a]
        for d in range(dev_type.shape[0]):      # M = I - K J, J block diagonal
            q = dev_port[d]
            w = 2 if dev_type[d] == 0 else (3 if dev_type[d] == 1 else 1)
            for a in range(n):
                for b in range(w):
                    acc = 0.0
                    for cc in range(w):
                        acc += K[a, q + cc] * J[q + cc, q + b]
                    M[a, q + b] -= acc
        _lu(M, piv, n)
        _lu_solve(M, piv, dv, n)
        # limit steps that drive a junction further into conduction (scale the whole step: keeps the direction)
        sc = 1.0
        for d in range(dev_type.shape[0]):
            q = dev_port[d]
            jp = q + 1 if dev_type[d] == 0 else (q + 2 if dev_type[d] == 1 else q)
            if dev_type[d] != 2:
                lim = GRID_STEP; von = GRID_ON
            else:
                lim = DIODE_STEP; von = DIODE_ON
            base = v[jp] if v[jp] > von else von          # free travel up to the conduction threshold
            if dv[jp] > 0.0 and v[jp] + dv[jp] > base + lim:
                r = (base + lim - v[jp]) / dv[jp]
                if r < sc: sc = r
        nd = 0.0
        for a in range(n):
            dv[a] *= sc
            if abs(dv[a]) > nd: nd = abs(dv[a])
        if nd < tol:                            # converged: first-order update of the currents, no re-evaluation
            for d in range(dev_type.shape[0]):
                q = dev_port[d]
                w = 2 if dev_type[d] == 0 else (3 if dev_type[d] == 1 else 1)
                for a in range(w):
                    acc = 0.0
                    for cc in range(w):
                        acc += J[q + a, q + cc] * dv[q + cc]
                    tmp[a] = acc
                for a in range(w):
                    i[q + a] += tmp[a]
            for a in range(n): v[a] += dv[a]
            return it + 1
        lam = 1.0
        while True:
            for a in range(n): vt[a] = v[a] + lam * dv[a]
            eval_devices(vt, dev_type, dev_port, dev_par, i, J, tmp)
            nb = 0.0
            for a in range(n):
                acc = p[a] - vt[a]
                for b in range(n): acc += K[a, b] * i[b]
                F[a] = acc; db[a] = acc
            if sc < 1.0 or lam < 0.02:
                break                           # limited or tiny step: take it, the test below would be meaningless
            _lu_solve(M, piv, db, n)
            for a in range(n):
                if abs(db[a]) > nb: nb = abs(db[a])
            if nb <= (1.0 - 0.25 * lam) * nd or nb < tol:
                break
            lam *= 0.5
        for a in range(n): v[a] = vt[a]
    return -maxit


def dc_op(S: System, vsrc=None, steps=20, tol=1e-9):
    """DC operating point by adaptive source stepping. Returns x (full unknown vector) and port voltages v."""
    vs = S.vdc if vsrc is None else vsrc
    Adc = refined_inverse(S.Gdc)
    Kdc = S.Nv @ Adc @ S.Bi
    Pdc = S.Nv @ Adc @ S.Bv
    n = S.Nv.shape[0]
    v = np.zeros(n); i = np.zeros(n); J = np.zeros((n, n)); tmp = np.zeros(12); W = np.zeros((n + 4, max(n, 1)))
    lam, dl = 0.0, 1.0 / steps
    while lam < 1.0:
        nxt = min(1.0, lam + dl)
        vtry = v.copy()
        it = newton(Pdc @ (vs * nxt), Kdc, vtry, S.dev_type, S.dev_port, S.dev_par, i, J, tmp, tol, 100, W) if n else 1
        if it < 0:
            dl *= 0.5
            if dl < 1e-6:
                raise RuntimeError(f"dc_op did not converge near source factor {lam:.4f}")
            continue
        v = vtry; lam = nxt
        if it < 12: dl = min(dl * 1.5, 0.25)
    if n:
        eval_devices(v, S.dev_type, S.dev_port, S.dev_par, i, J, tmp)
    x = Adc @ (S.Bv @ vs + S.Bi @ i)
    return x, v


def init_state(S: System, x):
    """Trapezoidal companion states consistent with a DC solution x."""
    q = S.Ns @ x                      # capacitor voltages / inductor currents
    return S.gs * q                   # cap: s = gc*v (i=0); inductor: s = rl*j (v=0)


@njit(error_model="numpy")
def run(u, vdc, in_idx, s, v, Pv, Ps, K, Sv, Ss, Si, Yv, Ys, Yi, dev_type, dev_port, dev_par, tol, maxit):
    """u: (N, n_inputs) input-source values. Returns y (N, n_out), total Newton iterations, fail count."""
    N = u.shape[0]
    n = K.shape[0]; ny = Yv.shape[0]
    y = np.empty((N, ny))
    i = np.zeros(n); J = np.zeros((n, n)); tmp = np.zeros(12); W = np.zeros((n + 4, max(n, 1)))
    uu = vdc.copy()
    vprev = v.copy()
    iters = 0; fails = 0
    for t in range(N):
        for k in range(in_idx.shape[0]):
            uu[in_idx[k]] = u[t, k]
        p = Pv @ uu + Ps @ s
        # predictor: linear extrapolation of the previous two solutions
        for a in range(n):
            g = 2.0 * v[a] - vprev[a]
            vprev[a] = v[a]
            v[a] = g
        it = newton(p, K, v, dev_type, dev_port, dev_par, i, J, tmp, tol, maxit, W)
        if it < 0:
            fails += 1; iters += maxit
        else:
            iters += it
        y[t] = Yv @ uu + Ys @ s + Yi @ i           # uses s(n-1): must precede the state update
        s[:] = Ss @ s + Sv @ uu + Si @ i
    return y, iters, fails
