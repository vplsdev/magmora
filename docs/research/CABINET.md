# The cabinet: the built-in modelled 4x12 and the virtual microphone

What the cabinet section computes, where the numbers of the built-in cabinet come from, and where it is wrong. The
executable forms are `proto/cab/` (Python prototype) and `dsp/src/cab/` (the C++ port in the plug-in).

## 1. The virtual microphone

The cabinet section plays a *pack*: a folder of impulse responses of one cabinet at several microphone positions, or
the built-in pack (section 2). A virtual microphone moves between the recorded positions. Python prototype:
`proto/cab/virtualmic.py`; C++: `dsp/src/cab/VirtualMic.h`, `Positions.h`, `Morph.h`, which match the prototype to
rounding (`eval/cab/test_virtualmic.py`, `dsp/tests/test_cab.cpp`).

- **Positions from file names.** `parse_name()` reads, in order: a per-file entry of the pack's mapping file
  (`magmora-positions.json` next to the pack); the mapping file's regular expressions with named groups (`mic`,
  `speaker`, `x` across the cone from the cap centre, `dist` off the grille, `angle`, `step`, `variant`) and their
  units; then built-in tokens (lengths in in / mm / cm, off-axis angles, speaker tags such as LL LR UL UR, microphone
  model-number tokens, zone words such as cap, cap edge, cone, edge, and lettered or numbered steps). Files are grouped
  into tracks per microphone, speaker, variant, angle and grille distance, each sorted along its coordinate. A pack
  whose positions are only an ordered series (no millimetres) becomes a "step" track that the position knob walks
  through in equal steps, from the cap outwards. When that order comes from the names alone (no mapping file), the
  C++ pack loader turns a track round if its last step is brighter than its first (energy of the first difference
  over the energy, times the rate squared), so the knob's cap end is the bright end whichever way a maker letters
  its files: ML Sound Lab letters A to E dark to bright. An order a mapping file gives is kept as written. The
  prototype does not do this.
- **Interpolation between two recorded positions ("morph").** Peaks and notches of the two magnitude responses are
  matched along a log-frequency axis by dynamic programming (1/96 octave, each feature may move up to 1/6 octave) and
  are moved as well as faded; the result is rebuilt as a minimum-phase response. A plain crossfade would cancel where
  the two responses are out of phase, and interpolating levels in dB turns a notch that moves into two half notches;
  the synthetic test in `test_virtualmic.py` checks that a notch moving from 3.0 to 3.6 kHz stays one deep notch at
  the midpoint. For packs that are not minimum-phase, the interpolated arrival time is kept as a delay. Grille distance
  is a second axis (`interpolate_2d`); off-axis angle snaps to recorded angles.
- **Two microphones.** Each slot has level, constant-power pan and polarity. For minimum-phase or already aligned
  packs nothing is measured or moved; for raw packs with time of flight the arrival difference is measured by
  upsampled cross-correlation and removed with sub-sample accuracy (the detected polarity is shown, never applied).
- **Runtime.** A pack is indexed on the main thread; a new response is built on a worker thread when a control moves
  and handed to the audio thread through an atomic pointer; a partitioned zero-latency convolver crossfades to it over
  20 ms. The audio thread never allocates. Latency: zero.

A slot can also play a single impulse-response file of the user's (`dsp/src/cab/UserIr.h`).

## 2. The built-in cabinet: "Trad 4x12 (modelled)"

The default pack is computed, not recorded: a frequency-domain physical model of a closed-back 4x12 with four
12-inch paper-cone drivers and a 57-type dynamic microphone, rendered to impulse responses. Code: `proto/cab/model.py`
and the layers built on it, `model4.py`, `model5.py`, `model7.py` and `model8.py` (the shipped form), with the shell
elements in `shell.py`, `shelln.py` and `harmonics5.py`.

### 2.1 The model

- **Driver.** An axisymmetric thin-shell finite-element model of the paper cone, the dust cap and the half-roll
  surround (`shell.py`: conical-frustum Kirchhoff-Love elements), with the coil, former and half the spider as a mass
  at the neck. The mechanical impedance the coil sees is the shell's own, so the electrical impedance follows from
  the same structure that radiates. Circumferential orders n >= 1 (`shelln.py`, `harmonics5.py`) are driven by the
  mass of the lead-out wires; they radiate nothing on the axis and more off it.
- **Top end** (`model7.py`). The paper is not uniform around the circumference, so bending waves run at slightly
  different speeds in different sectors: each ring's radiation gets a phase spread proportional to the lag it has
  accumulated from the neck (a fixed, smooth pattern, seeded once). The cap's loss factor rises with frequency, and
  the glue bead between cap and cone is a ring mass.
- **Cabinet and room** (`model5.py`). The inside of the box as built (the drivers, mounted from behind, stand inside
  the volume and shift its standing waves); the panels as plywood plates coupled to the standing waves; first-order
  images in the floor, the wall behind, the side walls and the ceiling.
- **Field and microphone** (`model.py`). Rayleigh integral over the miked cone's real geometry and over the other
  three, baffle-edge sources, a floor image; the microphone as a diaphragm average with a first-order pattern whose
  pressure share follows published polar readings, and its published on-axis curve as a minimum-phase capsule
  response.
- **Unit tolerance** (`model8.py`). Each cone has its own value of four driver numbers, as a deviation from the
  shared value within a fixed tolerance: cone Young's modulus +-15 %, pulp density +-5 %, cone loss factor +-20 %,
  moving mass of the coil assembly +-10 %. The deviations of the three fitted cones sum to zero, so the shared values
  stay the type; the fourth cone carries the shared values.

There is no correction filter, band gain or residual stage: every number is geometry, material, placement or a room
distance. Limit checks against physics and closed forms, without any measured reference: `proto/cab/checks.py`,
`checks5.py`, `shell_checks.py`.

### 2.2 Where the numbers come from

Published data, digitised by us into `docs/research/data/`: the driver's published electrical and dimensional data
and on-axis response (`cab_driver_published.csv`, `driver_ref_onaxis.csv`), the voice-coil impedance rise
(`driver_8ohm_impedance_band.csv`, as in DEVICES.md 6), and the on-axis responses and polar readings of the
microphone types (`mic_*.csv`). The driver's moving mass, force factor, compliance and losses, the cone and cap
geometry, the cabinet's inner dimensions and the microphone placement are not published; they are estimates (marked
EST in the code) with physical ranges.

Within those ranges and tolerances the free numbers were fitted against magnitude responses of a real cabinet of the
same family recorded at documented positions. Those recordings are private and are not in this repository; nothing
derived from them per frequency is either. The fitted values are `proto/cab/tuned_r8a.json` (physical parameters
only).

### 2.3 Reproducing the pack

```
source scripts/env.sh
$BLOCKAMP_PY proto/cab/make_builtin_pack.py --check-only   # render, compare with plugin/generated/BuiltinCab.json
$BLOCKAMP_PY proto/cab/make_builtin_pack.py                # rewrite plugin/generated/BuiltinCab.inc and .json
```

The script reads `proto/cab/tuned_r8a.json` and nothing else, refuses to run unless the model's fingerprint is
`1a1824ea7a9a9376`, and renders the grid the virtual microphone needs: four cones, the microphone on the axis at 0 to
2 in from the cap centre in 0.25 in steps and 0 / 0.5 / 1 in off the grille, 108 responses of 4096 samples at 48 kHz
(85 ms), float32, one scale for the whole pack (peak 1.0). `BuiltinCab.inc` is compiled into the plug-in through
`plugin/BuiltinCab.h` and indexed exactly as a folder of the same files would be (`test_cab`: bit for bit).

### 2.4 Limits

- On the positions it was fitted on, the model differs from the real recordings, in 1/3-octave bands from 70 Hz to
  1 kHz / 1 to 5 kHz / 5 to 10 kHz, by about 1.0 / 1.1 / 2.3 dB rms: less than moving a real microphone one inch
  along the cone changes a real recording, but more than the difference between two real speakers of the same type at
  the same position below 5 kHz. Its top-end level and how that changes from the cap to the edge stay within the
  spread between the four real speakers. Held-out positions were never compared.
- The fine structure of a real cabinet's response (the dense pattern of small peaks and dips) is not reproduced.
- The way a real recording changes as the microphone backs off the grille from 0.5 to 1 in is not reproduced: the
  model fails that check (correlation 0.68 against a bar of 0.89).
- In blind listening against real recordings at the same positions, the real ones still sound more real.

For the most realistic result, load an impulse response or a pack you trust.
