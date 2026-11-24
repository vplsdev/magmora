"""Thin-shell finite elements for circumferential order n >= 1 (round 5): the same conical-frustum
element as shell.py with the displacement varying around the axis, u and w as cos(n theta) and the circumferential
displacement v as sin(n theta) (the semi-analytic element for shells of revolution; Zienkiewicz and Taylor, the
chapter on shells of revolution with non-symmetric loads). Strains of Love's first approximation on a cone
(t_r = dr/ds, n_r = radial component of the normal):

    e_s  = u'                       e_th = (u t_r + w n_r + n v) / r          g    = v' - t_r v / r - n u / r
    k_s  = -w''                     k_th = -t_r w' / r + n (n w + n_r v) / r^2
    2k_st = 2 n w' / r + n_r v' / r - 2 t_r (n w + n_r v) / r^2

Global unknowns per node: U_r, U_z, beta = dw/ds, v. A real cone has these modes (they are what a "bell" does);
an axisymmetric drive does not excite them, a small asymmetry does. Nothing here knows about any measurement.
"""
import numpy as np

_GX, _GW = np.polynomial.legendre.leggauss(4)


def element(r1, z1, r2, z2, E, h, rho, n, nu=0.3):
    """Stiffness and mass (8x8, global dof order U_r, U_z, beta, v at node 1 then node 2) for circumferential order n."""
    L = float(np.hypot(r2 - r1, z2 - z1))
    tr, tz = (r2 - r1) / L, (z2 - z1) / L
    nr, nz = -tz, tr
    C = E * h / (1 - nu ** 2)
    Db = E * h ** 3 / (12 * (1 - nu ** 2))
    D = np.zeros((6, 6))
    D[:2, :2] = C * np.array([[1, nu], [nu, 1]]); D[2, 2] = C * (1 - nu) / 2
    D[3:5, 3:5] = Db * np.array([[1, nu], [nu, 1]]); D[5, 5] = Db * (1 - nu) / 2
    K = np.zeros((8, 8)); Mm = np.zeros((8, 8))
    for x, wg in zip(_GX, _GW):
        s = 0.5 * (x + 1)
        r = r1 + s * (r2 - r1)
        N1, N2 = 1 - s, s
        H = np.array([1 - 3 * s ** 2 + 2 * s ** 3, L * (s - 2 * s ** 2 + s ** 3), 3 * s ** 2 - 2 * s ** 3, L * (-s ** 2 + s ** 3)])
        dH = np.array([(-6 * s + 6 * s ** 2) / L, 1 - 4 * s + 3 * s ** 2, (6 * s - 6 * s ** 2) / L, -2 * s + 3 * s ** 2])
        ddH = np.array([(-6 + 12 * s) / L ** 2, (-4 + 6 * s) / L, (6 - 12 * s) / L ** 2, (-2 + 6 * s) / L])
        # local dof order: u1, w1, b1, v1, u2, w2, b2, v2
        u = np.array([N1, 0, 0, 0, N2, 0, 0, 0]); du = np.array([-1 / L, 0, 0, 0, 1 / L, 0, 0, 0])
        v = np.array([0, 0, 0, N1, 0, 0, 0, N2]); dv = np.array([0, 0, 0, -1 / L, 0, 0, 0, 1 / L])
        w = np.array([0, H[0], H[1], 0, 0, H[2], H[3], 0]); dw = np.array([0, dH[0], dH[1], 0, 0, dH[2], dH[3], 0])
        ddw = np.array([0, ddH[0], ddH[1], 0, 0, ddH[2], ddH[3], 0])
        B = np.stack([du, (u * tr + w * nr + n * v) / r, dv - tr * v / r - n * u / r,
                      -ddw, -tr * dw / r + n * (n * w + nr * v) / r ** 2,
                      2 * n * dw / r + nr * dv / r - 2 * tr * (n * w + nr * v) / r ** 2])
        jac = np.pi * r * L * 0.5 * wg
        K += B.T @ D @ B * jac
        Mm += rho * h * (np.outer(u, u) + np.outer(v, v) + np.outer(w, w)) * jac
    T = np.zeros((8, 8))
    for a in (0, 4):
        T[a, a], T[a, a + 1] = tr, tz
        T[a + 1, a], T[a + 1, a + 1] = nr, nz
        T[a + 2, a + 2] = 1.0
        T[a + 3, a + 3] = 1.0
    return T.T @ K @ T, T.T @ Mm @ T


def assemble(mesh, n):
    """Stiffness per material group and mass for order n, from a shell.Mesh (4 unknowns per node)."""
    nd = 4 * len(mesh.r)
    Ks = {g: np.zeros((nd, nd)) for g in mesh.groups}
    Mm = np.zeros((nd, nd))
    for a, b, g, E, h, rho in mesh.elems:
        k, m = element(mesh.r[a], mesh.z[a], mesh.r[b], mesh.z[b], E, h, rho, n)
        idx = [4 * a, 4 * a + 1, 4 * a + 2, 4 * a + 3, 4 * b, 4 * b + 1, 4 * b + 2, 4 * b + 3]
        Ks[g][np.ix_(idx, idx)] += k
        Mm[np.ix_(idx, idx)] += m
    return Ks, Mm


def solve(Ks, Mm, free, etas, f, rhs):
    """Displacements (len(f), ndof) for the harmonic load vector rhs (ndof,), or (len(f), ndof) if it depends on
    frequency. etas as in shell.solve. One banded solve per frequency."""
    from scipy.linalg import solve_banded
    from scipy.sparse import csr_matrix
    from scipy.sparse.csgraph import reverse_cuthill_mckee
    nd = Mm.shape[0]
    w = 2 * np.pi * np.asarray(f, float)
    free = np.asarray(free)
    pat = np.abs(Mm[np.ix_(free, free)]) > 0
    for K in Ks.values():
        pat |= np.abs(K[np.ix_(free, free)]) > 0
    perm = reverse_cuthill_mckee(csr_matrix(pat), symmetric_mode=True)
    order = free[perm]
    ix = np.ix_(order, order)
    m = len(order)
    ii, jj = np.nonzero(pat[np.ix_(perm, perm)])
    bw = int(np.abs(ii - jj).max())
    def banded(A):
        ab = np.zeros((2 * bw + 1, m))
        ab[bw + ii - jj, jj] = A[ix][ii, jj]
        return ab
    Kab = {g: banded(K) for g, K in Ks.items()}
    Mab = banded(Mm)
    rhs = np.broadcast_to(np.asarray(rhs, complex), (len(w), nd))
    x = np.zeros((len(w), nd), complex)
    eta = {g: np.broadcast_to(np.asarray(etas[g], float), w.shape) for g in Ks}
    for k, wk in enumerate(w):
        ab = -wk ** 2 * Mab.astype(complex)
        for g in Ks:
            ab += (1 + 1j * eta[g][k]) * Kab[g]
        x[k, order] = solve_banded((bw, bw), ab, rhs[k, order], check_finite=False)
    return x
