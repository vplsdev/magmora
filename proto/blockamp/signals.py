"""Deterministic test signals shared by the SPICE reference generator and the engine tests."""
import numpy as np


def sine(fs, f0, amp, dur, ramp=0.002):
    t = np.arange(int(round(dur * fs))) / fs
    return amp * np.sin(2 * np.pi * f0 * t) * np.minimum(1.0, t / ramp)


def burst(fs, f0=110.0, amp_hi=0.5, amp_lo=0.01, t_hi=0.12, t_lo=0.18, f_lo=440.0):
    """Loud burst followed by a quiet tone: shows blocking distortion (bias shift) and its recovery."""
    n1, n2 = int(t_hi * fs), int(t_lo * fs)
    t = np.arange(n1 + n2) / fs
    x = np.where(np.arange(n1 + n2) < n1, amp_hi * np.sin(2 * np.pi * f0 * t), 0.0)
    return x + amp_lo * np.sin(2 * np.pi * f_lo * t)


def chord(fs, amp=0.4, dur=0.4, freqs=(82.41, 123.47, 164.81, 246.94)):
    """Sustained power chord with a 5 ms attack: loads the supply (sag) and the output stage."""
    t = np.arange(int(dur * fs)) / fs
    x = sum(np.sin(2 * np.pi * f * t + 0.7 * k) for k, f in enumerate(freqs)) / len(freqs)
    return amp * x * np.minimum(1.0, t / 0.005)


def chug(fs, amp=0.5, f0=82.41, rate=8.0, dur=0.5, decay=0.03):
    """Palm-mute style pulse train: decaying low-E bursts (low-end tightness and recovery between notes)."""
    t = np.arange(int(dur * fs)) / fs
    ph = (t * rate) % 1.0 / rate
    env = np.exp(-ph / decay)
    return amp * env * np.sign(np.sin(2 * np.pi * f0 * t)) * 0.5 + amp * env * np.sin(2 * np.pi * f0 * t) * 0.5
