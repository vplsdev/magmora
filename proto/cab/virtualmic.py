"""Virtual microphone on the user's own response files . Python prototype of the C++ cabinet block (dsp/src/cab).

Nothing here contains or reads any response: functions take arrays and file *names*. Three parts:
  1. position metadata from file names (several naming schemes) and from a small user-editable JSON mapping;
  2. interpolation between two recorded positions (snap / linear / log-magnitude / feature-tracking morph), with
     minimum-phase reconstruction, and bilinear use over cap distance and grille distance;
  3. two microphones: alignment, polarity, offset in millimetres, level, pan.
docs/research/CABINET.md section 3 describes the design.
"""
import json, re
from pathlib import Path
import numpy as np

C0 = 343.0
INCH = 25.4

# ----------------------------------------------------------------------------- 1. position metadata
# A microphone token is up to three letters (a maker's model prefix, whatever it is) followed by a model-number core,
# as one word: not preceded by a letter or digit, not followed by a digit. (core, fewest, most prefix letters), in
# priority order. Short cores need a prefix, because bare "87" or "88" are not microphones. The shortest common
# tokens (one or two digits after letters, e.g. a speaker code ending in "20") cannot be told apart from pack codes
# by a generic rule; such packs name their microphone in the mapping file.
MIC_CORES = [("57", 0, 3), ("58", 0, 3), ("421", 0, 3), ("121", 0, 3), ("160", 0, 3), ("184", 0, 3), ("414", 0, 3),
             ("906", 0, 3), ("609", 0, 3), ("7b", 0, 3), ("4038", 0, 3), ("87", 1, 1), ("88", 1, 1), ("5", 1, 1)]
NOT_MIC_PREFIX = ("oa", "pos", "pre")   # off-axis angle, position index and preamp markers, never a microphone
ZONES = {"cap": 0.0, "center": 0.0, "centre": 0.0, "dustcap": 0.0, "capedge": 1.0, "cap-edge": 1.0, "cap_edge": 1.0, "cap edge": 1.0, "edge of cap": 1.0, "cone": 2.0, "mid": 2.0,
         "midcone": 2.0, "cone-edge": 3.0, "coneedge": 3.0, "edge": 3.0, "surround": 3.0}   # ordered zones: an index, not a distance
_NUM = r"(\d+(?:[.,]\d+)?)"
_LEN = re.compile(_NUM + r"\s*(in|inch|inches|\"|''|mm|cm)(?![a-z])", re.I)
_ANG = re.compile(r"(?:\bOA\s*|off[\s_-]*axis[\s_-]*)" + _NUM + r"|" + _NUM + r"\s*(?:deg|°)", re.I)
_SPK = re.compile(r"(?<![A-Za-z])(LL|LR|UL|UR|TL|TR|BL|BR)(?![A-Za-z])")
_LETTER = re.compile(r"[\s_-]([A-J])(?:[\s_.-]|$)")
_INDEX = re.compile(r"(?:pos(?:ition)?|bright(?:ness)?|#)[\s_-]*(\d{1,2})", re.I)


def _mm(value, unit):
    v = float(value.replace(",", "."))
    u = unit.lower()
    return v * INCH if u in ("in", "inch", "inches", '"', "''") else v * 10.0 if u == "cm" else v


def mic_token(low):
    """The microphone token in a lower-case name (see MIC_CORES), or None. The longest token wins, then the earlier
    core in MIC_CORES, then the earlier position."""
    best, best_key = None, None
    for p in range(len(low)):
        if p > 0 and ("0" <= low[p - 1] <= "9" or "a" <= low[p - 1] <= "z"):
            continue
        k = 0
        while p + k < len(low) and "a" <= low[p + k] <= "z":
            k += 1
        if k > 3 or low[p:p + k] in NOT_MIC_PREFIX:
            continue
        for rank, (core, fewest, most) in enumerate(MIC_CORES):
            e = p + k + len(core)
            if fewest <= k <= most and low.startswith(core, p + k) and not (e < len(low) and "0" <= low[e] <= "9"):
                key = (e - p, -rank, -p)
                if best_key is None or key > best_key:
                    best, best_key = low[p:e], key
                break
    return best


def parse_name(name, mapping=None):
    """Position metadata from one file name. Returns a dict with any of:
        mic, speaker, x_mm (from the cap centre), dist_mm (off the grille), angle_deg, zone (ordered index),
        step (index along an undocumented ordered path, e.g. letters A..E), variant (anything left: preamp, take)
    and 'source': 'mapping' | 'scheme' | 'tokens'. A user mapping wins over everything.
    mapping: dict as read from a pack's mapping file (see MAPPING_EXAMPLE)."""
    stem = Path(name).stem
    m = mapping or {}
    if stem in m.get("files", {}) or name in m.get("files", {}):
        return dict(m["files"].get(name, m["files"].get(stem)), source="mapping")
    for sch in m.get("schemes", []):                      # user regex with named groups; units given per group
        g = re.search(sch["regex"], stem)
        if g:
            d = {k: v for k, v in g.groupdict().items() if v is not None}
            out = dict(source="scheme")
            for k, v in d.items():
                if k in ("x", "dist"):
                    out[k + "_mm"] = _mm(v, sch.get("units", {}).get(k, "in"))
                elif k == "angle":
                    out["angle_deg"] = float(v)
                elif k == "step":
                    order = sch.get("order")
                    out["step"] = order.index(v) if order else (ord(v.upper()) - 65 if v.isalpha() else int(v))
                else:
                    out[k] = v
            return out
    out = dict(source="tokens")
    low = stem.lower()
    lens = [(_mm(a, u), g.start()) for g in _LEN.finditer(stem) for a, u in [g.groups()]]
    if len(lens) >= 2:                                    # two lengths: across the cone first, then off the grille
        out["x_mm"], out["dist_mm"] = lens[0][0], lens[1][0]
    elif len(lens) == 1:                                  # one length: it is a distance unless the name says "from cap/centre"
        key = "x_mm" if re.search(r"from[\s_-]*(cap|cent)", low) else "dist_mm"
        out[key] = lens[0][0]
    a = _ANG.search(stem)
    if a:
        out["angle_deg"] = float((a.group(1) or a.group(2)).replace(",", "."))
    s = _SPK.search(stem)
    if s:
        out["speaker"] = s.group(1)
    w = mic_token(low)
    if w:
        out["mic"] = w
    for z in sorted(ZONES, key=len, reverse=True):
        if z in low:
            out["zone"] = ZONES[z]
            break
    i = _INDEX.search(stem)
    l = _LETTER.search(" " + stem + " ")
    if i:
        out["step"] = int(i.group(1))
    elif l and "x_mm" not in out:
        out["step"] = ord(l.group(1)) - 65
    return out


MAPPING_EXAMPLE = {
    "_comment": "Put this next to a pack as magmora-positions.json. 'schemes' are tried in order on each file name (without .wav); "
                "named groups: mic, speaker, x (across the cone from the cap centre), dist (off the grille), angle, step (an ordered, "
                "undocumented path), variant. 'files' overrides single files. 'phase': 'minimum' | 'aligned' | 'raw'.",
    "phase": "minimum",
    "schemes": [{"regex": r"^SPK (?P<speaker>LL|LR|UL|UR) .* (?P<mic>[A-Za-z]{0,3}57) (?P<x>[0-9.]+)in (?P<dist>[0-9.]+)in(?: OA(?P<angle>[0-9]+))? (?P<variant>\w+)$",
                 "units": {"x": "in", "dist": "in"}},
                {"regex": r"-(?P<mic>[0-9A-Za-z]+)-(?P<step>[A-E])$", "order": ["A", "B", "C", "D", "E"]}],
    "files": {"my favourite.wav": {"mic": "57", "x_mm": 19.0, "dist_mm": 0.0}},
}


def build_grid(names, mapping=None):
    """Group a pack's files into microphone 'tracks': {(mic, speaker, variant, angle): sorted list of (coordinate, name)}.
    The coordinate is x_mm where documented, else the zone index, else the step along an ordered path; the kind is
    returned too, because an undocumented path can only be offered as 0..1 'brightness', never as millimetres."""
    tracks = {}
    for n in names:
        p = parse_name(n, mapping)
        kind = "x_mm" if "x_mm" in p else "zone" if "zone" in p else "step" if "step" in p else None
        if kind is None:
            continue
        key = (p.get("mic", "?"), p.get("speaker", "?"), p.get("variant", ""), float(p.get("angle_deg", 0.0)), float(p.get("dist_mm", 0.0)), kind)
        tracks.setdefault(key, []).append((float(p[kind]), n))
    return {k: sorted(v) for k, v in tracks.items()}


def bracket(coords, x):
    """Indices of the two recorded coordinates around x and the fraction between them (clamped at the ends)."""
    c = np.asarray(coords, float)
    if x <= c[0]:
        return 0, 0, 0.0
    if x >= c[-1]:
        return len(c) - 1, len(c) - 1, 0.0
    j = int(np.searchsorted(c, x, side="right"))
    return j - 1, j, float((x - c[j - 1]) / (c[j] - c[j - 1]))


# ----------------------------------------------------------------------------- 2. interpolation
def _minimum_phase(mag, n):
    """Minimum-phase impulse response (length n) with magnitude `mag` given on rfft bins of an nfft-point transform."""
    nfft = 2 * (len(mag) - 1)
    full = np.concatenate([mag, mag[-2:0:-1]])
    c = np.fft.ifft(np.log(np.maximum(full, 1e-9 * full.max()))).real
    w = np.zeros(nfft); w[0] = 1; w[1:nfft // 2] = 2; w[nfft // 2] = 1
    return np.fft.ifft(np.exp(np.fft.fft(c * w))).real[:n]


def _onset(h, fs):
    """Arrival time in samples (sub-sample): centre of energy of the first 0.5 ms after 10 % of the envelope peak."""
    e = np.abs(h)
    i = int(np.argmax(e > 0.1 * e.max()))
    j = i + max(int(0.0005 * fs), 2)
    return float(np.sum(np.arange(i, j) * e[i:j] ** 2) / (np.sum(e[i:j] ** 2) + 1e-30))


def _shift(h, d):
    n = 2 * len(h)
    return np.fft.irfft(np.fft.rfft(h, n) * np.exp(-2j * np.pi * np.fft.rfftfreq(n) * d), n)[: len(h)]


LOGF_PER_OCT = 96


def _log_axis(fs, nfft, lo=60.0):
    f = np.fft.rfftfreq(nfft, 1.0 / fs)
    u = np.arange(0, int(np.log2(0.49 * fs / lo) * LOGF_PER_OCT) + 1) / LOGF_PER_OCT
    return f, lo * 2 ** u


def _db_on_log(mag, f, fl):
    """Level in dB on the log axis: power averaged over the spacing of the axis (so narrow notches keep their depth)."""
    c = np.concatenate([[0.0], np.cumsum(mag ** 2)])
    e = fl * 2 ** (0.5 / LOGF_PER_OCT)
    b = fl * 2 ** (-0.5 / LOGF_PER_OCT)
    lo = np.searchsorted(f, b); hi = np.maximum(np.searchsorted(f, e), lo + 1)
    return 10 * np.log10((c[np.minimum(hi, len(mag))] - c[lo]) / (hi - lo) + 1e-30)


def _smooth(x, n):
    k = np.hanning(2 * n + 1); k /= k.sum()
    return np.convolve(np.pad(x, n, mode="edge"), k, mode="valid")


def warp_path(a_db, b_db, max_shift=LOGF_PER_OCT // 6, stiffness=0.02, trend=LOGF_PER_OCT // 2):
    """Monotonic alignment of two level curves on the log-frequency axis (dynamic programming). Matches peaks and
    notches of the detail (the curve minus its half-octave trend), allowing each feature to move by at most
    max_shift points (1/6 octave). Returns index arrays (i, j): point i of a corresponds to point j of b."""
    da, dbb = a_db - _smooth(a_db, trend), b_db - _smooth(b_db, trend)
    n = len(da)
    W = max_shift
    INF = 1e18
    D = np.full((n, 2 * W + 1), INF)                       # D[i, k]: a[i] matched to b[i + k - W]
    back = np.zeros((n, 2 * W + 1), np.int8)
    ks = np.arange(-W, W + 1)
    def local(i):
        j = i + ks
        ok = (j >= 0) & (j < n)
        c = np.full(2 * W + 1, INF)
        c[ok] = (da[i] - dbb[j[ok]]) ** 2 + stiffness * ks[ok] ** 2
        return c
    D[0] = local(0)
    for i in range(1, n):
        c = local(i)
        # b index may advance by 0, 1 or 2 per step of a: shift k changes by -1, 0 or +1
        prev = np.stack([np.concatenate([D[i - 1][1:], [INF]]), D[i - 1], np.concatenate([[INF], D[i - 1][:-1]])])   # from k+1, k, k-1
        m = np.argmin(prev, axis=0)
        D[i] = c + prev[m, np.arange(2 * W + 1)]
        back[i] = m
    k = int(np.argmin(D[-1]))
    path = np.zeros(n, int)
    for i in range(n - 1, -1, -1):
        path[i] = k
        k = int(np.clip(k + (1 - back[i][k]), 0, 2 * W))
    return np.arange(n), np.clip(np.arange(n) + path - W, 0, n - 1)


def interpolate(a, b, t, fs, method="morph", minimum_phase=True, nfft=None):
    """Response a fraction t of the way from recorded position a to recorded position b.

      snap     the nearer file, untouched
      linear   (1 - t) a + t b after removing the arrival-time difference: what a crossfader does; notches of a and b
               both survive at reduced depth and new ones appear where the two cancel
      logmag   level in dB interpolated bin by bin, minimum phase: no cancellation, but a notch that moves between
               a and b becomes two shallow ones (a morph into a smooth EQ)
      morph    features (peaks, notches) are matched along the log-frequency axis and moved as well as faded;
               the broad shape is the dB interpolation; minimum phase
    For non-minimum-phase packs (minimum_phase=False) the result is the minimum-phase response delayed by the
    interpolated arrival time, so that the time of flight still changes smoothly with position."""
    a, b = np.asarray(a, float), np.asarray(b, float)
    n = max(len(a), len(b))
    if method == "snap" or t <= 0.0 or t >= 1.0:
        return (a if t < 0.5 else b).copy()
    ta, tb = _onset(a, fs), _onset(b, fs)
    if method == "linear":
        return (1 - t) * np.pad(a, (0, n - len(a))) + t * _shift(np.pad(b, (0, n - len(b))), ta - tb)
    nfft = nfft or int(2 ** np.ceil(np.log2(2 * n)))
    A, B = np.abs(np.fft.rfft(a, nfft)), np.abs(np.fft.rfft(b, nfft))
    plain = np.exp((1 - t) * np.log(A + 1e-12) + t * np.log(B + 1e-12))
    if method == "logmag":
        mag = plain
    elif method == "morph":
        f, fl = _log_axis(fs, nfft)
        la, lb = _db_on_log(A, f, fl), _db_on_log(B, f, fl)
        i, j = warp_path(la, lb)
        u = (1 - t) * i + t * j                               # where each matched feature sits at fraction t
        v = (1 - t) * la[i] + t * lb[j]
        o = np.argsort(u)
        lt = np.interp(np.arange(len(fl)), u[o], v[o])
        # the morph supplies the detail on the log axis; within one axis step and below its start the bin-wise
        # interpolation is kept (it carries the resolution finer than 1/96 octave)
        fine = 20 * np.log10(plain + 1e-12) - np.interp(f, fl, (1 - t) * la + t * lb, left=np.nan, right=np.nan)
        mag_db = np.interp(f, fl, lt, left=np.nan, right=np.nan) + fine
        mag = np.where(np.isnan(mag_db), plain, 10 ** (np.nan_to_num(mag_db) / 20))
    else:
        raise ValueError(method)
    h = _minimum_phase(mag, n)
    if not minimum_phase:
        h = _shift(h, (1 - t) * ta + t * tb - _onset(h, fs))
    return h


def apply_trend(h, f_pts, db_pts, fs):
    """Multiply a response by a smooth level curve (dB at f_pts, flat beyond its ends), keeping it minimum phase.
    Used for the physics-informed correction: what a position model says the broad level does between two recorded
    positions, beyond a straight line in dB."""
    nfft = int(2 ** np.ceil(np.log2(2 * len(h))))
    f = np.fft.rfftfreq(nfft, 1.0 / fs)
    g = np.interp(np.log(np.maximum(f, 1.0)), np.log(f_pts), db_pts)
    return _minimum_phase(np.abs(np.fft.rfft(h, nfft)) * 10 ** (g / 20), len(h))


def interpolate_2d(grid, x, d, fs, method="morph", minimum_phase=True):
    """grid: {(x_coord, dist_coord): response}. Interpolates across the cone at the two bracketing grille distances,
    then between those. Missing corners fall back to the nearest recorded distance."""
    xs = sorted({k[0] for k in grid}); ds = sorted({k[1] for k in grid})
    i0, i1, tx = bracket(xs, x)
    j0, j1, td = bracket(ds, d)
    rows = []
    for j in (j0, j1):
        a, b = grid.get((xs[i0], ds[j])), grid.get((xs[i1], ds[j]))
        if a is None or b is None:
            rows.append(a if a is not None else b)
        else:
            rows.append(interpolate(a, b, tx, fs, method, minimum_phase))
    if rows[0] is None or rows[1] is None or j0 == j1:
        return rows[0] if rows[0] is not None else rows[1]
    return interpolate(rows[0], rows[1], td, fs, method, minimum_phase)


# ----------------------------------------------------------------------------- 3. two microphones
def relative_delay(a, b, fs, up=8, max_ms=10.0):
    """(samples by which a trails b, polarity of a against b), from the upsampled cross-correlation peak."""
    n = int(2 ** np.ceil(np.log2(len(a) + len(b))))
    X = np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n))
    Xu = np.zeros(n * up // 2 + 1, complex); Xu[: len(X)] = X
    c = np.fft.irfft(Xu)
    m = int(max_ms * 1e-3 * fs * up)
    w = np.concatenate([c[-m:], c[: m + 1]])
    k = int(np.argmax(np.abs(w)))
    return (k - m) / up, int(np.sign(w[k]))


def pan_gains(pan):
    """Constant-power pan, -1..1 -> (left, right); centre is -3 dB each."""
    th = (pan + 1) * np.pi / 4
    return float(np.cos(th)), float(np.sin(th))


def two_mics(h1, h2, fs, level1_db=0.0, level2_db=0.0, pan1=0.0, pan2=0.0, invert1=False, invert2=False,
             align=True, offset_mm=0.0, pack_phase="raw"):
    """Stereo response (2, n) of two microphones.

    align: microphone 2 is moved onto microphone 1 in time. For 'minimum' and 'aligned' packs nothing is measured or
    moved (their files already share a time origin; estimating a delay from minimum-phase files would invent one).
    For 'raw' packs the arrival difference is estimated by cross-correlation and removed with sub-sample accuracy.
    Polarity is never changed automatically; the detected relative polarity is returned so that a UI can show it.
    offset_mm then moves microphone 2 away (positive) or toward the source by that path length."""
    n = max(len(h1), len(h2))
    a = np.pad(np.asarray(h1, float), (0, n - len(h1))); b = np.pad(np.asarray(h2, float), (0, n - len(h2)))
    d, pol = (0.0, 1)
    if pack_phase == "raw":
        d, pol = relative_delay(b, a, fs)
    move = (-d if (align and pack_phase == "raw") else 0.0) + offset_mm * 1e-3 / C0 * fs
    if abs(move) > 1e-9:
        b = _shift(np.pad(b, (0, 64)), move)[:n] if move > 0 else _shift(b, move)
    g1 = 10 ** (level1_db / 20) * (-1 if invert1 else 1); g2 = 10 ** (level2_db / 20) * (-1 if invert2 else 1)
    (l1, r1), (l2, r2) = pan_gains(pan1), pan_gains(pan2)
    out = np.stack([g1 * l1 * a + g2 * l2 * b, g1 * r1 * a + g2 * r2 * b])
    return out, dict(measured_delay_samples=d, detected_polarity=pol, moved_samples=move)
