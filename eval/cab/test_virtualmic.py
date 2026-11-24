"""Unit tests of the virtual-microphone prototype on names and synthetic responses.  $BLOCKAMP_PY eval/cab/test_virtualmic.py"""
import sys, unittest
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "proto" / "cab")); sys.path.insert(0, str(Path(__file__).resolve().parent))
import virtualmic as V, metrics as M

FS = 48000


def notch_ir(fc, depth_db=18.0, q=8.0, n=2048):
    """Minimum-phase response: gentle low-pass with one notch at fc."""
    f = np.fft.rfftfreq(4 * n, 1 / FS)
    mag = 1 / np.sqrt(1 + (f / 6000.0) ** 4) * 10 ** (-depth_db / 20 / (1 + (q * (f / fc - fc / np.maximum(f, 1))) ** 2))
    return V._minimum_phase(mag, n)


def notch_at(h, lo, hi):
    f = np.fft.rfftfreq(8192, 1 / FS); m = (f > lo) & (f < hi)
    d = 20 * np.log10(np.abs(np.fft.rfft(h, 8192)) + 1e-12)
    ref = 20 * np.log10(1 / np.sqrt(1 + (f / 6000.0) ** 4))
    return f[m][np.argmin((d - ref)[m])], float((d - ref)[m].min())


class Names(unittest.TestCase):
    def test_documented_scheme_by_tokens(self):
        p = V.parse_name("SPK LR CAB 4x12 DY57 0.75in 0.5in OA30 PRE1.wav")
        self.assertEqual((p["speaker"], p["mic"], p["angle_deg"]), ("LR", "dy57", 30.0))
        self.assertAlmostEqual(p["x_mm"], 19.05); self.assertAlmostEqual(p["dist_mm"], 12.7)

    def test_user_scheme_and_override(self):
        m = dict(V.MAPPING_EXAMPLE)
        p = V.parse_name("SPK UL CAB 4x12 DY57 1.25in 0.0in PRE2.wav", m)
        self.assertEqual((p["source"], p["speaker"], p["variant"]), ("scheme", "UL", "PRE2")); self.assertAlmostEqual(p["x_mm"], 31.75)
        self.assertEqual(V.parse_name("PACK-OS-421-D.wav", m)["step"], 3)
        self.assertEqual(V.parse_name("my favourite.wav", m), dict(mic="57", x_mm=19.0, dist_mm=0.0, source="mapping"))

    def test_common_tokens(self):
        self.assertEqual(V.parse_name("Maker OS R30 57 Cap Edge 1in.wav")["zone"], 1.0)
        self.assertAlmostEqual(V.parse_name("Maker OS R30 57 Cap Edge 1in.wav")["dist_mm"], 25.4)
        self.assertEqual(V.parse_name("412_R30_dy421_cone_45deg_25mm.wav")["angle_deg"], 45.0)
        self.assertAlmostEqual(V.parse_name("412_R30_dy421_cone_45deg_25mm.wav")["dist_mm"], 25.0)
        self.assertEqual(V.parse_name("Brand 4x12 RB121 pos 3.wav")["step"], 3)
        self.assertEqual(V.parse_name("no position here.wav")["source"], "tokens")

    def test_generic_mic_tokens(self):
        # up to three letters of a model prefix, then a model-number core; position, angle and preamp markers are not mics
        mic = lambda n: V.parse_name(n).get("mic")
        self.assertEqual(mic("412_R30_dy421_cone_45deg_25mm.wav"), "dy421")
        self.assertEqual(mic("Maker OS R30 57 Cap Edge 1in.wav"), "57")
        self.assertEqual(mic("XYZ 412 Q25 b5-CE 1.wav"), "b5")
        self.assertEqual(mic("ab57 then cd421 cap.wav"), "cd421")
        self.assertEqual(mic("XYZ 412 AB20 N22-5.wav"), None)
        self.assertEqual(mic("OA20 cone 1in.wav"), None)
        self.assertEqual(mic("pos5 cap.wav"), None)
        self.assertEqual(mic("cone57 abcd57.wav"), None)

    def test_builtin_pack_names(self):
        # the built-in cabinet (A30): every name of its grid reads back to its speaker and position, and the grid is
        # fine enough for the A19 morph everywhere (6.35 mm across the cone, 12.7 mm off the grille)
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "proto" / "cab"))
        import make_builtin_pack as B
        for sp in ("LL", "LR", "UL", "UR"):
            for c in B.CAPS:
                for g in B.GRILLE:
                    p = V.parse_name(B.file_name(sp, c, g), B.MAPPING)
                    self.assertEqual((p["source"], p["mic"], p["speaker"]), ("scheme", "57-type", sp))
                    self.assertAlmostEqual(p["x_mm"], 25.4 * c); self.assertAlmostEqual(p["dist_mm"], 25.4 * g)
        self.assertLessEqual(max(np.diff(B.CAPS)) * 25.4, 12.7 + 1e-9); self.assertLessEqual(max(np.diff(B.GRILLE)) * 25.4, 12.7 + 1e-9)

    def test_grid_and_bracket(self):
        names = [f"SPK LR CAB 4x12 DY57 {c:.2f}in 0.0in PRE1.wav" for c in (0, 1, 2)] + ["PACK-57-A.wav", "PACK-57-C.wav", "PACK-57-E.wav", "readme.wav"]
        g = V.build_grid(names, V.MAPPING_EXAMPLE)
        kinds = sorted(k[-1] for k in g)
        self.assertEqual(kinds, ["step", "x_mm"])
        steps = [v for k, v in g.items() if k[-1] == "step"][0]
        self.assertEqual([c for c, _ in steps], [0.0, 2.0, 4.0])
        self.assertEqual(V.bracket([0, 25.4, 50.8], 19.05)[:2], (0, 1)); self.assertAlmostEqual(V.bracket([0, 25.4, 50.8], 19.05)[2], 0.75)
        self.assertEqual(V.bracket([0, 25.4], 99.0), (1, 1, 0.0))


class Interp(unittest.TestCase):
    def test_endpoints_and_identity(self):
        a, b = notch_ir(3000), notch_ir(3600)
        for m in ("snap", "linear", "logmag", "morph"):
            np.testing.assert_allclose(V.interpolate(a, b, 0.0, FS, m), a); np.testing.assert_allclose(V.interpolate(a, b, 1.0, FS, m), b)
            if m != "snap":
                self.assertLess(max(r for r, _ in M.band_distance(V.interpolate(a, a, 0.5, FS, m), a, FS)), 0.05)

    def test_moving_notch(self):
        """A notch that moves from 3.0 to 3.6 kHz: the true midpoint has ONE notch near 3.3 kHz, about 18 dB deep."""
        a, b = notch_ir(3000), notch_ir(3600)
        f_m, d_m = notch_at(V.interpolate(a, b, 0.5, FS, "morph"), 2500, 4200)
        self.assertLess(abs(f_m - 3290), 80); self.assertLess(d_m, -13.0)          # moved, and still deep
        f_l, d_l = notch_at(V.interpolate(a, b, 0.5, FS, "logmag"), 2500, 4200)
        self.assertGreater(d_l, -11.0)                                               # the dB interpolation fills it: two half notches
        self.assertGreater(min(abs(f_l - 3000), abs(f_l - 3600)), -1)               # (they stay at the old places)
        self.assertTrue(abs(f_l - 3000) < 120 or abs(f_l - 3600) < 120)

    def test_fixed_notch_keeps_depth(self):
        a, b = notch_ir(3000, 18), 0.5 * notch_ir(3000, 18)
        self.assertLess(notch_at(V.interpolate(a, b, 0.5, FS, "morph"), 2500, 3600)[1], -16.0)

    def test_raw_pack_keeps_time_of_flight(self):
        a, b = np.concatenate([np.zeros(20), notch_ir(3000)]), np.concatenate([np.zeros(60), notch_ir(3000)])
        h = V.interpolate(a, b, 0.25, FS, "morph", minimum_phase=False)
        self.assertAlmostEqual(V._onset(h, FS) - V._onset(a, FS), 10.0, delta=1.0)


class TwoMics(unittest.TestCase):
    def test_raw_alignment_polarity_offset(self):
        a = notch_ir(3000); b = -0.7 * V._shift(np.pad(notch_ir(4000), (0, 256)), 37.3)[:2048]
        out, info = V.two_mics(a, b, FS, pack_phase="raw")
        self.assertAlmostEqual(info["measured_delay_samples"], 37.3, delta=1.0); self.assertEqual(info["detected_polarity"], -1)
        _, info2 = V.two_mics(a, b, FS, pack_phase="minimum")
        self.assertEqual(info2["moved_samples"], 0.0)                                # nothing is invented for aligned packs
        _, info3 = V.two_mics(a, a, FS, pack_phase="minimum", offset_mm=10.0)
        self.assertAlmostEqual(info3["moved_samples"], 10e-3 / 343 * FS, places=6)
        f = np.fft.rfftfreq(4096, 1 / FS)
        o, _ = V.two_mics(a, a, FS, pack_phase="minimum", offset_mm=20.0)           # 20 mm: first cancellation at c / (2 * 0.02) = 8.6 kHz
        s = np.abs(np.fft.rfft(o[0], 4096)) / (np.abs(np.fft.rfft(a, 4096)) + 1e-12)
        self.assertLess(abs(f[np.argmin(s[(f > 5000) & (f < 12000)]) + np.searchsorted(f, 5000)] - 8575), 300)

    def test_pan_level_invert(self):
        a = notch_ir(3000)
        o, _ = V.two_mics(a, a, FS, pan1=-1.0, pan2=1.0, level2_db=-6.0, invert2=True, pack_phase="minimum")
        np.testing.assert_allclose(o[0], a, atol=1e-12); np.testing.assert_allclose(o[1], -10 ** (-6 / 20) * a, atol=1e-9)
        l, r = V.pan_gains(0.0)
        self.assertAlmostEqual(l * l + r * r, 1.0)


if __name__ == "__main__":
    unittest.main(verbosity=1)
