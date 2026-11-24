"""Tube and diode equations. The same formulas are emitted as ngspice B-sources by spice.py.

Triode plate current: Koren (1996) form with a grid-voltage offset. Grid current: smooth power-law diode of
Dempwolf & Zoelzer (2011). Beam tetrode: Koren space current, a 3/2-power knee line and a screen share that
rises below the knee (docs/research/DEVICES.md; fits reproduced by scripts/fit_devices.py).
All parameter values live in models.py with their sources.
"""
import math
from numba import njit

LN_MAX = 40.0
KNEE_BETA = 4.0
PEN_EPS3 = 1e-60      # keeps the smooth minimum defined when both currents vanish (amps^3)
PEN_EPS1 = 1e-20


@njit(cache=True, error_model="numpy")
def softplus(x):
    """log(1+exp(x)) and its derivative, overflow-safe."""
    if x > LN_MAX:
        return x, 1.0
    if x < -LN_MAX:
        return 0.0, 0.0          # below 4e-18: treated as cut off (avoids denormals in the solver)
    e = math.exp(x)
    return math.log1p(e), e / (1.0 + e)


@njit(cache=True, error_model="numpy")
def triode(vpk, vgk, P, out):
    """P = [mu, ex, kg1, kp, kvb, gg, cg, xi, vg0, vct]. out[0:2] = ip, ig; out[2:6] = dip/dvpk, dip/dvgk, dig/dvpk, dig/dvgk.
    vct is a constant added to the grid voltage in the plate law only (contact potential plus initial electron
    velocity); with it Koren's form follows the data-sheet curves at low plate voltage (docs/research/DEVICES.md 2)."""
    mu, ex, kg1, kp, kvb, gg, cg, xi, vg0, vct = P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7], P[8], P[9]
    # plate current
    if vpk <= 0.0:
        ip = 0.0; dpp = 0.0; dpg = 0.0
    else:
        r = math.sqrt(kvb + vpk * vpk)
        x = kp * (1.0 / mu + (vgk + vct) / r)
        sp, dsp = softplus(x)
        e1 = vpk / kp * sp
        if e1 <= 0.0:
            ip = 0.0; dpp = 0.0; dpg = 0.0
        else:
            ip = 2.0 / kg1 * e1 ** ex
            dip_de1 = ex * ip / e1
            dx_dvpk = -kp * (vgk + vct) * vpk / (r * r * r)
            de1_dvpk = sp / kp + vpk / kp * dsp * dx_dvpk
            de1_dvgk = vpk * dsp / r
            dpp = dip_de1 * de1_dvpk
            dpg = dip_de1 * de1_dvgk
    # grid current
    sg, dsg = softplus(cg * (vgk + vg0))
    if sg <= 0.0:
        ig = 0.0; dgg = 0.0
    else:
        base = sg / cg
        ig = gg * base ** xi
        dgg = xi * ig / base * dsg
    out[0] = ip; out[1] = ig
    out[2] = dpp; out[3] = dpg; out[4] = 0.0; out[5] = dgg


@njit(cache=True, error_model="numpy")
def pentode(vpk, vsk, vgk, P, out):
    """Beam tetrode. P = [mu, ex, kg1, kp, kv, rs, gg, cg, xi, lam, npar, asc] (per tube; npar tubes in parallel).
    Space current (Koren):  i0 = (pi/kg1) * E1^ex,  E1 = softplus(kp(1/mu + vgk/vsk)) * vsk/kp
    Below the knee every grid curve of a beam tube falls onto one 3/2-power line a = kv * vp^1.5, with
    vp = softplus(4 vpk)/4 (equals vpk above about 1 V, 0 below -1 V). The plate takes the smooth minimum of the two:
      plate   ip = m * (1 + lam vp),   m = i0 a / (i0^3 + a^3)^(1/3)
      screen  is = i0 * (rs + asc * i0 / (i0 + a))     rs: share above the knee; the second term is the part of
                                                        the space current the plate refuses below the knee
      grid    ig = gg * (softplus(cg vgk)/cg)^xi
    Fitted to digitised data-sheet curves: docs/research/DEVICES.md section 4.
    out[0:3] = ip, is, ig ; out[3:12] = Jacobian row-major wrt (vpk, vsk, vgk)."""
    mu, ex, kg1, kp, kv, rs = P[0], P[1], P[2], P[3], P[4], P[5]
    gg, cg, xi, lam, npar, asc = P[6], P[7], P[8], P[9], P[10], P[11]
    vs = vsk if vsk > 1.0 else 1.0
    dvs = 1.0 if vsk > 1.0 else 0.0
    x = kp * (1.0 / mu + vgk / vs)
    sp, dsp = softplus(x)
    e1 = vs / kp * sp
    if e1 <= 0.0:
        i0 = 0.0; di0_ds = 0.0; di0_dg = 0.0
    else:
        i0 = math.pi / kg1 * e1 ** ex
        di0_de1 = ex * i0 / e1
        de1_ds = (sp / kp + vs / kp * dsp * (-kp * vgk / (vs * vs))) * dvs
        de1_dg = dsp
        di0_ds = di0_de1 * de1_ds
        di0_dg = di0_de1 * de1_dg
    # smoothly rectified plate voltage (width 1/KNEE_BETA volts): no corner at vpk = 0, where Newton would cycle
    # when the plate rings around zero on an inductive load
    vps, dvps = softplus(KNEE_BETA * vpk)
    vps /= KNEE_BETA
    rt = math.sqrt(vps)
    a = kv * vps * rt
    da = 1.5 * kv * rt * dvps
    d3 = i0 * i0 * i0 + a * a * a + PEN_EPS3
    c = d3 ** (-1.0 / 3.0)
    m = i0 * a * c
    dm_di0 = a * c * (1.0 - i0 * i0 * i0 / d3)
    dm_da = i0 * c * (1.0 - a * a * a / d3)
    lf = 1.0 + lam * vps
    den = i0 + a + PEN_EPS1
    fs_ = rs + asc * i0 / den
    dis_di0 = rs + asc * i0 * (i0 + 2.0 * a + 2.0 * PEN_EPS1) / (den * den)
    dis_da = -asc * i0 * i0 / (den * den)
    sg, dsg = softplus(cg * vgk)
    if sg <= 0.0:
        ig = 0.0; dgg = 0.0
    else:
        base = sg / cg
        ig = gg * base ** xi
        dgg = xi * ig / base * dsg
    out[0] = npar * m * lf
    out[1] = npar * i0 * fs_
    out[2] = npar * ig
    out[3] = npar * (dm_da * da * lf + m * lam * dvps); out[4] = npar * dm_di0 * di0_ds * lf;  out[5] = npar * dm_di0 * di0_dg * lf
    out[6] = npar * dis_da * da;                        out[7] = npar * dis_di0 * di0_ds;      out[8] = npar * dis_di0 * di0_dg
    out[9] = 0.0;                                       out[10] = 0.0;                         out[11] = npar * dgg


@njit(cache=True, error_model="numpy")
def diode(v, P, out):
    """Shockley diode with linear continuation above vcrit. P = [is, nvt, vcrit]."""
    isat, nvt, vcrit = P[0], P[1], P[2]
    if v > vcrit:
        e = math.exp(vcrit / nvt)
        out[0] = isat * (e - 1.0) + isat * e / nvt * (v - vcrit)
        out[1] = isat * e / nvt
    else:
        e = math.exp(v / nvt)
        out[0] = isat * (e - 1.0)
        out[1] = isat * e / nvt + 1e-12
