# The model

What Magmora computes, where its numbers come from, how it was checked, and where it is wrong. The research
behind it is in `docs/research/` (CIRCUIT.md: the circuit and every component value with its source and a
confidence mark; DEVICES.md: tube, transformer and speaker data; CABINET.md: the built-in cabinet and the virtual
microphone). No schematic image is in this repository; everything is described in our own words.

Comments in the source code also cite the maintainers' internal design notes, which are not published: the plans
(`docs/PLAN.md`, `docs/PLAN_EXPANSION.md`) and their sections, work packages (WP, XP), amendments (A1, A2, ...),
decisions (D1, D2, ...) and the cabinet model's round-by-round ledger (`docs/research/CAB.md`). The public record of
what those notes settled is this file, `docs/VERIFICATION.md` and `docs/research/`.

## 1. The circuit

The circuit studied is a two-channel, 120 W all-valve head of 1992 (the block-letter version of the reference head), from the
manufacturer's published service drawings, read value by value at 400 dpi and read a second time
independently (CIRCUIT.md section 1). The model includes everything between the input jack and the speaker
terminals:

- **Preamp:** V1A, the rhythm and lead pre-gain networks (with Bright and Crunch), V1B, the cold-biased V2A,
  V2B, the pad into V5B, the unity-gain shunt-feedback stage V5A, the tone stack with its mid control at the
  wiper, the rhythm and lead post-gain controls, the cathode follower V3B and V3A. Both channels share every
  valve; the relays switch networks, a cathode bypass and a grid shunt, as on the drawing (CIRCUIT.md 2 to 6).
- **Power amplifier:** a long-tailed-pair phase inverter and four 6L6GC in push-pull (modelled as two matched
  pairs), grid stoppers, screen resistors, flyback diodes, the output transformer, global negative feedback
  from the 8 ohm tap through the resonance and presence networks (CIRCUIT.md 7).
- **Supply:** rectifier and reservoir, the screen dropper with the standby switch in it, the preamp dropper
  ladder, and the fixed bias. Supply sag and the slow recovery of coupling capacitors after grid conduction
  (blocking) come out of the circuit; nothing is added on top.
- **Load:** a reactive 16 ohm load fitted to published impedance curves of 12-inch guitar speakers in a
  4x12, or a plain resistor (CIRCUIT.md 7.4).

Each value carries a confidence mark in CIRCUIT.md: A (read unambiguously), B (read, connection interpreted),
C (a published measurement of another unit) or E (our estimate). The C and E values that matter most:

| what | value | source |
|---|---|---|
| plate supply at idle | 475 V, source 487 V behind 160 ohm | technician measurements (C) / estimate (E) |
| bias | -63.35 V, set for the measured 16 mA idle current | CIRCUIT.md 8 (E) |
| output transformer | 2.45 k plate to plate, 50 H, 0.35 ohm secondary | published tear-down turns ratio (C); leakage 6 mH and 300 pF per half are estimates (E) |
| speaker load | 12.9 ohm, two semi-inductive sections, resonance 110 Hz | data sheet and published curves (C); resonance peak (E) |
| 12AX7 and 6L6GC laws | fitted to data-sheet curves digitised by us | DEVICES.md 2 to 4 |
| pot tapers | audio 10 %, "5 %" audio 5 % at half rotation | interpretation of the printed codes (E) |

Not modelled: the effects loop, the heater supply and mains hum, microphonics, valve warm-up and ageing,
transformer core saturation, tube mismatch, relay transients.

## 2. Two value sets

The engine carries two complete value sets and runs either, chosen per instance and saved with the session:

- **As drawn (1992):** every value of the 1992 drawing.
- **Revised (default, internally "prototype"):** the same, except three values of the lead path: C17 470 pF
  without R29, and C2 1 nF, which are the values the manufacturer's own drawing of 2001 shows for the same chain
  of stages.

How Revised was chosen. Measured with tone bursts, both real-amp captures lose about
12 dB more low end ahead of their clipping stages than the 1992 netlist does, in agreement with each other.
No single 1992 part, and no combination of parts within tolerance, explains that; the 2001 values do, with no
free parameter. **But a first-order high-pass ahead of each captured amplifier, at a different corner in each
capture chain, explains it as well or better**, and two captures cannot separate the two explanations. The
second capture also took part in the choice. Revised is therefore provisional: both sets ship,
every results table shows both, and "As drawn" is the one that enters equal-freedom comparisons. What would
settle it: captures of one amplifier at several gain settings in one documented chain, or reading the three
parts on a block-letter board.

## 3. The solver

- **Nodal K-method on small clusters.** The circuit is cut into seven clusters at nodes driven from a low
  impedance into a large resistor: four preamp clusters, the tone stack with the follower and V3A, the whole
  power amplifier with its feedback loop, and the supply. Each cluster is a state-space system with
  trapezoidal companion models; its nonlinearity is the vector of valve port voltages, solved by damped Newton
  with junction-step limiting and a monotonicity test on successive corrections. The power amplifier and its
  global feedback are one implicit system: there is no delay in any signal feedback path.
- **Coupling between clusters** uses the companion conductance of the downstream load inside the upstream
  solve, with only the valves' departure from their linearised slope one sample late.
- **Controls** are resistance changes on a fixed topology; a moving control rebuilds the matrices of its
  cluster every 32 host samples. Relay-type switches change under a 12 ms output mute.
- **Device laws** are tabulated with cubic Hermite interpolation at load time. Newton tolerance 1e-4 V.
- **When a solve does not converge** (under extreme input), the engine walks back to the previous solution and
  continues adaptively, within a fixed budget per sample. The output stays bounded and finite; such samples
  are counted as fallbacks for diagnosis and are expected on torture input.
- **Oversampling** of the whole chain with our own half-band and third-band FIR filters, linear phase
  (latency reported to the host) or minimum phase (zero reported latency):

| quality | 44.1 / 48 kHz | 88.2 / 96 kHz | 176.4 / 192 kHz | use |
|---|---|---|---|---|
| Standard | 6x | 3x | 1x | default |
| High | 8x | 4x | 1x (as Standard) | real time; at 176.4 / 192 kHz 2x could not be held under one core |
| Ultra | 16x | 8x | 4x | offline rendering only; plays at High in real time |

Below about 176 kHz internal the trapezoidal rule's frequency warping makes the model audibly dark and its
harmonics wrong, so there is no lower setting.

## 4. Verification

Three kinds, each reproducible by a script (full table with every number: `docs/VERIFICATION.md`):

1. **Against SPICE.** ngspice runs the same netlist, exported from the same Python description, with device
   laws identical character for character. DC operating point within 0.05 mV; small-signal response to every
   probe and harmonics of sine sweeps on every channel within the gates, with the exceptions in section 5;
   every circuit cluster on its own agrees with ngspice to -59 dB or better (gate -30 dB).
   The prototype in `proto/` is the executable specification; the C++ engine matches it (G1b).
2. **Against the real amplifier.** Two neural captures of block-letter heads, with the controls a player has
   (input gain, knobs, switches) fitted and scored on a multi-resolution spectral loss and per-band balance. The
   captures are private, so this comparison is not reproducible from the repository; its failing gates are listed
   in section 5.
3. **Engineering.** Aliasing, latency, CPU, automation without zipper noise, state recall, plug-in validation
   (pluginval at strictness 10), soak testing.

## 5. Known failures and limitations

- **Lead channel driven hard, harmonics at Standard.** With the lead channel hot at 0.5 V input, the worst
  harmonic at the speaker is 2.57 dB from ngspice against a gate of 1.5 dB. Most of it is the trapezoidal rule
  at Standard's internal rate on a hard-clipped output stage: the monolithic solver at the same rate is 2.46 dB
  off on the same harmonic, about 45 dB down. At High it is 1.01 dB against a 1.0 dB gate with the Revised
  values (As drawn: 0.40 dB); the Python prototype gives the same 1.01 dB, so this is the model itself. The
  gates were not moved.
- **One small-signal case,** the lead channel with lead post-gain at 0, is 0.513 dB from ngspice up to 10 kHz
  against a 0.5 dB gate.
- **Band balance against the captures.** The 3.2 to 6.4 kHz band on the main capture (1.56 dB against 1.5)
  and the 100 to 200 and 200 to 400 Hz bands on the second capture exceed their gates.
- **High quality is not real-time safe on every input.** On full-scale noise at a 96 kHz host rate, High needs
  about 3 % more than one core. Ultra is offline only by design.
- **The default voicing is provisional** (section 2), and the second capture is not blind for it.
- **Unknowns of a specific amplifier:** transformer leakage and capacitance, the speaker resonance peak, the
  supply source resistance, the bias, pot tapers and valve spread are estimates or averages (CIRCUIT.md 10,
  DEVICES.md 7). The captures were taken through unknown load boxes and chains, so the model cannot be
  matched to them more closely than those unknowns allow.
- **The built-in cabinet is a model, shipped provisionally**: the round-8 physical
  model of a closed-back 4x12 with a 57-type microphone (docs/research/CABINET.md), rendered on the virtual
  microphone's position grid and compiled in. Its broad tone differs from the real recordings by less than a real
  one-inch microphone move, on the positions it was tuned on (held-out positions were never compared); its fine
  structure is not a real cabinet's; it fails the gate on how the sound changes as the microphone backs off the
  grille; and in blind A/B listening the real recordings still sound more real.
- **Slow effects** (sag recovery beyond 85 ms, long blocking) are checked against SPICE only: the captures are
  neural models with about 85 ms of memory.
