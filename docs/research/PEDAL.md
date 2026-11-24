# The overdrive pedal ("Cinder", working name): circuit, devices, model, verification

Research of 23 Nov 2026. The executable form is `proto/blockamp/pedal.py`; the SPICE check is
`eval/check_pedal_spice.py`. The scoring against the pedal reference (section 7) reads private captures, so its scripts
are not in the repository. No schematic image, excerpt or third-party data
file is in the repository; everything below is in our own words, and the only data files are curves we digitised
ourselves (`docs/research/data/1n914_onsemi_forward.csv`, `ksc1815_fairchild_transfer.csv`).

"The pedal reference" is a private set of neural captures of one real unit of the 1979 design (a 2003 reissue)
over a drive sweep. It is never in the repository; only aggregate numbers from it appear here (section 7).

Confidence marks, as in CIRCUIT.md: **A** two independent readings agree and a third source confirms the value;
**B** two readings agree; **C** sources disagree or there is one source; **E** our estimate.

## 1. Sources read in this round

| key | source | what it gave | how it was read |
|---|---|---|---|
| K | R. G. Keen, article on the technology of the reference overdrive (geofex.com, 1998). [N18] (title in the private notes) | the circuit in words, stage by stage, with values; the table of output resistors by version; remarks on the op-amp | page text, read in full |
| L | an LTspice schematic file of the 1979 circuit published with a simulation write-up (cushychicken.github.io) | a complete machine-readable schematic | **netlist rebuilt by us from the file's geometry** (wires and pin positions; no dangling pin), so this reading does not depend on anyone's drawing |
| G | General Guitar Gadgets, parts list of their replica project (read in the research round) | values, version differences | page text |
| A | Aion FX, build document of their replica (2025) | values "for the original 1979 version", a statement on the original's drive-pot taper | PDF text |
| N | New Japan Radio, NJM4558 data sheet (2013) | op-amp gain, bandwidth, slew rate, output swing | PDF text and the swing figure |
| T | Texas Instruments, RC4558 data sheet (rev. H, 2024) | a second maker's figures for the same type | PDF text |
| O | onsemi, 1N914/1N4148 data sheet (1N914-D) | diode forward curves and capacitance | **vector curves of the PDF digitised by us** |
| V | Vishay, 1N4148 data sheet (2024) | cross-check of the forward curve | figure read by eye |
| F | Fairchild, KSC1815 data sheet (2002), second source of the 2SC1815 | transistor transfer curve, current gain, capacitance | **vector curves digitised by us** |

Could not be reached again: the Electrosmash analysis (the host does not resolve from here). A manufacturer's
drawing was not found, and a data sheet of the original MA150 diode was not found (404).

The two independent readings of the topology are **K** (prose) and **L** (rebuilt netlist). They agree on every
connection of the signal path. G and A are replicas' parts lists: they confirm values, not connections.

## 2. Circuit

Supply 9 V; a bias rail at half of it (two equal resistors and a large capacitor in the unit; an ideal 4.5 V
source in the model). Signal path, with our designators:

1. **Input buffer.** Jack, `Cin`, `Rin` in series, base of Q1; `Rb1` from the base to the bias rail; emitter follower
   with `Re1` to ground.
2. **Clipping amplifier.** From the emitter through `Cc1` to the non-inverting input of op-amp A, which `Rp1` ties to
   the bias rail. Inverting input: `Cg` and `Rg` in series to ground. Feedback from output to inverting input, all in
   parallel: `Rf` in series with the Drive pot, `Cf`, and two anti-parallel diodes.
3. **Tone stage.** `Rlp` from op-amp A's output to op-amp B's non-inverting input, `Clp` from there to ground, `Rp2`
   from there to the bias rail. The Tone pot's ends are on B's two inputs; its wiper goes through `Ct` and `Rt` to
   ground. `Rfb2` from B's output to its inverting input.
4. **Level.** B's output through `Cc2` and `Rlv` to the top of the Level pot; its bottom on the bias rail; wiper to
   the electronic bypass switch (not modelled) and on to the output buffer.
5. **Output buffer.** Emitter follower Q2 biased through `Rb2`, emitter resistor `Re2`; then `Ros` in series, `Cout`,
   and `Rosh` to ground at the output jack.

## 3. Values

### 3.1 Signal path

| part | value | K | L | G | A | mark | note |
|---|---|---|---|---|---|---|---|
| Cin | 22 nF | - | 20 nF | 22 nF | 22 nF | B | 20 nF and 22 nF are the same part in old and new marking; 0.4 Hz difference in a 14 Hz corner |
| Rin | 1 k | - | 1 k | (one of five 1 k) | 1 k | B | in series with the base |
| Rb1, Rb2 | 510 k | 510 k | 510 k | 510 k | 510 k | A | |
| Re1, Re2 | 10 k | 10 k | 10 k | 10 k | 10 k | A | |
| Cc1 | 1 uF | 1 uF | 1 uF | 1 uF | 1 uF | A | |
| Rp1 | 10 k | "usually 10 k" | 10 k | 10 k | 10 k | A | |
| Rg, Cg | 4.7 k, 47 nF | yes | yes | yes | yes | A | corner 720 Hz |
| Rf | 51 k | yes | yes | yes | yes | A | |
| Drive pot | 500 k | yes | yes | yes | yes | A | taper: 3.3 |
| Cf | 51 pF | yes | yes | yes | (47 pF fitted, "original 51 pF") | A | |
| diodes | small-signal silicon | "silicon signal diodes" | 1N4148 | 1N914 | 1N914 | B | the original part number (MA150) is from G only; no data sheet found |
| Rlp, Clp | 1 k, 0.22 uF | yes | yes | yes | yes | A | corner 723 Hz |
| Rp2 | 10 k | - | 10 k | (one of seven 10 k) | 10 k | B | loads the low-pass: 0.83 dB of loss |
| Tone pot | 20 k | yes | yes | yes | yes | A | taper: 3.3 |
| Rt, Ct | 220 ohm, 0.22 uF | yes | yes | yes | yes | A | |
| Rfb2 | 1 k | - | 1 k | 1 k | 1 k | B | |
| Cc2, Rlv | 1 uF, 1 k | - | yes | yes | yes | B | |
| Level pot | 100 k | yes | yes | yes | yes | A | taper: 3.3 |
| Rsw | 510 k | "high value resistors" at the switch | 510 k | - | - | C | wiper to bias rail; part of the electronic switch, absent from replicas with a mechanical bypass |
| Cc3 | 1 uF | - | 1 uF | - | - | C | **open:** K describes the wiper reaching the output buffer's base through the switch transistor, with no capacitor named. With or without it the audio band is unaffected (0.3 Hz corner); the base's bias path differs, which moves the follower's idle current by an estimated 15 % |
| Ros, Rosh | 100 ohm, 10 k | yes | (file ends at the emitter) | yes | yes | A | 470 ohm and 100 k in the 1981 version (K, G, A) |
| Cout | 10 uF | 10 uF | - | 10 uF | - | B | |
| op-amp | 4558-type dual | yes | (another type used in the file) | yes | yes | A | |
| transistors | 2SC1815 | yes | (another type) | "original 2SC1815" | (another type) | B | |

Not modelled: the electronic bypass (two JFETs and a flip-flop; the switch transistor in the signal path is about
100 ohm when on, per K), the bias divider's finite impedance, supply ripple, op-amp input bias current and
input resistance (25 nA and 5 M typical per N: 0.25 mV across 10 k).

### 3.2 The 1979 and 1981 output networks: recommendation

An earlier estimate said the two networks differ by 0.04 dB into the head. Measured on the model (round 2, one
cluster at 288 kHz, the pedal driving 1 M):

| setting, tone | input | 1979: peaks, H2, H3 | 1981: peaks, level, H2, H3 | level-matched waveform difference |
|---|---|---|---|---|
| boost (Drive 0, Tone 5, Level 10), 440 Hz | 0.3 V | +0.56 / -0.56 V, -53.7, -19.3 dB | +0.57 / -0.57 V, +0.12 dB, -56.3, -19.3 dB | -48.5 dB |
| same | 1.0 V | +1.15 / -1.13 V, -43.3, -22.5 | +1.16 / -1.16 V, +0.14 dB, -49.7, -22.4 | -45.4 dB |
| same | 2.0 V | +1.87 / -1.56 V, -25.0, -33.7 | +1.91 / -1.70 V, +0.31 dB, -28.9, -30.8 | -31.4 dB |
| same, 110 Hz | 1.0 V | +1.09 / -1.07 V, -42.0, -20.2 | +1.10 / -1.09 V, +0.14 dB, -45.9, -20.1 | -36.8 dB |
| Drive 5, Tone 5, Level 7.5, 440 Hz | 2.0 V | +0.56 / -0.50 V, -29.0, -30.4 | +0.57 / -0.51 V, +0.19 dB, -29.3, -30.4 | -48.5 dB |

- The small-signal difference is 0.12 to 0.18 dB of level, nothing else.
- The 1979 network does halve the output follower's AC load (5.0 k against 9.0 k), so its negative half-wave gives
  out earlier, as round 1 found. But it only shows once the output itself exceeds about 1.5 V peak, which needs
  about 2 V peak at the input with Level at maximum. There the two versions differ by 0.3 dB in level and 3 to 4 dB
  in a second harmonic that is 25 dB down; the level-matched difference is 31 dB below the signal. Up to 1 V in, it
  is 37 dB or more below, and most of that is the 0.1 dB level difference's phase.
- Both effects are smaller than the spread between two transistors of the same type (section 5).

**Recommendation: one version, the 1979 network, no switch.** The reason is
"0.12 dB small-signal, and a follower headroom difference that needs 2 V at the input to appear". A switch would
add a control whose effect cannot be heard below clipping levels that no guitar reaches without a boost in front.

### 3.3 Pot tapers

Sources disagree, and a taper only decides where on the knob a setting lies, never what the circuit can do.

| pot | sources | in the model | mark |
|---|---|---|---|
| Drive | A: the original is linear; G and L: logarithmic | **audio taper, 15 % of the resistance at half rotation.** Decided by the pedal reference: its eleven drive positions fit an audio law with 15 % at half rotation (0.66 dB rms in mid-band gain, best total resistance 485 k, a pot's tolerance from the nominal 500 k), and a linear law not at all (3.35 dB rms, best total 324 k) | C (one real unit) |
| Tone | A: a special symmetric ("W") taper; G: a special taper, 25 k linear as substitute | symmetric law, audio taper mirrored about the centre | E |
| Level | K and A: audio; G: linear | audio, 10 % at half rotation | C |

The reference's tone positions are not documented and its two Level groups do not share an output scale, so
neither of the other two tapers could be checked against it.

## 4. Device data (nothing from memory: every value from a document opened for this work)

### 4.1 Clipping diodes

Law i = Is (exp(v / (n VT)) - 1), VT at 25 C. Fitted to the onsemi sheet's figures 3 and 4 (forward voltage from
1 uA to 10 mA, digitised from the PDF's own curve coordinates), over 1 uA to 3 mA, which covers what the diodes
carry in this circuit (at most the input voltage divided by 4.7 k):

**Is = 4.31 nA, n = 1.944**; rms error 0.9 mV, worst 2.3 mV. Cross-check against V's figure 2: 0.26 V at 1 uA and
about 0.60 V at 1 mA by eye, against 0.273 V and 0.617 V from the fit. Capacitance at 0 V: 0.87 pF per diode
(O, figure 7; the table's maximum is 4 pF); both are in the model beside `Cf`. The placeholder parameters of the
research round's script are gone. `python -m blockamp.pedal --fit` reproduces the fit.

The original diode type has no data sheet in hand, and a 2003 reissue may carry yet another part; the reference
suggests the real unit's diodes drop slightly more (section 7).

### 4.2 Transistors

Base-emitter law from F's figure 2 (collector current against base-emitter voltage, 0.1 to 3 mA used):
**Is = 0.155 pA, n = 1.132**, rms error 0.3 mV. Current gain 200 (F's figure 3 is flat on the 200 line from 1 mA to
about 80 mA; the type's classes span 70 to 700). Idle point in the model: base 3.72 V, emitter 3.09 V, 0.31 mA.

**Early voltage (round 2).** Read from the slopes of F's figure 1 (output characteristics): 51 V on the 100 uA
curve and 78 V on the next, at collector currents of 25 to 40 mA where self-heating steepens the curves. The model
uses 65 V (mark C). With it, our follower agrees with ngspice's own Gummel-Poon transistor within 0.09 dB on every
harmonic of the 72 cases (section 5); without it the difference is up to 3.2 dB.

### 4.3 Op-amp

From N (typical, at +-15 V): voltage gain 100 dB, gain-bandwidth 3 MHz, slew rate 1 V/us, output swing equal to the
supply less 2 V per side into 2 k (figure, linear from +-6 V to +-17 V; **extrapolated** to the pedal's +-4.5 V:
+-2.5 V around the bias rail, mark E). T gives 4 MHz and 2.2 V/us for another maker's current part.

The ratio of the data-sheet slew rate to 2 pi times the gain-bandwidth is 53 mV, which is twice the thermal
voltage: the signature of a plain bipolar input pair. The model uses exactly that: the input stage's
transconductance follows a tanh law with a 53 mV scale, which gives the data-sheet bandwidth for small signals
and the data-sheet slew rate for large ones from the same two numbers. Estimates inside the macro-model: the
compensation capacitor (30 pF; only ratios with it are data), the open-loop output resistance (100 ohm; T plots
25 to 200 ohm), the sharpness of output saturation.

## 5. The model

`proto/blockamp/pedal.py` holds two constructions of the same circuit. Both feed the K-method and ngspice from
one description.

- **The product model (`build2`, `split`): each nonlinear element is one scalar function of one control voltage.**
  This is what the engine implements.
- The first construction (`build`), which assembles the same elements from junctions so that the head's solver runs
  it unchanged (12 ports). It was the first to be verified against ngspice and is kept as that lineage; the two
  agree to between -70 and -84 dB on waveforms.

| element | law | ports |
|---|---|---|
| clipping diodes | i = Is1 (exp(u/nVT) - 1) - Is2 (exp(-u/nVT) - 1); the two saturation currents differ by a forward-voltage mismatch | 1 |
| each op-amp's input pair | i = Imax tanh(u / 53 mV): the data-sheet bandwidth for small signals and the data-sheet slew rate for large ones (4.3) | 1 each |
| each op-amp's output saturation | a clamp to 2.5 V either side of the bias rail | 1 each |
| each follower's transistor | base current: junction law over beta; collector current: junction law times the Early factor, written inside the exponent | 2 each |

Nine scalar ports, 11 states as one cluster. Also in the model since round 2: input bias currents (25 nA typical),
the Early effect, the diodes' capacitance, a diode mismatch (7.3).

### 5.1 How much of the transistors and op-amps to model, closed

SPICE on both sides, 72 sine cases (110, 440, 2000 Hz; 1 mV to 3 V peak; four knob settings), against ngspice's
own Gummel-Poon transistors (Early voltage, junction capacitances) in the followers; worst difference of any
harmonic within 60 dB of the strongest, below 20 kHz:

| model | worst | cases over 0.5 dB | worst by input level 1 mV / 10 mV / 0.1 / 0.3 / 1 / 3 V |
|---|---|---|---|
| linear followers, one-pole op-amps (as first planned; round 1) | no even harmonics | 32 of 72 | - |
| junction-law followers without the Early effect | 3.16 dB | 13 of 72 | 0.02 / 0.03 / 0.23 / 0.35 / 1.09 / 3.16 |
| **the product model** | **0.09 dB** | **0 of 72** | 0.00 / 0.01 / 0.01 / 0.02 / 0.03 / 0.09 |

Round 1 also showed that the tanh input stage matters only at 3 V (8.4 dB without it). The 0.5 dB rule is now met
at every level. What remains unit-dependent, not model error: at 3 V in, the same harmonics move by 14 dB between a
current gain of 100 and one of 400, both inside the transistor type's range.

### 5.2 Trapezoidal warping at 20 kHz, and the fix

Round 1 failed the small-signal gate at 20 kHz with Drive 10 (0.322 / 0.180 dB against 0.25 / 0.1). The trapezoidal
rule places every reactance at tan(wT/2)/(wT/2) times its frequency: 1.9 % high at 20 kHz and 264.6 kHz, where the
pedal's response falls steeply.

**Fix: the two capacitors that shape the treble after the clipper (`Clp`, `Ct`) are prewarped at 20 kHz** in the
discrete model only: their value is multiplied by (wT/2)/tan(wT/2), 0.981 at 264.6 kHz and 0.984 at 288 kHz. The
SPICE reference keeps the true values.

Worst small-signal error against ngspice over the 9 settings, to 20 kHz (and to 10 kHz):

| what is prewarped | 264.6 kHz | 288 kHz | 352.8 kHz |
|---|---|---|---|
| nothing (round 1) | 0.322 (0.063) | 0.271 (0.053) | 0.180 (0.035) |
| every capacitor, at 14 kHz | 0.165 (0.143) | 0.139 (0.121) | 0.092 (0.080) |
| **`Clp` and `Ct`, at 20 kHz** | **0.157 (0.152)** | - | **0.087 (0.085)** |

- It is a redistribution, not a free gain: the error below 10 kHz rises from 0.06 to 0.15 dB. Both are inside the
  gate, which is on the worst point up to 20 kHz.
- Prewarping every capacitor passed the small-signal gate too but **failed the harmonic gate** (1.01 dB at
  Standard, 0.56 dB at High, at 3 V in): it scales the op-amps' compensation capacitors, which set the slew rate.
  Hence only the two linear treble capacitors.
- At the 288 kHz the product actually runs at for a 48 kHz host, no prewarp gives 0.271 dB, still over the gate;
  with the fix the two host rates behave alike.

## 6. Gate G-P1 (measured, final model with the reference unit's values of 7.6)

`eval/check_pedal_spice.py`; ngspice 45 on the same netlist; "Standard" is 264.6 kHz and "High" 352.8 kHz, the
lowest internal rates those qualities run at. The last column is the four-cluster split of section 8.

| quantity | gate | one cluster | four clusters |
|---|---|---|---|
| operating point, 25 nodes, 3 settings | 5 mV | 0.0005 mV, pass | - |
| small-signal response, 9 settings, 20 Hz to 20 kHz | 0.25 / 0.1 dB | **0.156 / 0.087 dB, pass** | - |
| closed-form stage equations against the netlist in ngspice | 0.25 dB | 0.155 dB, pass | - |
| harmonics, 72 cases at two probes | 1.0 / 0.5 dB | **0.440 / 0.208 dB, pass** | 0.440 dB at Standard, pass (High not rerun on the final values; 0.418 before them) |
| against full transistor models | 0.5 dB | 0.09 dB, pass (5.1) | - |

No solver failure in any gate case. Two things found on the way and fixed in the model, not in the gates:

- the scalar-port Newton iteration cycled when an op-amp slewed (3 V at 2 kHz and above): step limits per device,
  under-relaxation after 10 iterations, and the head's fallback (return to the last accepted solution and walk
  there in 8 steps). After that: 3 V sines at 5 and 10 kHz and a 2 V square wave run with no failure at three
  rates; full-band 3 V noise gives at most 1 failed sample in 0.3 s (at about 66 iterations per sample: a
  worst-case cost the engine has to bound, as the head does);
- ngspice's output-grid interpolation (round 1): the reference grid is 1.536 MHz.

## 7. Against the pedal reference (gate G-P1r)

Scored by private scripts that render the captures and fit the knob mapping. 49 usable captures (one of the 50 is a bypassed
pedal): 11 drive positions at three tone settings and Level 6, 6 at a fourth, 10 at a fifth with Level at maximum.

### 7.1 What the reference is, measured

- Polarity inverted and 2.0 samples late at 48 kHz in every capture (range 1.9 to 3.1). Harmless here.
- **A low-pass that is not the pedal's.** The captures' small-signal response relative to the model: -0.3 dB at
  2.2 kHz, -0.6 at 3.3 kHz, -0.9 at 4.4 kHz, -3 at 6.6 kHz, -6 at 8.8 kHz and **-25 to -50 dB at 13.2 kHz**, in
  all five tone sets alike. Nothing in this circuit can fall 20 dB in half an octave; and in that range the
  captures are not linear where the pedal is (their gain changes by up to several dB with level). The first-order
  corner that best explains the part below 5 kHz is 10.6 kHz.
- **A floor.** The captures' stated validation error is -40 dB (median; -45 to -24). Their third harmonic while
  the pedal is linear sits at -60 dB (median; worst capture -45 dB). Their second harmonic while clipping is
  -48 dB (median; -56 to -40).
- **Sessions.** Above -9 dB re full scale the captures disagree with each other; those levels are not scored. The
  input level differs between tone sets: +0.9 dB (set 2), -0.3 dB (set 5), -3.2 dB (the Level-maximum set, which
  also has its own output scale and a 0.66 dB scatter of scale from capture to capture).

### 7.2 Method

A stepped-sine fingerprint (110, 440, 990, 2200, 4400 Hz; 3 dB level steps) through each capture and through the
prototype. Fitted on tone sets 1 and 3 only (22 captures), in two stages:

1. the knob mapping, from low-level gains against the linearised model, exactly: the drive pot's resistance at
   each of the 11 positions and one tone position per set (round 1 did this through an interpolated table and got
   three tone positions wrong, which is what had made tone set 2 look different);
2. the volts that full scale stands for (0.593 V peak) and one output scale.

Then frozen. A holdout set gets its own tone position and its own input level (a session quantity; scored with and
without); the Level-maximum set also its output scale. No component value was fitted to make these numbers.

### 7.3 What the reference changed in the model, and what it did not

| question | finding | action |
|---|---|---|
| Second harmonic: model below -65 dB, reference at -40 to -50 | Input bias current (25 nA through up to 551 k of feedback) already gives -51 to -64 dB. The reference's second harmonic keeps one phase within a capture (resultant length 0.87) and partly across captures (0.58), so part of it is the unit. It does not grow with drive, which rules out bias current and points to unequal diodes. Fitted on 10 training captures: the median error is smallest at 6 to 10 mV of forward-voltage mismatch (2.6 dB against 8.9 dB with matched diodes); on 14 holdout captures 8 mV gives 3.8 dB against 9.9 dB | **8 mV diode mismatch** in the model. The data sheet's forward-voltage window is 100 mV wide. The rms error stays at 10 dB whatever the value: the rest is the captures' floor |
| Bias current, offset, transistor gain | 500 nA (the data-sheet maximum) would give -25 to -38 dB, far more than the reference shows; offsets of 6 mV change nothing; a current gain of 70 or 700 changes the second harmonic by under 3 dB at these levels | typical values kept |
| Treble: 1 dB more loss at 4.4 kHz, third harmonic of the 4.4 kHz tone 8 dB low | the capture low-pass of 7.1: a **capture property** | none. The diodes' capacitance (0.87 pF each) is in the model and is worth 0.1 dB |
| Clipped level 0.3 to 1 dB higher, onset harmonics off | Diode saturation current: not identifiable (a 3.6-fold change moves the training cost by 6 %, because it trades against the unknown input level). Emission coefficient: best at the data-sheet value (cost 975 / 971 / 985 at 0.88 / 1.0 / 1.15 times). Binned by level above each setting's compression point, the mean third-harmonic error at 110, 440 and 990 Hz is within 0.9 dB from 9 dB below the onset to 48 dB above it, and the fifth within 1 dB where it is above -60 dB | the data-sheet diode stays. The "several dB near the onset" of round 1 was scatter around a mean of zero |
| Early effect and current gain | not visible in the reference (it stops at 0.2 V in); decided against ngspice (5.1) | Early effect in the model; current gain 200 |

### 7.4 Result: G-P1r, whole

Prototype at 264.6 kHz. rms / worst over the captures of each group, dB. Each holdout set at its own input level.

| quantity | gate (worst holdout capture) | train (22) | set 2 (11) | set 5 (6) | set 4, Level max (10) | all holdouts (27) | |
|---|---|---|---|---|---|---|---|
| gain against drive (990 Hz, low level) | 1.0 | 0.51 / 0.90 | 0.37 / 0.85 | 0.36 / 0.58 | 0.67 / 1.23 | 0.50 / 1.23 | **fail** |
| mid hump: 110 Hz re 990 Hz | 1.0 | 0.56 / 0.87 | 0.26 / 0.42 | 0.21 / 0.30 | 0.29 / 0.44 | 0.26 / 0.44 | pass |
| top: 4400 Hz re 990 Hz | 1.0 | 0.78 / 1.52 | 1.09 / 1.52 | 1.12 / 1.27 | 0.83 / 1.16 | 1.01 / 1.52 | **fail** |
| clipping onset against drive | 1.0 | 0.28 / 0.59 | 0.55 / 1.10 | 0.39 / 0.64 | 0.58 / 1.12 | 0.53 / 1.12 | **fail** |
| third harmonic above -25 dB | 2.0 | 0.94 / 4.78 | 0.79 / 3.12 | 0.99 / 2.79 | 1.01 / 3.05 | 0.89 / 3.12 | **fail** |
| third harmonic, -40 to -25 dB | 2.0 | 3.24 / 12.9 | 2.75 / 7.56 | 3.21 / 7.39 | 4.15 / 11.0 | 3.51 / 11.0 | **fail** |
| third harmonic of the 4400 Hz tone | 2.0 | 8.00 / 16.7 | 7.16 / 16.3 | 10.6 / 13.3 | 9.00 / 16.7 | 8.14 / 16.7 | **fail** |
| fundamental level, all steps (no gate) | - | 0.69 / 3.28 | 0.62 / 2.96 | 0.61 / 2.38 | 0.86 / 2.95 | 0.72 / 2.96 | |

**One of seven gates passes; six fail on the worst holdout capture.** The gates were not moved.

With the holdout sets held at the training sets' input level instead, the worst cases are gain 1.72, hump 0.44,
top 1.50, onset 4.35, third harmonic 5.51 / 10.7 / 16.7 dB.

Diagnostic, which is **not** passing (like the front high-pass for the amp captures, it models
the capture, not the pedal): with the 10.6 kHz capture low-pass applied to the model, third harmonics scored only
12 dB or more above each capture's own floor and only below 10 kHz, the holdouts' worst cases are gain 1.27
(fail), hump 0.40 (pass), **top 0.87 (pass)**, onset 1.12 (fail), third harmonic 2.76 (fail) and 9.74 (fail);
the 4400 Hz tone's third harmonic is not scored.

### 7.5 Within the reference's validity

Published next to 7.4 and, like it, not a pass. Same simulations, same fit. Differences from G-P1r as written:
scored up to 5 kHz (all five fundamentals are; third harmonics only up to 5 kHz, so those of 110, 440 and 990 Hz);
harmonics only where the capture's is 12 dB or more above that capture's own floor; each session's input level
fitted (as in 7.4); limits no tighter than the reference's own scatter:

- level quantities: 2 standard deviations of the largest capture-to-capture scale scatter inside one session,
  2 x 0.66 = **1.32 dB**;
- harmonics: the reading error that a floor 12 dB down can cause, 20 log10(1 - 10^(-12/20)) = **2.51 dB**.

rms / worst over the captures of each group, dB:

| quantity | limit (worst holdout capture) | train (22) | set 2 (11) | set 5 (6) | set 4, Level max (10) | all holdouts (27) | |
|---|---|---|---|---|---|---|---|
| gain against drive | 1.32 | 0.51 / 0.90 | 0.37 / 0.85 | 0.36 / 0.58 | 0.67 / 1.23 | 0.50 / 1.23 | within |
| mid hump: 110 Hz re 990 Hz | 1.32 | 0.56 / 0.87 | 0.26 / 0.42 | 0.21 / 0.30 | 0.29 / 0.44 | 0.26 / 0.44 | within |
| top: 4400 Hz re 990 Hz | 1.32 | 0.78 / 1.52 | 1.09 / 1.52 | 1.12 / 1.27 | 0.83 / 1.16 | 1.01 / 1.52 | **over** |
| clipping onset against drive | 1.32 | 0.28 / 0.59 | 0.55 / 1.10 | 0.39 / 0.64 | 0.58 / 1.12 | 0.53 / 1.12 | within |
| third harmonic above -25 dB (389 readings on holdouts) | 2.51 | 0.63 / 2.65 | 0.66 / 3.03 | 0.97 / 2.79 | 0.85 / 3.05 | 0.78 / 3.05 | **over** |
| third harmonic, -40 to -25 dB (99 readings) | 2.51 | 1.21 / 5.56 | 1.10 / 2.24 | 0.42 / 0.94 | 1.78 / 4.75 | 1.37 / 4.75 | **over** |

Four of six within, two harmonic rows and the top row over on the worst single reading. The top row is the
capture low-pass (-0.9 dB at 4.4 kHz, 7.1), which this table does not remove because 4.4 kHz is below 5 kHz.
Neither table is a pass; the deciding test is blind listening.

### 7.6 The reference unit: final values and final scores

This step makes the captures the target and allows every component its real tolerance and the semiconductors their
data-sheet spread, as one physical unit's values. 7.4 and 7.5 above are the model with nominal components; this
section is the model as specified for the engine.

**Fit (training sets only).** Twelve linear parts and the knob mapping, against 124
low-level gains of the 22 training captures up to 2.2 kHz (above that the captures' low-pass begins, which
must not be fitted), each part pulled weakly toward its nominal value so that it moves only if the data ask:
rms 0.46 dB with nominal parts, **0.12 dB** fitted (worst 0.40 dB).

| part | nominal | reference unit | deviation | allowed |
|---|---|---|---|---|
| `Rg` (gain leg) | 4.7 k | 4.655 k | -1.0 % | 5 % |
| `Cg` (gain leg) | 47 nF | 43.42 nF | -7.6 % | 10 % |
| `Rf` (feedback) | 51 k | 50.32 k | -1.3 % | 5 % |
| `Cf` (feedback) | 51 pF | 49.96 pF | -2.0 % | 10 % |
| `Rlp` | 1 k | 1.007 k | +0.7 % | 5 % |
| `Clp` (0.22 uF tantalum) | 0.22 uF | 0.2536 uF | +15.3 % | 20 % |
| `Ct` (0.22 uF tantalum) | 0.22 uF | 0.2063 uF | -6.2 % | 20 % |
| Tone pot, total | 20 k | 19.76 k | -1.2 % | 20 % |
| Drive pot, total | 500 k | 481 k | -3.8 % | 20 % |
| op-amp gain-bandwidth | 3 MHz | 2.835 MHz | -5.5 % | 2 to 4 MHz |
| `Rp2`, `Rt`, `Rfb2` | 10 k, 220, 1 k | unchanged (under 0.1 %) | | 5 % |
| clipping diodes: mismatch | 0 | 8 mV | (7.3) | 100 mV window |
| clipping diodes: saturation current, emission coefficient | 4.31 nA, 1.944 | unchanged | not identifiable: the training cost is 222.2 / 222.1 / 222.1 / 222.7 at 0.85 / 0.92 / 1.0 / 1.08 times the emission coefficient, each trading against the input level | |
| transistors, bias current, offsets | | unchanged | not visible in the reference | |

The largest move, `Clp` +15 %, lowers the treble from about 700 Hz upward. It may be carrying some of the capture
low-pass's first tenths of a decibel below 2.2 kHz; it is inside a tantalum's tolerance either way.

**Holdouts, scored once after the fit** (prototype at 264.6 kHz; rms / worst, dB; each holdout set with its own tone
position and input level, set 4 also its output scale; fitted full scale 0.611 V peak).

G-P1r as written:

| quantity | gate | train (22) | set 2 (11) | set 5 (6) | set 4, Level max (10) | all holdouts (27) | |
|---|---|---|---|---|---|---|---|
| gain against drive | 1.0 | 0.31 / 0.66 | 0.30 / 0.69 | 0.38 / 0.59 | 0.69 / 1.33 | 0.49 / 1.33 | **fail** |
| mid hump: 110 Hz re 990 Hz | 1.0 | 0.33 / 0.57 | 0.27 / 0.43 | 0.14 / 0.22 | 0.08 / 0.12 | 0.19 / 0.43 | pass |
| top: 4400 Hz re 990 Hz | 1.0 | 0.61 / 1.33 | 0.64 / 1.04 | 0.91 / 1.06 | 0.64 / 0.98 | 0.71 / 1.06 | **fail** |
| clipping onset against drive | 1.0 | 0.27 / 0.60 | 0.56 / 1.10 | 0.34 / 0.54 | 0.50 / 1.01 | 0.50 / 1.10 | **fail** |
| third harmonic above -25 dB | 2.0 | 0.91 / 4.30 | 0.78 / 2.78 | 0.91 / 2.85 | 0.81 / 2.74 | 0.81 / 2.85 | **fail** |
| third harmonic, -40 to -25 dB | 2.0 | 3.15 / 12.7 | 2.71 / 7.19 | 3.19 / 7.23 | 4.03 / 10.9 | 3.43 / 10.9 | **fail** |
| third harmonic of the 4400 Hz tone | 2.0 | 7.88 / 16.6 | 7.11 / 16.1 | 10.6 / 13.5 | 9.02 / 16.7 | 8.12 / 16.7 | **fail** |
| fundamental level, all steps (no gate) | - | 0.53 / 3.40 | 0.50 / 2.92 | 0.50 / 2.53 | 0.82 / 3.22 | 0.64 / 3.22 | |

Within the reference's validity (7.5's rules; the session scatter is 0.67 dB here, so the level limit is 1.34 dB):

| quantity | limit | train (22) | set 2 (11) | set 5 (6) | set 4, Level max (10) | all holdouts (27) | |
|---|---|---|---|---|---|---|---|
| gain against drive | 1.34 | 0.31 / 0.66 | 0.30 / 0.69 | 0.38 / 0.59 | 0.69 / 1.33 | 0.49 / 1.33 | within |
| mid hump | 1.34 | 0.33 / 0.57 | 0.27 / 0.43 | 0.14 / 0.22 | 0.08 / 0.12 | 0.19 / 0.43 | within |
| top: 4400 Hz re 990 Hz | 1.34 | 0.61 / 1.33 | 0.64 / 1.04 | 0.91 / 1.06 | 0.64 / 0.98 | 0.71 / 1.06 | within |
| clipping onset | 1.34 | 0.27 / 0.60 | 0.56 / 1.10 | 0.34 / 0.54 | 0.50 / 1.01 | 0.50 / 1.10 | within |
| third harmonic above -25 dB (389 readings) | 2.51 | 0.74 / 2.34 | 0.71 / 2.40 | 0.89 / 2.85 | 0.60 / 2.63 | 0.71 / 2.85 | **over** |
| third harmonic, -40 to -25 dB (99 readings) | 2.51 | 1.44 / 4.45 | 1.64 / 3.35 | 0.77 / 1.48 | 1.38 / 3.48 | 1.38 / 3.48 | **over** |

What the unit fit did and did not do, against 7.4 (nominal parts), on the holdouts:

- better: fundamentals 0.72 to 0.64 dB rms; hump 0.26 to 0.19; top 1.01 / 1.52 to 0.71 / 1.06; third harmonics
  slightly (strong: worst 3.12 to 2.85);
- unchanged: clipping onset (worst 1.12 to 1.10) and its rms (0.53 to 0.50);
- not closed: **gain against drive, worst 1.23 to 1.33 dB.** The worst capture is in the Level-maximum session,
  whose captures scatter by 0.67 dB in scale among themselves (rms there 0.69); on the two Level-6 holdout sets
  the worst is 0.69 dB. No component of the pedal can remove a scatter between captures of one session.
- G-P1r as written still fails six of seven; the validity table has four of six within, and its level rows now
  all within, the gain row by 0.01 dB of a limit that is itself that session's scatter.

Why each failing gate fails:

- **top, third harmonic of the 4.4 kHz tone:** the capture low-pass (7.1). The model cannot and should not follow it.
- **gain (1.23) and onset (1.12):** the worst capture is in the Level-maximum set in both, whose captures scatter
  by 0.66 dB in scale among themselves; on the two Level-6 holdout sets the worst cases are 0.85 and 1.10 dB.
- **third harmonics:** by rms the strong ones are within 1 dB and the mean error is under 1 dB at every level
  above the onset; the worst single points are 3 dB (strong) and 11 dB (near the floor). A gate on the worst
  single reading of a harmonic 25 to 40 dB down cannot be met against captures whose own error is -40 dB.

What can be said with confidence: across a drive sweep in which gain moves by 19 dB and the clipping onset by
23 dB, on captures the mapping was not fitted to, the model follows a real unit's gain within 0.5 dB rms, its mid
hump within 0.3 dB, its onset within 0.5 dB and its third harmonic within 1 dB where it is strong. What cannot be
checked with this reference: anything above 5 kHz, harmonics below about -40 dB, levels above 0.2 V peak.

## 8. Cost, and the four-cluster split

**The split (`pedal.split`, `SplitSim`), solved in order within one sample, no delay anywhere:**

| cluster | contents | ports | states |
|---|---|---|---|
| O1 | input buffer up to the clipping amplifier's input (that input draws only its bias current: an exact cut) | 2 | 2 |
| O2 | clipping amplifier, loaded by a linear copy of the tone stage | 3 | 6 |
| O3 | tone stage driven by O2's output voltage, loaded by a linear copy of the level network | 2 | 5 |
| O4 | level control and output buffer, driven by O3's output voltage | 2 | 3 |

PEDAL.md's round-1 proposal said seven scalar ports; the correct Early effect costs one more per follower: nine.
Each cluster must start from the operating point of the one before (without that, a 0.7 mV step at start-up took
a second to settle).

**It matches the one-cluster model:** waveform difference at the output between -92 and -178 dB over sines from
0.1 to 3 V, a 2 V square wave and 3 V noise, at three rates (the -92 dB cases are those where an op-amp saturates
and the linear copies stop being exact); gate G-P1's harmonics are identical to the third decimal (section 6).

**Cost.** A C port of the kernel (private bench, clang -O3, one thread; the machine was busy, load average 5 to 6,
so these are upper bounds), 4 s of decaying low notes with partials, at 288 kHz:

| variant | noon, 0.5 V peak | boost, 0.5 V peak | everything at 10, 1.9 V peak |
|---|---|---|---|
| all four clusters in full, tolerance 1e-7 V | 476 ns = 13.7 % | 511 ns = 14.7 % | 512 ns = 14.7 % |
| **cheap mode with guard**, tolerance 1e-6 V | 212 ns = 6.1 % | 230 ns = 6.6 % | 269 ns = 7.7 % |
| same, tolerance 1e-4 V (the head's) | 159 ns = 4.6 % | 163 ns = 4.7 % | 185 ns = 5.3 % |

- "Cheap mode with guard": while an op-amp's input difference is under 2 mV and its internal node is more than 1 V
  clear of the rails, its tanh is a straight line and its clamp carries nothing, so O2 runs with one port and O3
  with none. Two guard outputs per cluster check this every sample; when it fails the sample is redone with the
  full cluster on the same states. On the 1.9 V signal 5.2 % of the samples needed the full clusters. The output
  matches the prototype to -126 dB or better in every row (the last Newton step is applied to first order, as in
  the head).
- **The 2 % budget is not met: 4.6 to 5.3 % measured in the cheapest correct form tried.** The two followers
  take about half of that (two exponentials and a 2x2 solve each, per iteration). Still available to the C++ port and not
  tried: one exponential per transistor instead of two, tabulated device laws as in the head, and closed-form
  solutions of the one-junction clusters. Our estimate for all of that is about 3 % of a core; it is an estimate.
- Worst case: full-band 3 V noise needs about 66 Newton iterations per sample in O2; the engine needs the head's
  bounded-work rule there.
- The prototype in numba runs the one-cluster model at 0.7 to 1.0 us per sample (2.35 us for round 1's 12 ports).

Options at this point: raise the pedal's budget to 5 %, or accept the pedal without the followers' Early effect and
the op-amps' tanh in real time (about 3 %; both matter only above 1 V in), or reach 3 % in the C++ port. What the
port does is in PEDAL_SPEC.md section 8.

## 9. What fails, and what is still open

- **G-P1r as written: six of seven gates fail** on the worst holdout capture, with the fitted reference unit
  (7.6): gain 1.33, top 1.06, onset 1.10 against 1.0 dB; third harmonics 2.85, 10.9 and 16.7 against 2.0 dB. Within
  the reference's validity: the two harmonic rows are over (2.85 and 3.48 against 2.51 dB), everything else within.
  Neither table is a pass. By rms on the holdouts: gain 0.49, hump 0.19, top 0.71, onset 0.50, strong third
  harmonic 0.81 dB.
- **Cost: 4.6 to 5.3 %** measured (section 8); the ceiling is 5 % and the aim 3 %.
- G-P1 passes whole on the final values (one cluster and four); the 0.5 dB modelling rule (5.1) passes whole (checked on the nominal-value
  model before the unit fit; not repeated after it).
- Not identifiable from the reference, left at data-sheet values: diode saturation current and emission
  coefficient, transistor gain, Early voltage, op-amp bias current and offsets.
- Open: `Cc3` and the bias path of the output buffer; the Tone and Level tapers (the reference cannot show them);
  the original diode type; op-amp output swing at 9 V (extrapolated); the Electrosmash analysis unread; no
  manufacturer's drawing.
- The reference unit's values describe one pedal as seen through neural captures made by a third party; the 8 mV
  diode mismatch has no determined sign, and `Clp`'s +15 % may include a little of the capture chain.
- Blind listening clips were made and then dropped.
- The chain's aliasing with the pedal engaged (gate G-P3) needs the engine and was not measured.

The specification for the engine is `docs/research/PEDAL_SPEC.md`.
