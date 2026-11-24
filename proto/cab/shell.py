"""Axisymmetric thin-shell finite elements for a loudspeaker cone, dust cap and surround (round 4).

Conical-frustum elements (Kirchhoff-Love thin shell, axisymmetric loading): meridional displacement u linear, normal
displacement w cubic (Hermite) along the meridian; membrane and bending strains

    e_s = u'            e_theta = U_r / r            k_s = -w''            k_theta = -(dr/ds) w' / r

Global unknowns per node: U_r, U_z and the rotation beta = dw/ds. This is the textbook element (Grafton and Strome
1963; Zienkiewicz and Taylor, The Finite Element Method, the chapter on axisymmetric shells), written here from the
strain definitions with numerical integration. Unlike the one-dimensional bending-wave line of rounds 1 to 3 it has
the membrane-bending coupling of a real cone: the cone is stiff where it is steep and small and breaks up from the
rim inward, with no "ring frequency" parameter. Nothing here knows about any measurement.
"""
import numpy as np

_GX, _GW = np.polynomial.legendre.leggauss(4)


def element(r1, z1, r2, z2, E, h, rho, nu=0.3):
    """Stiffness and mass (6x6, global dof order U_r1, U_z1, beta1, U_r2, U_z2, beta2) of one frustum element."""
    L = float(np.hypot(r2 - r1, z2 - z1))
    tr, tz = (r2 - r1) / L, (z2 - z1) / L          # unit tangent along the meridian
    nr, nz = -tz, tr                               # unit normal (toward +z for a flat disc)
    C = E * h / (1 - nu ** 2)
    Db = E * h ** 3 / (12 * (1 - nu ** 2))
    D = np.zeros((4, 4))
    D[:2, :2] = C * np.array([[1, nu], [nu, 1]])
    D[2:, 2:] = Db * np.array([[1, nu], [nu, 1]])
    K = np.zeros((6, 6)); M = np.zeros((6, 6))
    for x, wg in zip(_GX, _GW):
        s = 0.5 * (x + 1)                          # 0..1
        r = r1 + s * (r2 - r1)
        N1, N2 = 1 - s, s
        H = np.array([1 - 3 * s ** 2 + 2 * s ** 3, L * (s - 2 * s ** 2 + s ** 3), 3 * s ** 2 - 2 * s ** 3, L * (-s ** 2 + s ** 3)])
        dH = np.array([(-6 * s + 6 * s ** 2) / L, 1 - 4 * s + 3 * s ** 2, (6 * s - 6 * s ** 2) / L, -2 * s + 3 * s ** 2])
        ddH = np.array([(-6 + 12 * s) / L ** 2, (-4 + 6 * s) / L, (6 - 12 * s) / L ** 2, (-2 + 6 * s) / L])
        # local dof order: u1, w1, b1, u2, w2, b2
        u = np.array([N1, 0, 0, N2, 0, 0]); du = np.array([-1 / L, 0, 0, 1 / L, 0, 0])
        w = np.array([0, H[0], H[1], 0, H[2], H[3]]); dw = np.array([0, dH[0], dH[1], 0, dH[2], dH[3]]); ddw = np.array([0, ddH[0], ddH[1], 0, ddH[2], ddH[3]])
        B = np.stack([du, (u * tr + w * nr) / r, -ddw, -tr * dw / r])
        jac = 2 * np.pi * r * L * 0.5 * wg
        K += B.T @ D @ B * jac
        M += rho * h * (np.outer(u, u) + np.outer(w, w)) * jac
    T = np.zeros((6, 6))
    for a in (0, 3):
        T[a, a], T[a, a + 1] = tr, tz              # u = U.t
        T[a + 1, a], T[a + 1, a + 1] = nr, nz      # w = U.n
        T[a + 2, a + 2] = 1.0
    return T.T @ K @ T, T.T @ M @ T


class Mesh:
    """Nodes on a meridian, elements in material groups, constraints and lumped terms. Assembles one real stiffness
    matrix per group (so that each group can carry its own loss factor) and one mass matrix."""

    def __init__(self):
        self.r, self.z, self.elems, self.fixed, self.groups = [], [], [], set(), []

    def node(self, r, z):
        self.r.append(float(r)); self.z.append(float(z))
        return len(self.r) - 1

    def line(self, pts, group, E, h, rho, first=None, last=None):
        """Chain of elements through pts [(r, z), ...]; first/last reuse existing node indices. h may be a list per element."""
        ids = []
        for i, (r, z) in enumerate(pts):
            if i == 0 and first is not None:
                ids.append(first)
            elif i == len(pts) - 1 and last is not None:
                ids.append(last)
            else:
                ids.append(self.node(r, z))
        if group not in self.groups:
            self.groups.append(group)
        hh = np.broadcast_to(np.asarray(h, float), (len(ids) - 1,))
        for k, (a, b) in enumerate(zip(ids, ids[1:])):
            self.elems.append((a, b, group, E, float(hh[k]), rho))
        return ids

    def fix(self, node, dofs):
        for d in dofs:
            self.fixed.add(3 * node + d)

    def assemble(self):
        n = 3 * len(self.r)
        Ks = {g: np.zeros((n, n)) for g in self.groups}
        M = np.zeros((n, n))
        self.area, self.normal = np.zeros(len(self.r)), np.zeros((len(self.r), 2))
        for a, b, g, E, h, rho in self.elems:
            k, m = element(self.r[a], self.z[a], self.r[b], self.z[b], E, h, rho)
            idx = [3 * a, 3 * a + 1, 3 * a + 2, 3 * b, 3 * b + 1, 3 * b + 2]
            Ks[g][np.ix_(idx, idx)] += k
            M[np.ix_(idx, idx)] += m
        self.free = np.array([i for i in range(n) if i not in self.fixed])
        return Ks, M

    def radiators(self, ids):
        """Per node of the chain `ids`: ring area it stands for and its unit normal (r, z components)."""
        r, z = np.array(self.r)[ids], np.array(self.z)[ids]
        ds = np.hypot(np.diff(r), np.diff(z))
        tr, tz = np.diff(r) / ds, np.diff(z) / ds
        area = np.zeros(len(ids)); nr = np.zeros(len(ids)); nz = np.zeros(len(ids))
        for k in range(len(ds)):                   # each element's area and normal split between its two nodes
            rm = 0.5 * (r[k] + r[k + 1])
            for j, rr in ((k, 0.5 * (r[k] + rm)), (k + 1, 0.5 * (r[k + 1] + rm))):
                a = 2 * np.pi * rr * ds[k] / 2
                area[j] += a; nr[j] += -tz[k] * a; nz[j] += tr[k] * a
        return area, nr / area, nz / area


def solve(Ks, M, free, etas, f, force_dof, lumped=None):
    """Displacements for a unit harmonic force at force_dof: array (len(f), ndof). etas: {group: loss factor (scalar or
    array over f)}. lumped: {dof: (mass, stiffness, damping)} added on the diagonal. The unknowns are reordered
    (reverse Cuthill-McKee) and each frequency is one banded solve."""
    from scipy.linalg import solve_banded
    from scipy.sparse import csr_matrix
    from scipy.sparse.csgraph import reverse_cuthill_mckee
    n = M.shape[0]
    w = 2 * np.pi * np.asarray(f, float)
    free = np.asarray(free)
    pat = np.abs(M[np.ix_(free, free)]) > 0
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
    Mab = banded(M)
    pos = {int(d): i for i, d in enumerate(order)}
    b = np.zeros(m, complex); b[pos[force_dof]] = 1.0
    x = np.zeros((len(w), n), complex)
    eta = {g: np.broadcast_to(np.asarray(etas[g], float), w.shape) for g in Ks}
    for k, wk in enumerate(w):
        ab = -wk ** 2 * Mab.astype(complex)
        for g in Ks:
            ab += (1 + 1j * eta[g][k]) * Kab[g]
        for d, (mm, kk, cc) in (lumped or {}).items():
            ab[bw, pos[d]] += -wk ** 2 * mm + kk + 1j * wk * cc
        x[k, order] = solve_banded((bw, bw), ab, b, check_finite=False)
    return x
