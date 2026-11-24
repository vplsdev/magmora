"""Cabinet metrics. Pure functions on impulse responses; no file access.

Every comparison is made after the free alignments the reference's processing makes meaningless: one level and one
delay per shot. Smoothing is power averaging over a fractional-octave window, the same procedure that produced the
real-to-real yardsticks, so model-to-real and real-to-real numbers are directly comparable.
"""
import numpy as np
from scipy.signal import butter, sosfiltfilt

BANDS = [(70.0, 1000.0), (1000.0, 5000.0), (5000.0, 10000.0)]
NFFT_AT_96K = 1 << 17


def nfft(fs):
    """FFT length with the same bin spacing in hertz at every rate (0.73 Hz)."""
    return int(round(NFFT_AT_96K * fs / 96000.0 / 2)) * 2


def spectrum(ir, fs):
    n = nfft(fs)
    return np.fft.rfftfreq(n, 1.0 / fs), np.fft.rfft(np.asarray(ir, float)[:n], n)


def smooth_db(ir, fs, frac):
    """Power spectrum averaged over a 1/frac-octave window centred on every bin, in dB."""
    f, H = spectrum(ir, fs)
    c = np.cumsum(np.abs(H) ** 2)
    lo = np.searchsorted(f, f * 2 ** (-0.5 / frac))
    hi = np.minimum(np.maximum(np.searchsorted(f, f * 2 ** (0.5 / frac)), lo + 1), len(f) - 1)
    return f, 10 * np.log10((c[hi] - c[lo]) / np.maximum(hi - lo, 1) + 1e-30)


def difference_curve(a, b, fs, frac=3, lo=70.0, hi=10000.0, points=800):
    """Level-aligned smoothed difference a - b on a log frequency axis (each octave counts equally)."""
    f, sa = smooth_db(a, fs, frac)
    _, sb = smooth_db(b, fs, frac)
    fl = np.geomspace(lo, hi, points)
    d = np.interp(fl, f, sa - sb)
    return fl, d - d.mean()


def band_distance(a, b, fs, frac=3):
    """[(rms dB, worst dB)] in the three gate ranges, after one level alignment over 70 Hz to 10 kHz."""
    fl, d = difference_curve(a, b, fs, frac)
    return [(float(np.sqrt(np.mean(d[(fl >= lo) & (fl < hi)] ** 2))), float(np.abs(d[(fl >= lo) & (fl < hi)]).max()))
            for lo, hi in BANDS]


def trend_curve(bright, dark, fs, frac=6, lo=1000.0, hi=10000.0, points=300):
    """Position trend: smoothed level of the brighter shot minus the darker one, 1 to 10 kHz, log axis."""
    f, sa = smooth_db(bright, fs, frac)
    _, sb = smooth_db(dark, fs, frac)
    fl = np.geomspace(lo, hi, points)
    return fl, np.interp(fl, f, sa - sb)


def trend_compare(t_model, t_real, fl):
    """Correlation of two trend curves and the difference of their overall tilts (dB over the 1 to 10 kHz span)."""
    x = np.log10(fl / fl[0]) / np.log10(fl[-1] / fl[0])
    tilt = lambda t: float(np.polyfit(x, t, 1)[0])
    return dict(correlation=float(np.corrcoef(t_model, t_real)[0, 1]), tilt_model=tilt(t_model), tilt_real=tilt(t_real),
                tilt_error=tilt(t_model) - tilt(t_real))


def fractional_delay(ir, delay_samples):
    """Shift by a real number of samples (circular in a zero-padded frame of twice the length)."""
    n = 2 * len(ir)
    H = np.fft.rfft(ir, n) * np.exp(-2j * np.pi * np.fft.rfftfreq(n) * delay_samples)
    return np.fft.irfft(H, n)[: len(ir)]


def relative_delay(a, b, fs, up=8):
    """(delay of a after b in samples, polarity +1/-1, normalised correlation peak), by upsampled cross-correlation."""
    n = nfft(fs)
    X = np.fft.rfft(a[:n], n) * np.conj(np.fft.rfft(b[:n], n))
    Xu = np.zeros(n * up // 2 + 1, complex)
    Xu[: len(X)] = X
    c = np.fft.irfft(Xu)
    k = int(np.argmax(np.abs(c)))
    lag = k if k < len(c) // 2 else k - len(c)
    peak = abs(c[k]) * up / np.sqrt(np.sum(np.square(a[:n])) * np.sum(np.square(b[:n])) + 1e-30)
    return lag / up, int(np.sign(c[k])), float(peak)


def align(a, ref, fs):
    """a moved onto ref in time (sub-sample) and polarity; level untouched. Returns (aligned, delay, polarity)."""
    d, pol, _ = relative_delay(a, ref, fs)
    return pol * fractional_delay(a, -d), d, pol


def blend(a, b, fs, aligned=True):
    """Equal sum 0.5*(a+b), b aligned onto a first when asked."""
    n = min(len(a), len(b))
    a, b = np.asarray(a[:n], float), np.asarray(b[:n], float)
    if aligned:
        b = align(b, a, fs)[0]
    return 0.5 * (a + b)


def third_octave_centres(lo=80.0, hi=8000.0):
    k = np.arange(np.ceil(3 * np.log2(lo / 1000.0)), np.floor(3 * np.log2(hi / 1000.0)) + 1)
    return 1000.0 * 2 ** (k / 3.0)


def band_levels(ir, fs, centres, frac=3):
    f, H = spectrum(ir, fs)
    P = np.abs(H) ** 2
    return np.array([10 * np.log10(P[(f >= c * 2 ** (-0.5 / frac)) & (f < c * 2 ** (0.5 / frac))].mean() + 1e-30) for c in centres])


def blend_coherence(a, b, fs, centres=None):
    """Level of the aligned equal sum relative to the power sum of its parts, per 1/3-octave band (dB; +3 = in phase)."""
    centres = third_octave_centres() if centres is None else centres
    n = min(len(a), len(b))
    a = np.asarray(a[:n], float)
    b = align(np.asarray(b[:n], float), a, fs)[0]
    s = band_levels(a + b, fs, centres)
    pw = 10 * np.log10(10 ** (band_levels(a, fs, centres) / 10) + 10 ** (band_levels(b, fs, centres) / 10))
    return centres, s - pw


def minimum_phase(ir, fs):
    """Minimum-phase response with the same magnitude (real cepstrum folding)."""
    n = 4 * nfft(fs)
    mag = np.maximum(np.abs(np.fft.fft(ir, n)), 1e-9 * np.abs(np.fft.fft(ir, n)).max())
    c = np.fft.ifft(np.log(mag)).real
    w = np.zeros(n)
    w[0] = 1.0
    w[1: n // 2] = 2.0
    w[n // 2] = 1.0
    return np.fft.ifft(np.exp(np.fft.fft(c * w))).real[: len(ir)]


def _group_delay_bands(ir, fs, centres):
    """Energy-weighted group delay (ms) per octave band."""
    n = nfft(fs)
    x = np.asarray(ir, float)[:n]
    H = np.fft.rfft(x, n)
    D = np.fft.rfft(x * np.arange(len(x)), n)
    f = np.fft.rfftfreq(n, 1.0 / fs)
    P = np.abs(H) ** 2
    gd = np.real(D * np.conj(H)) / (P + 1e-30) / fs * 1e3
    out = []
    for c in centres:
        m = (f >= c / np.sqrt(2)) & (f < c * np.sqrt(2))
        out.append(np.sum(gd[m] * P[m]) / np.sum(P[m]))
    return np.array(out)


EGD_CENTRES = np.array([250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0])


def excess_group_delay(ir, fs, centres=EGD_CENTRES):
    """Group delay minus that of the minimum-phase response of equal magnitude, per octave band (ms), with the
    median over bands removed (the bulk delay of a shot is free)."""
    e = _group_delay_bands(ir, fs, centres) - _group_delay_bands(minimum_phase(ir, fs), fs, centres)
    return centres, e - np.median(e)


EDC_CENTRES = np.array([125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0])


def decay_curves(ir, fs, centres=EDC_CENTRES, span_ms=40.0, step_ms=1.0):
    """Backward-integrated energy decay per octave band, in dB on a 1 ms grid, counted from the arrival of the
    direct sound (0.5 ms before the broadband peak) and normalised to the band's energy from there on."""
    t = np.arange(0.0, span_ms + 1e-9, step_ms)
    out = []
    x = np.concatenate([np.zeros(int(0.02 * fs)), np.asarray(ir, float)])
    p = max(int(np.argmax(np.abs(x))) - int(0.0005 * fs), 0)
    for c in centres:
        sos = butter(4, [c / np.sqrt(2), min(c * np.sqrt(2), 0.49 * fs)], btype="band", fs=fs, output="sos")
        y = sosfiltfilt(sos, x)
        e = np.cumsum((y ** 2)[::-1])[::-1]
        d = 10 * np.log10(e / (e[p] + 1e-30) + 1e-30)
        idx = np.minimum(p + np.round(t * 1e-3 * fs).astype(int), len(d) - 1)
        out.append(d[idx])
    return t, np.array(out)


def decay_error(model, real, fs, floor_db=-30.0):
    """Worst difference (dB) between the two decay curves while the real one is above floor_db, per band."""
    _, dm = decay_curves(model, fs)
    _, dr = decay_curves(real, fs)
    dm = np.maximum(dm, -60.0)                     # numerical floor only: a response that has ended reads -60 dB
    return np.array([np.abs(m[r >= floor_db] - r[r >= floor_db]).max() for m, r in zip(dm, dr)])


def fine_structure(ir, fs, lo, hi, points_per_octave=200, prominence_db=1.0):
    """Statistics of the detail a 1/3-octave view loses (1/24-octave minus 1/3-octave level, log axis):
    (standard deviation dB, peak-to-peak dB, extrema per octave with at least prominence_db of swing)."""
    f, a = smooth_db(ir, fs, 24)
    _, b = smooth_db(ir, fs, 3)
    fl = np.geomspace(lo, hi, int(points_per_octave * np.log2(hi / lo)))
    d = np.interp(fl, f, a - b)
    count, hi_run, lo_run, direction = 0, d[0], d[0], 0       # turning points separated by a swing of prominence_db
    for v in d[1:]:
        hi_run, lo_run = max(hi_run, v), min(lo_run, v)
        if direction >= 0 and v < hi_run - prominence_db:     # a peak has been passed
            count += 1; direction = -1; lo_run = v
        elif direction <= 0 and v > lo_run + prominence_db:   # a notch has been passed
            count += 1; direction = 1; hi_run = v
    return float(d.std()), float(d.max() - d.min()), float(count / np.log2(hi / lo))


def decay_points(ir, fs, centres=(1000.0, 2000.0, 4000.0, 8000.0), times_ms=(5, 10, 20, 35)):
    """Energy-decay levels (dB) at a few times after the direct sound, per octave band: array (bands, times)."""
    t, d = decay_curves(ir, fs, centres=np.array(centres))
    return np.array([[row[t == x][0] for x in times_ms] for row in np.maximum(d, -60.0)])
