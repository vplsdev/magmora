"""The badge alphabet: our own wide, squared geometric capitals, as centre-line strokes.

Cap height 1, y up. Each entry is (advance, [strokes]). Drawn here from scratch, not traced.
"""
import math

T = 0.185          # stroke weight
H = T / 2
LO, HI = H, 1 - H
R = 0.27           # corner radius of the bowls


def _arc(cx, cy, r, start, stop, steps=12):
    return [(cx + r * math.cos(math.radians(start + (stop - start) * i / steps)),
             cy + r * math.sin(math.radians(start + (stop - start) * i / steps)))
            for i in range(steps + 1)]


def _c(w):
    right = w - H
    return ([(right + 0.01, HI)] + _arc(H + R, HI - R, R, 90, 180)
            + _arc(H + R, LO + R, R, 180, 270) + [(right + 0.01, LO)])


def _o(w):
    right = w - H
    return (_arc(H + R, HI - R, R, 90, 180) + _arc(H + R, LO + R, R, 180, 270)
            + _arc(right - R, LO + R, R, 270, 360) + _arc(right - R, HI - R, R, 0, 90)
            + [(H + R, HI)])


def _s(w):
    right = w - H
    r = (HI - LO) / 4
    mid = 0.5
    return ([(right, HI)] + _arc(H + r, HI - r, r, 90, 270)
            + _arc(right - r, mid - r, r, 90, -90) + [(H, LO)])


def _bowl(w, bottom):
    right = w - H
    r = (HI - bottom) / 2
    return [(H, HI)] + _arc(right - r, HI - r, r, 90, -90) + [(H, bottom)]


def _stem(x=H):
    return [(x, 0), (x, 1)]


ALPHABET = {
    " ": (0.45, []),
    "A": (1.10, [[(H, 0), (0.47, HI), (0.63, HI), (1.10 - H, 0)], [(0.30, 0.28), (0.80, 0.28)]]),
    "B": (0.96, [_stem(), _bowl(0.94, 0.5), [(H, 0.5)] + _arc(0.96 - H - 0.2, 0.3, 0.2, 90, -90) + [(H, LO)]]),
    "C": (1.04, [_c(1.04)]),
    "D": (1.02, [_stem(), _bowl(1.02, LO)]),
    "E": (0.90, [[(0.9, HI), (H, HI), (H, LO), (0.9, LO)], [(H, 0.5), (0.78, 0.5)]]),
    "F": (0.86, [[(0.86, HI), (H, HI), (H, 0)], [(H, 0.47), (0.74, 0.47)]]),
    "G": (1.08, [_c(1.08) + [(1.08 - H, 0.45), (0.62, 0.45)]]),
    "H": (1.00, [_stem(), _stem(1 - H), [(H, 0.5), (1 - H, 0.5)]]),
    "I": (T, [_stem()]),
    "J": (0.86, [[(0.86 - H, 1)] + _arc(0.86 - H - R, LO + R, R, 0, -90) + [(0, LO)]]),
    "K": (0.98, [_stem(), [(0.92, 1), (0.3, 0.5), (0.92, 0)]]),
    "L": (0.82, [[(H, 1), (H, LO), (0.82, LO)]]),
    "M": (1.20, [[(H, 0), (H, HI), (0.6, 0.25), (1.2 - H, HI), (1.2 - H, 0)]]),
    "N": (1.04, [[(H, 0), (H, HI), (1.04 - H, LO), (1.04 - H, 1)]]),
    "O": (1.14, [_o(1.14)]),
    "P": (0.96, [_stem(), _bowl(0.96, 0.38)]),
    "Q": (1.14, [_o(1.14), [(0.72, 0.32), (1.12, 0.02)]]),
    "R": (1.00, [_stem(), _bowl(0.98, 0.40), [(0.60, 0.38), (0.90, 0)]]),
    "S": (1.02, [_s(1.02)]),
    "T": (0.96, [[(0, HI), (0.96, HI)], [(0.48, HI), (0.48, 0)]]),
    "U": (1.02, [[(H, 1)] + _arc(H + R, LO + R, R, 180, 270) + _arc(1.02 - H - R, LO + R, R, 270, 360) + [(1.02 - H, 1)]]),
    "V": (1.06, [[(H, 1), (0.45, LO), (0.61, LO), (1.06 - H, 1)]]),
    "W": (1.36, [[(H, 1), (0.36, LO), (0.68, 0.72), (1.0, LO), (1.36 - H, 1)]]),
    "X": (1.02, [[(H, 0), (1.02 - H, 1)], [(H, 1), (1.02 - H, 0)]]),
    "Y": (1.02, [[(H, 1), (0.51, 0.45), (1.02 - H, 1)], [(0.51, 0.45), (0.51, 0)]]),
    "Z": (0.96, [[(0, HI), (0.96 - H, HI), (H, LO), (0.96, LO)]]),
}
TRACKING = 0.30


def word(text):
    """All strokes of a word and its total width."""
    x, strokes = 0.0, []
    for ch in text.upper():
        advance, paths = ALPHABET[ch]
        strokes += [[(x + px, py) for px, py in path] for path in paths]
        x += advance + TRACKING
    return strokes, x - TRACKING
