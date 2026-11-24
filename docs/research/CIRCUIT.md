# The circuit we model

Status: research baseline for the implementers. Everything below was read by us from the factory
drawing and then checked by simulation; it is written in our own words. The drawing itself is not
in this repository and must not be added to it (see "Sources").

Second reading (WP9, 22 Nov 2026): the three sheets were re-read independently, value by value and
connection by connection, at 400 dpi. Result: **no reading error was found**; two connections that
the first reading marked B are now A (relay K1 and the feedback tap, section 10); and **two values
of the lead path are changed away from the drawing** on measured evidence (C17/R29 and C2,
section 4, and docs/MODEL.md section 2). Tube data and the transformer and speaker values of sections 7 and 9
were replaced by data-sheet fits and published measurements (`DEVICES.md`).

Confidence scale used in the tables:

- **A** value and connection read unambiguously from the factory drawing at high zoom;
- **B** read from the drawing, but a connection or a switch state needed interpretation;
- **C** not on the drawing: taken from a measurement somebody published;
- **E** estimate of ours, to be treated as a bounded fit parameter.

## 1. Sources

| id | what | how we used it |
|----|------|----------------|
| S1 | The maker's factory schematic of the reference head, drawing 81501510, three sheets dated 31 Jan / 5 Feb 1992, titled for the preamp and power boards under the model's pre-production working name. [N12] (title and public copy in the private notes) | Primary source for every value marked A or B. Rasterised at 400 dpi and read region by region. |
| S2 | Owner's manuals of the original head and of its later revision. [N13] (source in the private notes) | Control list, front-panel order, level specifications, what differs on the later revision. |
| S3 | Technician measurements posted on music-electronics-forum.com (threads 19276, 1357, 33251, 31446, 969213) and the judyboxamp blog. | Supply, bias and transformer numbers that the drawing does not print. Marked C. |
| S4 | N. Koren, "Improved vacuum tube models for SPICE simulations", Glass Audio 8(5), 1996 (author's web copy). | Plate-current law and parameters for 12AX7 and 6L6GC. |
| S5 | K. Dempwolf, U. Zölzer, "A physically-motivated triode model for circuit simulations", DAFx-11. | Grid-current law and its 12AX7 parameters. |
| S6 | General Electric 6L6GC and 12AX7 data sheets. | Operating points used to sanity-check S4, inter-electrode capacitances. |
| S7 | Rod Elliott (sound-au.com) pickup level measurements; the reference driver's data sheet [N6] (source in the private notes). | Input-level and speaker-load plausibility. |
| S8 | The maker's factory schematic of the later revision's preamp board (drawing dated 15 Feb 2001). [N14] (title and public copy in the private notes) | The lead channel only: it is the same chain of stages as on S1, drawn nine years later. Used for the two lead-path values that the captures contradict on S1 (section 4). |
| S9 | Two neural captures of block-letter heads (private), probed with tone bursts. | Measured response ahead of and after the clipping stages of two real amplifiers. |

No voltage chart exists on S1, and we found none published elsewhere. All DC voltages in section 8
are therefore *simulation results* that are checked against the few measured numbers in S3.

### What "block letter" means electrically

The early (1992 to about 1994) heads carry the logo in block capitals, later ones the signature
logo; in 2004/05 the maker renamed the model. We looked for evidence of circuit revisions between them and
found none: the reports are of a different PCB colour, pot style and transformer lead colours, an
output-transformer vendor change described by the designer as "same spec", and different factory
tubes (Sylvania-made 6L6GC early, Chinese/Ruby later). Our model follows S1, which is the 1992
drawing, so it is a block-letter model by construction. The tube difference is the one real
electrical difference; it lives in the tube parameter sets (section 9), not in the netlist.

The later revision (2001, also renamed in 2004/05) is a different amplifier for our purposes: a sixth 12AX7 with a separate clean
preamp, separate EQ and presence/resonance per channel, an adjustable bias, a higher supply (555 V
plate was measured on one), and smaller coupling/bright capacitors in the lead path. None of that
is modelled.

## 2. Signal path at a glance

```
IN -> V1A -> [rhythm pre-gain net | lead pre-gain net] -> V1B -> V2A (39k cathode, "cold") -> V2B
   -> 1M/100k divider -> V5B -> V5A (unity-gain shunt-feedback stage) -> 470k/33k divider
   -> tone stack -> post-gain pots (rhythm | lead) -> V3B (cathode follower) -> FX send/return
   -> V3A -> PI (V4, long-tailed pair) -> 4 x 6L6GC push-pull -> output transformer -> speaker
                         ^------------- 39k -- resonance -- presence ----- 8 ohm tap
```

Things that are easy to get wrong and that we checked on the drawing more than once:

1. **Both channels share every tube.** There is no separate clean preamp. The channel relays only
   select which pre-gain network feeds V1B, switch V1B's cathode bypass, add a shunt on V2A's grid,
   and select which post-gain pot feeds V3B.
2. **V5 sits between V2B and the tone stack.** V2B's plate goes through 1 M into a 100 k grid leak
   (a 21 dB pad) to V5B; V5B feeds V5A through 1 M, and V5A has 1 M from its output back to its
   grid, so it is an inverting stage of gain just under one with a low output impedance. Its output
   reaches the tone stack through 470 k with 33 k to ground (a 24 dB pad that also sets the stack's
   source impedance at about 31 k).
3. **The tone stack is not the textbook one.** The mid capacitor goes to the *wiper* of the mid pot,
   and a fixed 47 k parallels the mid pot (section 5).
4. **Negative feedback comes from the 8 ohm tap** whatever the impedance switch says (confidence A:
   on sheet 2 the feedback pads are wired to the 8 ohm transformer lead, which reaches the jacks only
   through the middle position of the selector).
5. **The standby switch is in the screen/preamp feed**, after a 400 ohm 10 W resistor from the plate
   supply. There is no choke.

## 3. Input and V1A

| ref | value | connection | conf |
|-----|-------|------------|------|
| R27, R30 | 22 k each | from each input jack tip to the common node B. With only the high-gain jack used, the other jack's switch ties both tips together, so the two resistors are in parallel (11 k). With only the normal jack used, the unused jack's switch grounds R27, giving a 6 dB pad. | A |
| C20 | 39 pF | node B to ground | A |
| C15 | 0.1 uF | node B to node C (DC block) | A |
| R25 | 1 M | node C to ground | A |
| R22 | 68 k | node C to V1A grid | A |
| R2 | 220 k | V1A plate to rail V1 | A |
| R12 / C3 | 1.82 k / 1 uF | V1A cathode to ground | A |
| C6 | 22 nF | V1A plate to node D (feeds both pre-gain networks) | A |

## 4. Pre-gain networks, V1B, V2A, V2B, V5

Both networks hang on node D permanently; relay K2A picks the wiper that drives V1B through R82.

Lead network:

| ref | value | connection | conf |
|-----|-------|------------|------|
| C17 | **470 pF** (S1 prints 2.2 nF) | D to E | A on S1, value replaced: see below |
| R29 | **not fitted** (S1 prints 470 k across C17) | D to E | as C17 |
| VR2 (lead pre) | 1 M, "5%A" taper | E (top), wiper F, ground | A (taper E) |
| R41 | 1 M | E to ground (parallels the pot) | A |
| C19, R40 | 1 nF, 1 M | E to F (bright cap and resistor across the top of the pot) | A |

**C17, R29 and C2 differ from the drawing in the default value set, provisionally (the engine also carries the as-drawn set).** S1 is a pre-production drawing (5 Feb 1992; the title
block still carries the working name). Two captured block-letter heads, owned and captured by
different people, one from the speaker output and one (per its author's page; unverified, the
file itself says "amp") from a preamp output, both show a response
ahead of the clipping stages that falls 30 dB from 1 kHz to 95 Hz and 38 dB to 60 Hz. With the S1
values (470 k // 2.2 nF, then 22 nF from V1B to V2A) the netlist gives 19 and 22 dB at any gain
setting, and no other single element of the early stages closes the gap. The maker's later drawing of
the same lead channel (S8) has 470 pF alone in series at C17, no R29, and 1 nF at C2. With exactly
those values the model follows both captures to about 1 dB from 60 Hz to 7 kHz, and halving or
doubling either capacitor makes the fit worse. The S1 values remain
available as `PHYS` overrides (`c17=2.2e-9, r29=470e3, c2=22e-9`). No block-letter board was
inspected by us: confidence C (measurement), not A. A different high-pass ahead of each amplifier
in the two capture chains fits the same measurements as well; docs/MODEL.md section 2 says what
would separate the two.

Rhythm network (K4 is the crunch relay, driven by the crunch push-button; the bright push-button is
the other half of the same dual switch):

| ref | value | connection | conf |
|-----|-------|------------|------|
| R19 // C51 | 470 k // 270 pF | D to G, crunch **off** | B |
| R43 // C13 | 2.2 M // 470 pF | D to G, crunch **on** | B |
| R21 | 1 M | G to J | A |
| C10, R75 | 470 pF, 2.2 M | G to K; K to J. The bright switch shorts R75, putting C10 directly across R21 | B |
| R23 + C63 | 39 k + 0.1 uF | J to ground, crunch **off** only | B |
| VR1 (rhythm pre) | 1 M, "5%A" | J (top), wiper, ground | A |
| C27 | 180 pF | J to wiper | A |

Which relay state is "off" is an interpretation (B): the drawing shows contacts, not the idle
position. Our reading gives a clean network that sits about 32 dB below V1A's plate and a crunch
network about 12 dB below it rising to 6 dB below above a few hundred hertz, i.e. crunch adds
roughly 20 to 26 dB. The II manual quotes 24 dB for the same function, which supports the reading.

| ref | value | connection | conf |
|-----|-------|------------|------|
| R82 | 470 k | selected wiper to V1B grid | A |
| R3 | 100 k | V1B plate to V1 | A |
| R17 / C4 | 1.82 k / 1 uF | V1B cathode to ground | A |
| C26 + R18 | 22 uF + 47 k | V1B cathode to ground; relay K1A shorts R18 on the **rhythm** channel (full bypass), so the lead channel has only the 1 uF | A (section 10) |
| C2 | **1 nF** (S1 prints 22 nF) | V1B plate to R6 | C, see the note on C17 above |
| R6, R9 | 470 k, 1 M | series to V2A grid, grid to ground | A |
| R20 | 220 k | V2A grid to ground through K1B, **rhythm** channel only | A (section 10) |
| R1 | 100 k | V2A plate to V1 | A |
| R15 | 39 k | V2A cathode to ground, no bypass (the cold clipper) | A |
| C1 | 22 nF | V2A plate to node n1 | A |
| C48, R11 | 1 nF, 330 k | n1 to ground | A |
| R7 | 220 k | n1 to V2B grid | A |
| R4 | 220 k | V2B plate to V2 | A |
| R13 / C5 | 1.82 k / 1 uF | V2B cathode | A |
| R32, C57, R101 | 1 M, 22 nF, 100 k | V2B plate - R32 - C57 - V5B grid; R101 grid to ground | A |
| R96, R102 | 220 k, 2.2 k | V5B plate load to V2; cathode, no bypass | A |
| R87, C56 | 1 M, 22 nF | V5B plate - R87 - C56 - V5A grid | A |
| R91 | 1 M | V5A grid to ground | A |
| R88 | 1 M | V5A grid to node X (feedback) | A |
| R86, R93 | 100 k, 2.2 k | V5A plate load to V2; cathode, no bypass | A |
| C58 | 22 nF | V5A plate to node X | A |
| R89 | 470 k | X to tone-stack input T | A |

## 5. Tone stack and post gain

| ref | value | connection | conf |
|-----|-------|------------|------|
| R24 | 33 k | T to ground | A |
| C59 + R94 | 10 nF + 47 k | T to ground | A |
| R28 | 47 k | T to U (slope) | A |
| C14 // C60 | 470 pF // 100 pF | T to top of the high pot | A |
| VR5 (high) | 250 k linear | top, wiper = stack output, bottom = W | A |
| C18 | 22 nF | U to W | B (drawn on the U-W wire; position read from the symbol) |
| VR3 (low) | 1 M audio | rheostat from W to Y | A |
| R95 | 47 k | Y to ground | A |
| VR4 (mid) | 50 k linear | Y (top), ground (bottom); wiper to C21 | A |
| C21 | 22 nF | U to mid wiper | A |
| VR6, VR7 (post rhythm, post lead) | 1 M audio each | both tops on the stack output, bottoms grounded; K2B selects a wiper | A |
| R81 | 2.2 M | selected wiper to ground | A |
| C52 | 22 nF | selected wiper to V3B grid | A |

A JFET (J174) clamps the selected wiper to ground for a moment during channel switching. It is
off in normal use and is not modelled.

## 6. Follower, loop, V3A, phase inverter

| ref | value | connection | conf |
|-----|-------|------------|------|
| V3B | 12AX7 half | plate straight to V2; cathode follower | A |
| R80, R16 | 1 k, 100 k | cathode - R80 - node - R16 - ground | A |
| R79 | 1 M | grid to the R80/R16 node (bootstrapped bias) | A |
| C53, R103, C65, R33 | 0.33 uF, 100 k, 0.33 uF, 100 k | cathode - C53 - (R103 to ground) - C65 - send; R33 send to ground | A |
| R85 | 220 k | return to ground | A |
| C11, R26 | 47 nF, 1 M | return - C11 - V3A grid; R26 grid to ground | A |
| R5 | 68 k | V3A plate to V2 | A |
| R14 // C61 | 3.3 k // 4.7 nF | V3A cathode (partial bypass, lifts the top octave) | A |
| C49 | 22 nF | V3A plate to node P | A |
| R104, C66, R100 | 100 k, 0.1 uF, 1 M | P to the preamp-out jack | A |
| R48 | 100 k | P to PI input grid (V4B) | A |
| R50, R53 | 1 M each | each PI grid to node Q | A |
| R52 | 470 | joined PI cathodes to Q | A |
| R51 | 10 k | Q to R (tail) | A |
| R57 | 4.7 k | R to ground (feedback is injected at R) | A |
| C31 | 47 nF | V4A grid to R | A |
| R46, R58 | 82 k, 100 k | PI plate loads to V3 (V4B, V4A) | A |
| C28, C29 | 47 nF | PI plates to the power-tube grid circuits | A |
| R54, R55 | 220 k | grid circuits to the bias supply | A |
| R56 | 39 k | from the feedback wire to the resonance network | A |
| VR8 // C12 (resonance) | 1 M "5%A" rheostat // 6.8 nF | in series after R56 | A |
| C30 | 22 uF | feedback node S to R | A |
| C8 // C62 + VR9 (presence) | 0.1 uF // 33 nF, then 10 k audio rheostat to ground | S to ground | A |

With the loop unused, send and return are joined by relay K3 (modelled as a wire). The loop relay
is not modelled as a control: the product has no FX loop.

## 7. Power amplifier and supply

### 7.1 Output stage

| ref | value | connection | conf |
|-----|-------|------------|------|
| 4 x 6L6GC | | two per side, cathodes grounded, fixed bias | A |
| R202, R204, R206, R208 | 2.2 k | grid stopper per tube | A |
| C201, C202 | 100 pF | each drive line to ground | A |
| R203, R205, R207, R209 | 100 ohm 5 W | screen resistor per tube | A |
| CR200, CR201 | HV rectifier | each plate line to ground, reverse-biased (flyback clamp) | A |
| R210 | 400 ohm 10 W | plate supply to the screen node, standby switch in series | A |

The model lumps each pair of tubes into one device carrying twice the current, with the per-tube
resistors halved and capacitances doubled. This is exact for matched tubes.

### 7.2 Supply

| ref | value | connection | conf |
|-----|-------|------------|------|
| bridge | 4 x 1N4007 | HT winding to the plate node | A |
| C34 + C38 | 2 x 330 uF 350 V in series (165 uF) | plate node | A |
| C35 + C39 | 2 x 100 uF 350 V in series (50 uF) | screen node | A |
| R59 / C36 | 4 k 5 W / 47 uF | screen node to V3 (phase inverter) | A |
| R60 / C37 | 22 k / 22 uF | V3 to V2 (V2B, V5, V3) | A |
| R61 / C33 | 10 k / 22 uF | V2 to V1 (V1A, V1B, V2A) | A |
| bias | half-wave from a separate winding, 3 x 10 uF, 1 k, 2.7 k, 15 k to ground; no trim pot | 220 k feeds | A |

Not on the drawing, from S3: HT winding about 375 V AC loaded at idle; plate voltage at idle 465 to
481 V on four heads (we use 475 V); idle cathode current 15 to 18 mA per tube measured on one
later-named head, "11 to 14 mA" quoted as typical; bias "about -55 V" quoted but not metered; primary copper
resistance 29.5 and 37.5 ohm per half. Confidence C.

### 7.3 Output transformer

The maker's part 70500207. No data sheet. The only data on the actual part is a published tear-down:
1348 primary turns against 109 (16 ohm), 77 (8 ohm) and 54 (4 ohm) secondary turns, about 50 H of
primary inductance at 60 Hz and 100 V, nine interleaved winding sections (`DEVICES.md` section 5).
The turns give **2.45 k** plate to plate; the rated 120 W from four 6L6GC on 475 V agrees (the
data-sheet-fitted tube model delivers 133 W into 2.45 k from a stiff supply, 113 W into 4.2 k).
The forum figure of 4.2 k that the first version of this document quoted has no stated method and
is dropped. Values used: 2.45 k (range 2.2 to 3.0 k), 50 H, secondary resistance 0.35 ohm on the
16 ohm tap (comparable 100 W parts: 0.23 to 0.42 ohm; confidence C). Leakage (6 mH referred to the
primary) and winding capacitance (300 pF per half) were not found for this part and remain
estimates (E); comparable parts scaled to 2.45 k give 5 to 11 mH.

The core is modelled as linear. Published work that tried hysteresis models on a guitar push-pull
stage found a small effect confined to the lowest frequencies at a large cost; we follow that and
list it under risks.

### 7.4 Speaker load

The default load is a 16 ohm 4x12-style equivalent. Voice coil: 12.9 ohm (reference driver data sheet [N6])
in series with **two** semi-inductive sections, 1.245 mH parallel 11.11 ohm and 0.5784 mH parallel
80.59 ohm, fitted to the median of published impedance curves of 12-inch guitar speakers by the reference driver's maker
(0.17 dB rms from 2 to 10 kHz; the rise goes as about the square root of frequency, which one
inductor cannot follow: the single 1.6 mH section used before was up to 3.5 dB too high at 5 to
8 kHz). Resonance branch: 85 ohm, 31 mH, 68 uF in parallel (110 Hz); the frequency has published
support (107 to 118 Hz), the peak value does not (E).

A capture is normally taken into a reactive load box, not a cabinet, and no published numeric
curve of one was found. When the model is scored against a capture the load is therefore part of
the unknown capture chain: its resonance, peak and first inductive section are bounded fit
parameters. On the main capture they come out as 94 Hz, about 125 ohm and
0.6 mH. They are not the product default.

## 8. Simulated DC operating point

Independent agreement between ngspice and our solver is better than 20 mV on every node. Supply
model: 487 V behind 160 ohm (E); bias set to -63.35 V, which gives the measured 16 mA idle current
per tube with the data-sheet-fitted 6L6GC law (the commonly quoted -55 V would give about 30 mA).

| node | simulated | published check |
|------|-----------|-----------------|
| plate supply / screen node | 474.8 / 469.8 V | 465 to 481 V measured (S3) |
| 6L6GC idle current | 16.0 mA plate, 0.8 mA screen per tube | 15 to 18 mA measured (S3) |
| preamp rails V3 / V2 / V1 | 433 / 305 / 288 V | none published |
| 12AX7 plates | V1A 140, V1B 198, V2A 279, V2B 149, V5B 160, V5A 219, V3A 256 V; PI 278 and 289 V | none published |
| 12AX7 cathodes | 1.2 to 2.4 V; cold stage V2A 3.4 V (88 uA); PI 50 V; follower 126 V | none published |

`scripts/gen_reference.py` regenerates the full table (`reference/dc_op.json`).

### Functional cross-checks against the owner's manuals

Small-signal results from the SPICE deck, compared with the only published level figures:

| manual statement | simulated |
|------------------|-----------|
| Bright: "+6 dB at 2 kHz" (rhythm channel) | +6.0 dB at 2 kHz, bright in versus out (+3.5 dB at 1 kHz) |
| Crunch: "subtract 24 dB" from the rhythm input sensitivity (stated for the II) | the crunch network is 14 to 22 dB hotter than the clean one, depending on frequency |
| Resonance: about +10 dB at the cabinet resonance | +11.2 dB at 110 Hz from 0 to 10 with our speaker model |
| Presence: about +10 dB (stated at 2 kHz for the II) | +4.9 dB at 2 kHz, +7.3 dB at 5 kHz from 0 to 10 with the two-section voice coil |
| Lead sensitivity: 1 mV for nominal output at pre 5 (stated for the II) | 86.7 dB from input to the fourth stage's plate at pre 5 and 1 kHz; 78.7 dB to the speaker with everything at 5 |

The bright figure is an independent confirmation of the reading of the dual push-button and of
C10/R21/R75. The others are consistent within what the unknown tapers allow.

Small-signal gain along the chain, lead channel, all knobs at 5 (post at 4), 1 kHz, in dB relative
to the input jack: V1A plate 36.1; V1B grid 15.1; V1B plate 50.1; V2A plate 50.5; V2B plate 86.7;
V5B plate 97.3; node X 96.1; tone-stack output 58.0; follower 33.0; V3A plate 55.4; 6L6GC grids
79.4; speaker 74.4. With that much gain ahead of it the fourth stage clips for inputs above a few
millivolts, so on this channel the small-signal figures only describe the noise floor and the
very end of note decays. At 95 Hz the gain to the fourth stage is 30 dB lower than at 1 kHz: the
lead channel removes most of the bass before it clips (section 4).

## 9. Device models

All parameter values and their sources (data sheet, page, figure; fit errors; what the forms cannot
reproduce) are in `docs/research/DEVICES.md`; the digitised points are in `docs/research/data/` and
`scripts/fit_devices.py` reproduces every fit. Nothing is quoted from memory any more.

### 9.1 12AX7 plate current

Koren's form with a constant added to the grid voltage:

    E1 = (Vpk / kp) * ln(1 + exp(kp * (1/mu + (Vgk + vct) / sqrt(kvb + Vpk^2))))
    Ip = 2 * E1^ex / kg1            (zero for Vpk <= 0)

mu 98.40, ex 1.8114, kg1 2783.6, kp 857.6, kvb 47.27, vct 0.7790: fitted to the GE and Philips
plate families (0.04 and 0.06 mA rms). Koren's published set (100 / 1.4 / 1060 / 600 / 300) is
0.22 mA rms off the same curves and loses most of the current at low plate voltage (0.10 mA where
both sheets print 0.48 mA at 100 V and -1 V), which put our plates 14 V rms away from the
data-sheet load lines.

### 9.2 12AX7 grid current

    Ig = gg * ( ln(1 + exp(cg * (Vgk + vg0))) / cg )^xi

gg 6.177e-4, cg 9.901, xi 1.314: Dempwolf and Zölzer's Table 1, tube RSD-1, as printed. vg0 = 0
reproduces the paper and is kept as a bounded fit parameter. No data sheet prints positive-grid
curves of a 12AX7.

### 9.3 Capacitances

Per triode: grid-plate 1.7 pF, grid-cathode 1.6 pF, plate-cathode 0.46 pF (GE sheet), plus 0.7 pF
stray on the first two for socket and traces (E; the totals equal the 2.4 and 2.3 pF used by
Koren and by Dempwolf and Zölzer). The grid-plate capacitance matters: multiplied by the stage
gain it forms a low-pass with the 470 k and 220 k series resistors that sets much of the amp's
treble roll-off ahead of the clipping stages. The two captures bracket the value: one is fitted
slightly better with less stray, the other with more.

### 9.4 6L6GC

    E1  = (Vg2k / kp) * ln(1 + exp(kp * (1/mu + Vgk / Vg2k)))
    I0  = (pi / kg1) * E1^ex                         space current (Koren)
    Vp' = ln(1 + exp(4 Vpk)) / 4                     smooth rectifier: Vpk above 1 V, 0 below -1 V
    a   = kv * Vp'^1.5                               the line all grid curves share below the knee
    m   = I0 * a / (I0^3 + a^3)^(1/3)                smooth minimum of the two
    Ip  = m * (1 + lam * Vp')
    Ig2 = I0 * (rs + asc * I0 / (I0 + a))

mu 10.828, ex 1.3921, kg1 1624.7, kp 30.29, kv 8.670e-4, lam 6.326e-4, rs 0.06434, asc 0.3661,
fitted to the Tung-Sol plate, screen and transfer families at 400 V screen (7 mA rms on the plate
family, 4 mA on the screen current). Koren's `atan(Vp/12)` knee, used before, put the knee near
25 V where the sheets show 60 to 80 V (49 mA rms, 105 mA rms in the knee region) and delivered
20 to 25 % too much power in the data sheet's own class AB1 examples; the new law is within 5 % of
those tables without having been fitted to them. Control-grid current: gg 1.4224e-3, xi 0.9732
(Tung-Sol, +4 to +20 V; nearly linear, a third of the earlier estimate at +20 V); the onset
sharpness cg = 8 is still an estimate (nothing is published below +4 V).

The smooth rectifier replaces a hard corner at zero plate voltage. With the hard corner Newton's
method cycled when the plate rang around zero volts on the inductive load.

### 9.5 Licence note

Only equations and numbers published in papers and data sheets are used. No code or tables from
GPL/LGPL or "all rights reserved" tube libraries were read into this project.

## 10. Open points

1. Relay idle states: **settled**. The channel relays K1 and K2 are in series with the red (lead)
   lamp, so they are energised on the lead channel, and the drawing shows contacts de-energised:
   K2 drawn on the clean pre and post pots confirms the convention, so K1 (drawn closed) is closed
   on rhythm. The crunch push-button grounds the K4 coil when pressed and is drawn open, so the
   drawn K4 contacts (470 k // 270 pF in series, 39 k shunt) are the clean state. The opposite
   reading of K1 is refuted by the drawing; `k1_swap` remains only as a test switch.
2. Feedback tap: **settled** as the 8 ohm tap (sheet 2, traced through the selector). Sheet 3, drawn
   five days earlier, shows a transformer with two taps and the feedback on its top, also marked
   8 ohm.
3. Production against drawing: the lead path differs in two values (section 4). Whether the rhythm
   networks, the tone stack or the power amplifier of production heads differ from the drawing as
   well is not known; the saturated response of both captures is matched to 0.2 to 0.3 dB by the
   drawing's tone stack, follower, V3A and power amplifier, so nothing there is contradicted.
4. Pot tapers: only codes are printed. "A" is taken as 10 % at half rotation and "5%A" as 5 %. The
   low pot carries the same suffix as the gain pots ("P5"), which may mean 5 % as well; the capture
   that is described as "everything at noon" is matched with low at 5.6 and mid at 2.0, so either
   the tapers or that description are off. Tapers only decide where on the dial a sound sits.
5. Transformer leakage and winding capacitance, the peak of the cabinet resonance, the supply
   source resistance and the bias voltage (set for the measured idle current).
6. Tube spread: real 12AX7 and 6L6GC differ from the average data-sheet curves by tens of percent.
7. Both captures roll off above about 7 kHz in the saturated response (2 to 6 dB at 9 kHz) where
   the model does not. If the second one was taken from the 100 k PREAMP OUT jack, as its author's page suggests, cable capacitance
   explains it; for the other the cause (load box, line output, transformer) is not identified.
