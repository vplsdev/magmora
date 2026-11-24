"""Shared helpers for the evaluation harness: private-asset paths, audio IO and the capture-match metric."""
import json, os
from pathlib import Path
import numpy as np
import soundfile as sf
from scipy.signal import stft, welch, fftconvolve

SR = 48000
ASSETS = Path(os.environ.get("BLOCKAMP_ASSETS", ""))
PRIVATE = Path(os.environ.get("BLOCKAMP_PRIVATE", "."))
WORK = PRIVATE / "work"
IR_PATH = Path(os.environ.get("BLOCKAMP_IR", ""))
_MAP_FILE = Path(os.environ.get("BLOCKAMP_ASSET_MAP", Path(__file__).with_name("local_assets.json")))
ASSET_MAP = json.loads(_MAP_FILE.read_text()) if _MAP_FILE.exists() else {}


def asset(key, sub=None):
    """Path of a private asset named in eval/local_assets.json (never committed)."""
    v = ASSET_MAP[key] if sub is None else ASSET_MAP[key][sub]
    return Path(v) if os.path.isabs(v) else ASSETS / v


BANDS = [(40, 100), (100, 200), (200, 400), (400, 800), (800, 1600), (1600, 3200), (3200, 6400), (6400, 12000)]


def read(p):
    x, sr = sf.read(str(p), dtype="float64", always_2d=True)
    assert sr == SR, (p, sr)
    return x[:, 0]


def write(p, x):
    Path(p).parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(p), np.asarray(x, dtype=np.float32), SR, subtype="FLOAT")


def cab(x, ir=None):
    ir = read(IR_PATH) if ir is None else ir
    return fftconvolve(x, ir)[: len(x)]


def specs(x):
    """Multi-resolution log-magnitude STFTs of the RMS-normalised signal."""
    x = x / (np.sqrt(np.mean(x ** 2)) + 1e-9)
    out = []
    for n in (512, 2048, 8192):
        _, _, Z = stft(x, SR, nperseg=n, noverlap=n * 3 // 4)
        f = np.fft.rfftfreq(n, 1 / SR)
        out.append(20 * np.log10(np.abs(Z[f < 16000]) + 1e-5))
    return out


def mr_loss(a, b):
    """Mean absolute dB difference of multi-resolution spectrograms, floor -80 dB. Lower is better."""
    n = min(len(a), len(b))
    return float(np.mean([np.mean(np.abs(np.maximum(x, -80) - np.maximum(y, -80)))
                          for x, y in zip(specs(a[:n]), specs(b[:n]))]))


def band_diff(x, ref):
    """Long-term spectrum difference (dB) per band after RMS normalisation."""
    n = min(len(x), len(ref))
    f, X = welch(x[:n] / np.sqrt(np.mean(x[:n] ** 2)), SR, nperseg=8192)
    _, R = welch(ref[:n] / np.sqrt(np.mean(ref[:n] ** 2)), SR, nperseg=8192)
    d = 10 * np.log10(X + 1e-12) - 10 * np.log10(R + 1e-12)
    return [float(np.mean(d[(f >= a) & (f < b)])) for a, b in BANDS]


def busiest_window(di, seconds):
    """Start index of the window with the most input energy (the commercial matches were fitted the same way)."""
    win = int(seconds * SR)
    e = np.cumsum(np.concatenate([[0.0], di ** 2]))
    starts = np.arange(0, len(di) - win, SR // 4)
    return int(starts[np.argmax(e[starts + win] - e[starts])])
