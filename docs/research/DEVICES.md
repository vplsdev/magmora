# Device and load data: what was read from where

Status: input to the model. This file replaces every tube operating point that was earlier
quoted from memory with numbers digitised from documents that were opened for this work. The
digitised points are in `docs/research/data/*.csv` (numbers only; no scans are in the repository)
and every figure quoted below is printed by `scripts/fit_devices.py`.

How the curves were read: each data-sheet page was rendered at 300 dpi, the axes were calibrated
on printed grid lines, and the centre of each printed curve was sampled between grid lines by a
tracer that follows one curve from a seed point. Every trace was overlaid on the page and
inspected; points where neighbouring curves merge were removed by hand-set limits. Reading
uncertainty is about half a line width: 1.5 V and 0.015 to 0.02 mA on the triode sheets, 2 V and
1.5 mA (plate) or 0.4 mA (screen, grid) on the 6L6GC sheets. Currents below about 0.05 mA cannot
be resolved on any 12AX7 sheet.

## 1. Sources

| id | document | used for |
|----|----------|----------|
| T1 | General Electric 12AX7, ET-T509B (6-53), page 3 "Average plate characteristics"; page 1 for capacitances and the two class A1 points | `12ax7_ge_plate.csv` |
| T2 | Philips Data Handbook, ECC83, January 1970, sheet 5 (drawing 7Z06098) | `ecc83_philips_plate.csv` |
| T3 | RCA 12AX7-A (10-60) and Sylvania 12AX7 (Sept. 1955) | opened; both print the same curve family as T1 (rotated), so they were not digitised again |
| P1 | Tung-Sol 6L6GC "tentative data", plates #5818 and #5819 (1 April 1960) and the following pages: plate, screen and control-grid current at 400 V screen, the zero-bias family for 50 to 400 V screen, transfer curves at 400 V plate | all `6l6gc_tungsol_*.csv` |
| P2 | General Electric 6L6-GC, ET-T515A (2-59), page 2 (typical operation tables), pages 4 to 8 (curve families that look the same as P1; not digitised) | operating points, independent check |
| P3 | RCA 6L6-GC (8-60) | typical operation tables agree with P2; its curves are for 300 V screen and were not digitised |
| K | N. Koren, "Improved vacuum tube models for SPICE simulations", author's web copy, Table 1 | the published parameter sets (12AX7: 100 / 1.4 / 1060 / 600 / 300; "6L6CG": 8.7 / 1.35 / 1460 / kg2 4500 / 48 / 12) |
| DZ | K. Dempwolf, U. Zölzer, "A physically-motivated triode model for circuit simulations", DAFx-11, eq. 10 to 12 and Table 1 | 12AX7 grid current, positive-grid plate current |
| CH | I. Cohen, T. Hélie, "Real-time simulation of a guitar power amplifier", DAFx-10, Tables 2 and 3 | confirms K's 6L6GC set; its grid-current law (threshold 13 V, 6 k) is a placeholder, not a measurement, and is not used |
| H1 | Hammond 1750U data sheet (100 W guitar output transformer, 1.7 k) | transformer parasitics |
| H2 | Hammond 1650T (120 W, 1.9 k) and 1650R (100 W, 5 k) data sheets | transformer parasitics |
| G | groupdiy.com thread 62249 on the reference head's output transformer (tear-down and rewind by one member, March 2016). [N15] (title in the private notes) | the only data found on the actual part |
| Z | M. Zollner, "Physics of the Electric Guitar" (GITEC translation), section 11.2, Fig. 11.9 | speaker impedance, `driver_8ohm_impedance_band.csv` |
| C | [N6] the reference driver's product sheet (source in the private notes) | Re 7.3 / 12.9 ohm, Fs 75 Hz (nothing else is published) |

A Sylvania 6L6GC sheet (the tubes the early heads shipped with) could not be downloaded; the
Tung-Sol and GE sheets show what appear to be the same curve families (only the Tung-Sol copy was
digitised).

## 2. 12AX7 plate current

The two makers agree with each other within 0.1 mA almost everywhere. The published Koren set
does not agree with either: it is correct near 250 to 300 V but loses current at low plate
voltage (at 0 V grid and 50 V plate the sheets give 1.15 mA, the set gives 0.72; at -1 V and 100 V
0.48 against 0.10). On the load lines our stages use (100 k and 220 k from 295 to 310 V) its
plate voltage is wrong by 14 V rms, up to 21 V.

Koren's form fits both sheets once the grid voltage is given a constant offset `vct` (contact
potential plus initial electron velocity, a standard extension):

    E1 = (Vpk / kp) * ln(1 + exp(kp * (1/mu + (Vgk + vct) / sqrt(kvb + Vpk^2))))
    Ip = 2 * E1^ex / kg1

| set | GE: rms / max mA | Philips: rms / max mA | Vg = 0, Va < 100 V (GE / Ph) | load-line plate voltage error, rms / max (GE; Ph) |
|-----|------------------|-----------------------|------------------------------|--------------------------------------------------|
| Koren published | 0.224 / 0.49 | 0.250 / 0.80 | 0.40 / 0.37 mA | 13.6 / 20.6 V; 13.8 / 20.5 V |
| Koren refit without offset (exponent runs into its bound of 1.0) | 0.076 / 0.25 | 0.061 / 0.20 | 0.14 / 0.09 | 2.4 / 5.3; 3.1 / 6.9 |
| **Koren + vct, fitted to both sheets** | **0.040 / 0.13** | **0.061 / 0.40** | **0.05 / 0.04** | **1.2 / 2.0; 2.0 / 4.9** |
| Dempwolf-Zölzer law with their Table 1 sets (three measured tubes) | 0.19 to 0.23 | 0.22 to 0.26 | 0.10 (RSD), 0.43 (EHX) | 9 to 14 V rms |

Recommended: mu 98.40, ex 1.8114, kg1 2783.6, kp 857.6, kvb 47.27, vct 0.7790.
Data-sheet points with it: 250 V / -2 V gives 1.19 mA, 1.62 mA/V, 60 k, mu 98 (sheet: 1.2, 1.6,
62.5 k, 100); 100 V / -1 V gives 0.48 mA, 1.08 mA/V, 91 k (sheet: 0.5, 1.25, 80 k). The published
set gives 0.95 mA and 0.10 mA at those two points.

What it does and does not change:

- The cold stage (39 k cathode) is *not* affected: its bias point is 88 to 90 uA at -3.5 V with
  every Koren variant, because the published set is right at high plate voltage.
- Stages whose plates swing low are affected: more current at low plate voltage means plates
  bottom lower and the quiescent plate voltages move by 10 to 20 V.
- Positive grid: no data sheet prints it. The recommended set extrapolates to within 10 % of the
  measured Dempwolf-Zölzer tube (cathode current minus grid current) from +0.5 to +2 V for plate
  voltages of 20 to 200 V, so no separate treatment is needed.
- Cut-off below 0.05 mA is not resolvable on the sheets. The fit's error for currents below
  0.3 mA is 0.02 to 0.03 mA rms.

## 3. 12AX7 grid current

No data sheet opened prints positive-grid curves. The law we use is DZ eq. 11 without its constant
term; DZ Table 1 gives, for three measured tubes:

| tube | Gg | xi | Cg | Ig0 (not used) |
|------|----|----|----|----------------|
| RSD-1 | 6.177e-4 | 1.314 | 9.901 | 8.025e-8 |
| RSD-2 | 5.911e-4 | 1.358 | 11.76 | 4.527e-8 |
| EHX-1 | 3.263e-4 | 1.156 | 11.99 | 3.917e-8 |

Recommended: the RSD-1 row as printed (gg 6.177e-4, cg 9.901, xi 1.314, vg0 0), with EHX-1 as the
other end of the tube spread (half the grid current: 0.33 against 0.62 mA at +1 V). DZ state that
the grid current depends only weakly on plate voltage (their Fig. 5); no number is given.
DZ quote Cak 0.9, Cgk 2.3 and Cag 2.4 pF, the same totals Koren lists; T1 page 1 gives 1.7 / 1.6 /
0.46 and 0.34 pF for the bare tube without shield, so our "data sheet plus 0.7 pF stray" matches
both for grid-plate and grid-cathode.

## 4. 6L6GC

### 4.1 What the published set gets wrong

With 400 V on the screen the real knee is at 60 to 80 V; Koren's `atan(Vp/12)` puts it near
25 V. At 0 V grid and 30 V plate the sheet gives 135 mA, the set gives about 300. Error of the
old model against the digitised plate family: 49 mA rms, 172 mA worst, 105 mA rms in the knee
region. It also over-estimates plateau current at the sheets' class A points by 4 to 12 %, and the
earlier screen split under-estimates the screen current rise below the knee by half.

The digitised curves show the textbook beam-tube behaviour: below the knee all grid curves fall
onto one line, independent of grid voltage, close to a 3/2 power of plate voltage (29, 76, 135,
189, 236 mA at 10, 20, 30, 40, 50 V). That suggests the form:

    E1   = (Vg2k / kp) * ln(1 + exp(kp * (1/mu + Vgk / Vg2k)))
    I0   = (pi / kg1) * E1^ex                       space current (unchanged from Koren)
    Vp'  = ln(1 + exp(4 Vpk)) / 4                   as before
    w    = kv * Vp'^1.5 / I0                        ratio of the 3/2-power line to the space current
    g    = w / (1 + w^3)^(1/3)                      smooth minimum of the two (cube root only)
    Ip   = I0 * g * (1 + lam * Vp')
    Ig2  = I0 * (rs + asc / (1 + w))

`g` goes to 1 above the knee and to `w` below it, so `Ip` tends to `kv * Vp^1.5` there whatever
the grid does. It is smooth for Vpk > 0 and all derivatives are elementary; `w` needs a guard
when I0 is zero (g is then 1 and Ip is 0). The screen term uses the same `w` with a softer shape:
the sheets show the screen current rising well before the plate current has visibly dropped.

Recommended, per tube: mu 10.828, ex 1.3921, kg1 1624.7, kp 30.29, kv 8.670e-4 A/V^1.5,
lam 6.326e-4 /V, rs 0.06434, asc 0.3661. For n tubes in parallel kv scales with n like the
current.

| quantity (rms / max, mA) | old | new |
|--------------------------|-----|-----|
| plate family, 400 V screen | 48.9 / 172 | 7.1 / 27 |
| same, knee only (grid 0 to -8 V, plate below 100 V) | 105 / 172 | 7.2 / 22 |
| transfer curves at 400 V plate, screen 250 to 400 V | 7.9 / 17 | 4.1 / 8.6 |
| same, below 60 mA | 4.1 / 6.9 | 3.7 / 8.6 |
| zero-bias family, screen 50 to 400 V (plateau and upper knee) | 10.9 / 47 | 9.2 / 36 |
| screen current, 400 V screen, grid 0 and -10 V | 13.8 / 46 | 4.4 / 9.3 |
| screen current, zero bias, screen 100 to 400 V | 2.9 / 11.8 | 1.8 / 4.9 |

### 4.2 Independent check: the data-sheet class AB1 tables (P2 page 2)

A static push-pull pair on a resistive load, computed by quadrature (`push_pull` in the script).
None of these numbers were used in the fit.

| condition (two tubes) | data sheet | old model | new model |
|-----------------------|------------|-----------|-----------|
| 450 V, 400 V screen, -37 V, 70 V peak grid to grid, 5.6 k | 55 W; plate 116 to 210 mA; screen 5.6 to 22 mA | 67.6 W; 113 to 220; 8.5 to 22.5 | 54.6 W; 108 to 204; 5.6 to 25.0 |
| 360 V, 270 V, -22.5 V, 45 V, 6.6 k | 26.5 W; 88 to 132; 5 to 15 | 32.7 W; 90 to 147; 6.9 to 14.4 | 26.1 W; 78 to 134; 4.2 to 13.6 |
| 360 V, 270 V, -22.5 V, 45 V, 3.8 k | 18 W; 88 to 140; 5 to 11 | 21.7 W; 90 to 155; 6.9 to 12.6 | 17.1 W; 78 to 141; 4.2 to 10.0 |

The old model is 20 to 25 % high in power; the new one is within 5 %. Its idle current at the
360 V point is 11 % low, which is the size of its error near cut-off.

### 4.3 Our operating point

At 475 V plate and 470 V screen, 16 mA per tube needs -63.4 V with the new set (-59.4 V with the
old). The sheets stop at 400 V screen and the fit is 3.7 mA rms below 60 mA, so this is good to a
few volts only: the bias voltage stays a calibration value set for the measured idle current. The
forum figure of "about -55 V" would give roughly 30 mA per tube.

Four tubes driven to the onset of grid current from a stiff 475 V supply deliver 133 W into
2.45 k, 129 W into 3.0 k, 121 W into 3.6 k and 113 W into 4.2 k with the new set. The amplifier is
rated 120 W; supply sag takes some of this away, so the rating favours 2.2 to 3.0 k. The old set
gave 160 W and more, which no four 6L6GC on this supply produce.

### 4.4 Control-grid current

P1 plate #5819 prints grid current for +4 to +20 V at 400 V screen: 5.5, 10.7, 16.2, 21.1 and
26.1 mA, flat above about 150 V plate. It is almost exactly linear, 1.3 mA per volt. The earlier
estimate (1.2e-3, exponent 1.4) gives 8.4 to 79.5 mA at the same points. Fit of our form with the
onset sharpness left at 8 per volt: gg 1.4224e-3, xi 0.9732, vg0 0.

Not published: anything below +4 V, so the onset sharpness (cg) remains an estimate. Not
modelled: below roughly 60 to 100 V plate the printed grid current rises steeply (the +4 V curve
goes from 5.5 mA to about 17 mA at zero plate voltage), because the electrons the plate refuses
return towards the grid.

### 4.5 What the form cannot reproduce

- Screen current at grid voltages below about -20 V has humps between 50 and 200 V plate
  (secondary emission). The model is monotonic there; those curves were excluded from the fit.
- The knee is fitted to 7 mA rms, but individual curves are off by up to 22 mA around 40 to 60 V.
- The fit is weighted to the 400 V screen data, the nearest to our 470 V. At the sheets' 250 V
  screen class A points it reads low: 66.8 mA against 72 (250 V, -14 V) and 47.6 against 54
  (350 V, -18 V).
- The slope above the knee is one number (`lam`); the sheet's slope varies a little with current.
- Capacitances: T/P sheets give 10 pF input, 6.5 pF output, 0.6 pF grid to plate (unchanged).

## 5. Output transformer

Nothing was found for the maker's part 70500207 by number. The only data on the actual transformer is
one tear-down (G):

| quantity | reported in G | consequence |
|----------|---------------|-------------|
| primary turns | 1348 | |
| secondary turns | 8 ohm tap about 77, 16 ohm 109 (32 more), 4 ohm 54 to 55 | 16 x (1348/109)^2 = 2447 ohm; 8 x (1348/77)^2 = 2452 ohm plate to plate |
| "plate load" stated by the same person | 2200 (rewind: 3300) | |
| primary inductance | about 50 H at 60 Hz, 100 V rms | |
| core | 1 3/8 inch tongue (EI-138), 2 inch stack, 0.025 inch laminations | |
| winding | paper layers, no bobbin; secondary-primary alternating in nine sections (five secondary, four primary); bifilar secondary | low leakage for a guitar transformer |
| leakage inductance, capacitance, DCR of the original | not reported | |

Comparable 100 W parts with full published data:

| part | primary | primary DCR | secondary DCR (16 ohm) | primary inductance | leakage (primary, secondary shorted) |
|------|---------|-------------|------------------------|--------------------|--------------------------------------|
| Hammond 1750U (H1; replacement for a British 100 W head) | 1.7 k | 15.4 + 16.6 ohm | 0.32 ohm | 8.85 H at 1 kHz, 1 V | 7.97 mH at 1 kHz |
| Hammond 1650T (H2; 120 W, hi-fi, 6L6GC listed) | 1.9 k | 47.1 ohm end to end | 0.23 to 0.36 ohm | 124 H at 60 Hz, 10 V | 4.30 mH |
| Hammond 1650R (H2; 100 W) | 5 k | 54.6 + 46.6 ohm | 0.30 to 0.42 ohm | 320 H at 60 Hz, 10 V | 10.84 mH |

Recommended values:

| parameter | was | recommended | range | basis |
|-----------|-----|-------------|-------|-------|
| raa | 3.0 k | **2.45 k** | 2.2 to 3.0 k | turns count in G; rated power (4.3). The forum figure of 4.2 k has no stated method and contradicts both |
| lm | 25 H | 50 H | 9 to 60 H | G at 100 V, 60 Hz; H1 shows how far small-signal, 1 kHz values fall |
| lleak | 6 mH | 6 mH (unchanged) | 3 to 10 mH | H1 and H2 scaled in proportion to impedance give 5.3 to 11.5 mH at 2.45 k; nine-section interleaving argues for the low side. Not measured on the part: estimate |
| cpri | 300 pF per half | 300 pF (unchanged) | 100 to 600 pF | **not found** in any source; estimate |
| rpri_half | 33 ohm | 33 ohm | 29.5 to 37.5 | forum measurement already cited in CIRCUIT.md |
| rsec | 0.6 ohm | 0.35 ohm | 0.23 to 0.42 | H1, H2 |

The change in raa matters: with 2.45 k the tubes work into a 18 % lower load, which lowers the
voltage swing before the knee and raises peak current.

## 6. Speaker load

Published numeric impedance data for a 16 ohm 4x12 with the reference driver was **not found**. Its maker
publishes only Re (12.9 ohm for 16 ohm) and Fs (75 Hz, free air). Forum posts give a resonance of
118 Hz for a closed British 4x12 with other drivers of the same maker and 107 Hz for one commercial reactive
load; their curves are images or attachments and no peak value is stated in text.

The best published measurement found is Z, Fig. 11.9 upper left: impedance magnitude of several
12-inch 8 ohm guitar speakers of the reference driver's maker, un-mounted, anechoic, low level. Four 16 ohm drivers in
series-parallel have the impedance of one 16 ohm driver, so the family median, scaled by
12.9 / 7.3, is used as the target for the voice-coil rise (the box changes the resonance, not the
rise). Free-air resonances in the figure: 72 to 113 Hz, peaks 75 to 150 ohm for 8 ohm units.

The rise is far from a pure inductance: the median goes as about f^0.5 from 2 to 18 kHz.

| load (Re 12.9 ohm, resonance branch as before) | 0.3 to 18 kHz: rms / max dB | 2 to 10 kHz: rms / max dB | ohm at 1 / 3 / 5 / 8 kHz |
|-----------------------------------------------|------------------------------|---------------------------|---------------------------|
| target | | | 17.5 / 27.9 / 33.0 / 42.9 |
| old: 1.6 mH parallel 70 ohm | 1.88 / 3.54 | 2.65 / 3.54 | 16.2 / 34.3 / 49.2 / 62.9 |
| one section refitted: 1.28 mH parallel 45.6 ohm | 1.09 / 1.93 | 0.99 / 1.59 | 15.3 / 29.2 / 39.7 / 48.2 |
| **two sections in series: 1.245 mH parallel 11.11 ohm, then 0.5784 mH parallel 80.59 ohm** | 0.46 / 1.29 | 0.17 / 0.43 | 18.0 / 27.4 / 33.5 / 42.8 |

One section is more than 1 dB off between 2 and 10 kHz, so the two-section ladder is recommended
(one more state in the power cluster). The old load was 3.5 dB too high in impedance at 5 to
8 kHz. Because the output stage is close to a current source when the feedback is light, that is
up to 3.5 dB too much speaker voltage in that band with the old load.

Resonance branch: frequency 110 to 118 Hz is supported by the forum datum; the peak value in a
cabinet (we use 85 ohm) was **not found** and stays an estimate. Free-air peaks scaled to 16 ohm
would be 130 to 265 ohm; four drivers with different resonances in a lossy box give a lower,
broader peak.

Commercial reactive load boxes: no published numeric curve was found, only the two resonance
frequencies above. How a load box differs from a cabinet in the 3 to 8 kHz band can therefore not
be stated from data; a typical design uses a single inductor with a parallel resistor, i.e. the
"one section" shape above, which is 1 to 2 dB higher than a real voice coil at 5 to 8 kHz when
matched at 1 to 2 kHz.

## 7. Not found, still estimates

- 12AX7 grid current from a data sheet; cut-off behaviour below 0.05 mA.
- 6L6GC grid current below +4 V; any Sylvania 6L6GC curve family.
- 6L6GC curves above 400 V screen (we run at about 470 V): the model extrapolates.
- Leakage inductance, winding capacitance and secondary resistance of the actual transformer.
- Peak impedance of a 4x12 at resonance; any numeric curve of a reactive load box.
- Rectifier diode parameters (the 1N4007 set in `models.py`) were not part of this work.
