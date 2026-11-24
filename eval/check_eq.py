"""Gate G-E1: the EQ against its analogue prototype.

    source scripts/env.sh && $BLOCKAMP_PY eval/check_eq.py [--rates 44100,48000,...] [--only PATTERN] [--json OUT]

For every band at its frequency, gain and Q limits (and the high-pass and low-pass at their frequency limits and
both slopes), the impulse response of the C++ EQ (dsp/tools/magmora-eq) is compared with the analogue prototype
from 20 Hz to 20 kHz: magnitude within 0.1 dB, phase within 2 degrees. The EQ is measured where the chain runs it,
at the head's internal rate for that host rate and quality (D36). The prototypes are written out here in
closed form, independently of the C++ design code (dsp/src/eq/Design.cpp). Every other band sits at 0 dB, which
is an exact identity in the state-variable form.
"""
import argparse, fnmatch, json, os, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(os.environ.get('BLOCKAMP_PRIVATE', ''), 'build/dsp/dsp/tools/magmora-eq')
GATE_DB, GATE_DEG = 0.1, 2.0
RATES = [44100, 48000, 88200, 96000, 192000]
BUTTER2 = [1 / np.sqrt(2)]
BUTTER4 = [0.5411961001461970, 1.3065629648763766]
# band: (key prefix, frequency limits, has a type switch)
BANDS = [('eq_b1', (30.0, 500.0), 'low'), ('eq_b2', (100.0, 2000.0), None), ('eq_b3', (400.0, 8000.0), None),
         ('eq_b4', (1000.0, 16000.0), 'high')]


def section(kind, f0, gain_db, q, f):
    """Analogue second-order section at frequencies f (Hz)."""
    s = 1j * f / f0
    a = 10 ** (gain_db / 40)
    if kind == 'hp':
        return s * s / (s * s + s / q + 1)
    if kind == 'lp':
        return 1 / (s * s + s / q + 1)
    if kind == 'bell':
        return (s * s + s * a / q + 1) / (s * s + s / (a * q) + 1)
    if kind == 'lowshelf':
        return a * (s * s + np.sqrt(a) / q * s + a) / (a * s * s + np.sqrt(a) / q * s + 1)
    if kind == 'highshelf':
        return a * (a * s * s + np.sqrt(a) / q * s + 1) / (s * s + np.sqrt(a) / q * s + a)
    raise ValueError(kind)


def cases():
    out = []
    for slope in (1, 2):
        for f0 in (20.0, 1000.0):
            out.append((f'hp_{f0:g}_{12 * slope}', {'eq_hp_freq': f0, 'eq_hp_slope': slope},
                        [('hp', f0, 0, q) for q in (BUTTER2 if slope == 1 else BUTTER4)]))
        for f0 in (1000.0, 20000.0):
            out.append((f'lp_{f0:g}_{12 * slope}', {'eq_lp_freq': f0, 'eq_lp_slope': slope},
                        [('lp', f0, 0, q) for q in (BUTTER2 if slope == 1 else BUTTER4)]))
    for key, (lo, hi), shelf in BANDS:
        for kind in (['bell'] + ([shelf + 'shelf'] if shelf else [])):
            for f0 in (lo, hi):
                for g in (-15.0, 15.0):
                    for q in (0.3, 10.0):
                        settings = {key + '_freq': f0, key + '_gain': g, key + '_q': q}
                        if shelf:
                            settings[key + '_type'] = 0 if kind == 'bell' else 1
                        out.append((f'{key}_{kind}_{f0:g}_{g:+g}_q{q:g}', settings, [(kind, f0, g, q)]))
    return out


def engine_response(rate, settings, quality, seconds=4.0):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'ir.f64')
        args = [TOOL, '--rate', str(rate), '--quality', quality, '--seconds', str(seconds), '--out', path, '--set'] + \
            [f'{k}={v}' for k, v in settings.items()]
        rate = float(subprocess.run(args, check=True, capture_output=True, text=True, timeout=60).stdout)
        h = np.fromfile(path, dtype='<f8')
        n = len(h)
    if np.abs(h[-n // 8:]).max() > 1e-9 * np.abs(h).max():
        raise RuntimeError('impulse response not decayed within the window')
    spectrum = np.fft.rfft(h)
    bins = np.fft.rfftfreq(n, 1 / rate)
    return bins, spectrum


def check(rate, name, settings, sections, quality):
    bins, h = engine_response(rate, settings, quality)
    grid = np.unique(np.round(np.geomspace(20, 20000, 400) / (bins[1] - bins[0])).astype(int))
    f = bins[grid]
    a = np.ones(len(f), dtype=complex)
    for s in sections:
        a *= section(*s, f)
    ratio = h[grid] / a
    mag = 20 * np.log10(np.abs(ratio))
    phase = np.degrees(np.angle(ratio))
    i, j = np.argmax(np.abs(mag)), np.argmax(np.abs(phase))
    return dict(rate=rate, quality=quality, case=name, mag_db=float(np.abs(mag[i])), mag_at=float(f[i]), phase_deg=float(np.abs(phase[j])),
                phase_at=float(f[j]), ok=bool(np.abs(mag[i]) <= GATE_DB and np.abs(phase[j]) <= GATE_DEG))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rates', default=','.join(map(str, RATES)))
    ap.add_argument('--only', default='*')
    ap.add_argument('--quality', default='standard')
    ap.add_argument('--json', default=None)
    a = ap.parse_args()
    rows = []
    for rate in map(int, a.rates.split(',')):
        for name, settings, sections in cases():
            if not fnmatch.fnmatch(name, a.only):
                continue
            r = check(rate, name, settings, sections, a.quality)
            rows.append(r)
            print(f"{rate:6d} {name:32s} |mag| {r['mag_db']:7.3f} dB at {r['mag_at']:7.0f} Hz  |phase| {r['phase_deg']:6.2f} deg"
                  f" at {r['phase_at']:7.0f} Hz  {'ok' if r['ok'] else 'FAIL'}", flush=True)
    bad = [r for r in rows if not r['ok']]
    for rate in sorted({r['rate'] for r in rows}):
        rr = [r for r in rows if r['rate'] == rate]
        print(f"rate {rate}: {sum(r['ok'] for r in rr)}/{len(rr)} pass; worst magnitude {max(r['mag_db'] for r in rr):.3f} dB, "
              f"worst phase {max(r['phase_deg'] for r in rr):.2f} deg")
    print(f"{a.quality}: {len(rows)} cases, {len(bad)} failing (gate {GATE_DB} dB, {GATE_DEG} deg, 20 Hz to 20 kHz)")
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=1)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
