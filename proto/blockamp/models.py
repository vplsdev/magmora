"""Device parameter sets. Every number is fitted to data-sheet curves that were digitised for this project
(docs/research/data/*.csv, fits reproduced by scripts/fit_devices.py) or copied from a table of a paper that was
opened; docs/research/DEVICES.md gives document, page and figure for each. Nothing here is quoted from memory.
Values marked ESTIMATE have no published source and are bounded fit parameters.
"""

# 12AX7 / ECC83 -----------------------------------------------------------------------------
T12AX7 = dict(
    # Koren's plate law with a grid offset vct, fitted to GE ET-T509B p.3 and Philips ECC83 (1970) sheet 5:
    # rms error 0.04 / 0.06 mA. (Koren's published set, 100/1.4/1060/600/300, is 0.22 / 0.25 mA rms and gives
    # 0.10 mA where both sheets print 0.48 mA at 100 V, -1 V.)
    mu=98.40, ex=1.8114, kg1=2783.6, kp=857.6, kvb=47.27, vct=0.7790,
    # grid current ig = gg * (softplus(cg*(vgk+vg0))/cg)^xi : Dempwolf & Zoelzer, DAFx-11, eq. 11 and Table 1,
    # tube "RSD-1" as printed. vg0 is our contact-potential spread knob (0 = paper).
    gg=6.177e-4, cg=9.901, xi=1.314, vg0=0.0,
    cgp=1.7e-12, cgk=1.6e-12, cpk=0.46e-12,                  # GE ET-T509B p.1, per section, no shield
    cgp_stray=0.7e-12, cgk_stray=0.7e-12,                    # ESTIMATE: socket + PCB trace (totals match the 2.4 /
                                                             # 2.3 pF that Dempwolf & Zoelzer and Koren use)
)

# 6L6GC ---------------------------------------------------------------------------------------
P6L6GC = dict(
    # fitted to the Tung-Sol 6L6GC curves (plates #5818/#5819, 400 V screen): plate family 7.1 mA rms, screen
    # current 4.4 mA rms; reproduces the GE class AB1 table (450 V, 5.6 k: 55 W) within 5 % without being fitted to it
    mu=10.828, ex=1.3921, kg1=1624.7, kp=30.29,
    kv=8.670e-4,                                             # knee line: i = kv * vp^1.5 per tube
    lam=6.326e-4,                                            # slope above the knee, per volt
    rs=0.06434, asc=0.3661,                                  # screen share above the knee / refused-current share
    gg=1.4224e-3, xi=0.9732,                                 # grid current: Tung-Sol plate #5819, +4 to +20 V
    cg=8.0,                                                  # ESTIMATE: onset sharpness (nothing published below +4 V)
    cg1=10e-12, cpk=6.5e-12, cgp=0.6e-12,                    # data sheet input / output / transfer capacitance
)

# Silicon diodes ----------------------------------------------------------------------------
D1N4007 = dict(isat=7.0e-9, nvt=1.8 * 0.02585, vcrit=0.75)   # generic rectifier, ESTIMATE


def triode_vec(m):
    return [m["mu"], m["ex"], m["kg1"], m["kp"], m["kvb"], m["gg"], m["cg"], m["xi"], m["vg0"], m["vct"]]


def pentode_vec(m, npar=1):
    return [m["mu"], m["ex"], m["kg1"], m["kp"], m["kv"], m["rs"], m["gg"], m["cg"], m["xi"], m["lam"], float(npar), m["asc"]]


def diode_vec(m):
    return [m["isat"], m["nvt"], m["vcrit"]]
