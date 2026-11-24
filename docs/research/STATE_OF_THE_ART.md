# State of the art: circuit-level tube amplifier simulation

What others have published or stated, what we take from it, and what we do differently. Papers are
cited by venue and year; statements by vendors are quoted from their own pages or interviews and
are labelled as claims. Where we could not read a primary source we say so.

## 1. Device models

**Triodes.** Koren's 1996 phenomenological law is still the common baseline: five parameters, smooth,
accurate in the normal operating region, published parameters for the usual types. Its weak point
is the grid: Koren only suggests a diode with a series resistor and calls his resistor values rough.
Dempwolf and Zölzer (DAFx-11) derive a comparable plate law from the Langmuir-Child relation and add
a smooth power-law grid current, fitted to measured 12AX7s; their grid law is the best-documented
one for guitar-amp overdrive. Cardarilli et al. (2009, as reproduced by D'Angelo, Pakarinen and
Välimäki, 2013) use polynomial-in-Vgk coefficients with a piecewise grid current; Dunkel et al.
(DAFx-16) report that the piecewise form is awkward for Newton solvers. Cohen and Hélie (AES 129,
2010, and Acoustics 2012) measure triodes and make the grid current depend on plate voltage as
well; we could read only the abstracts.

*Our choice:* Koren plate current with the Dempwolf-Zölzer grid law, both smooth, both from papers.
Parameters are exposed as data so that a "tube set" is a small table, not code.

**Beam tetrodes.** Koren's pentode law gives a good plate characteristic for the 6L6GC but a screen
current that ignores the plate voltage. Cohen and Hélie (DAFx-10) use it unchanged for a
single-ended stage with a piecewise-quadratic grid current. Reefman's extended ("Derk") models add
plate-dependent screen current and secondary emission, but no 6L6GC parameter set is published with
them, and the fitted decks that circulate carry no licence. Nakabayashi's models are "all rights
reserved".

*Our choice:* Koren's space-current law with our own current-conserving plate/screen split
(CIRCUIT.md 9.4), anchored to data-sheet points. A future work package can fit Reefman's form to
data-sheet curves if screen behaviour turns out to limit realism.

## 2. Solving the circuit

| approach | representative work | strengths | problems for a high-gain head |
|----------|--------------------|-----------|------------------------------|
| Static waveshaper per stage with filters between | most early commercial sims; guitarix's precomputed tube tables | very cheap | no grid-current loading, no bias shift, no interaction; this is what "sounds like a plugin" |
| Nodal K-method / DK-method | Yeh, Abel and Smith (2010); Holters and Zölzer (2011, generalised; ACME.jl) | systematic from a netlist, implicit and stable, exact for the discretised circuit | Newton dimension grows with the number of tubes in one system; dense matrices grow with the square of the state count |
| Wave digital filters with R-type adaptors | Werner et al. (2015); Dunkel et al. (DAFx-16, four triodes at one 25-port adaptor) | modular, good numerical behaviour | multiple nonlinearities still need a multidimensional Newton solve at the root; the DAFx-16 preamp ran slightly slower than real time in MATLAB at 4x |
| Block-wise decomposition | Macak and Schimmel (DAFx-10): overlapping triode pairs, one-sample delay in the global feedback | cheap, keeps interstage loading approximately | every tube is evaluated twice; a delay inside the power-amp feedback loop changes the loop's phase margin, which is exactly what presence and resonance play with |
| State-space with tabulated nonlinearities | Macak, Holters and Schimmel (DAFx-12) | removes the iterative solve | table dimension explodes beyond two or three ports; awkward with continuously variable controls |
| Neural approximations of the nonlinear part | one maker's public description of its engine | fast evaluation | training data and tooling; opaque to verify; not a good fit for an open project whose claim is a checkable circuit model |

Numbers worth remembering: Dunkel et al. needed on average three Newton iterations per sample for
four coupled triodes; Macak and Schimmel ran four triodes plus a push-pull stage at 2x
oversampling in 58 % of a 1.6 GHz CPU in 2010; Yeh's thesis argues 8x oversampling is sufficient
for distortion circuits and uses 32x as a reference.

We found **no published model that solves the phase inverter, the push-pull output tubes, the
output transformer, the reactive load and the global feedback as one delay-free system** in real
time. Papers either stop at the preamp, treat a single-ended stage, or delay the feedback.

*Our choice (evidence summarised in docs/MODEL.md section 3 and docs/VERIFICATION.md G1):* the nodal K-method, applied to a handful of small
clusters instead of one system. Cuts are placed only where a stage drives a much higher resistance;
the downstream cluster receives the upstream voltage of the same sample and the upstream cluster
receives the downstream load current one oversampled sample later, linearly extrapolated. The whole
power amplifier including the feedback loop is one cluster and is solved without any delay.

## 3. Aliasing

Options are plain oversampling, antiderivative antialiasing (Parker et al. 2016; Bilbao et al. 2017)
and its extension to stateful systems (Holters, DAFx-19). The stateful variant has been shown on a
diode clipper and one op-amp pedal, still needs about 2x oversampling, and has not been
demonstrated for cascades of implicit multi-port stages. Commercial amp sims that state anything
offer 2x to 8x (one commercial maker lists none/2x/4x/8x).

*Our choice:* plain oversampling of the whole chain by 4x (default) or 8x, with linear-phase
half-band FIR stages, selectable minimum-phase for zero latency. Aliasing is measured, not
assumed (gate G4 in docs/VERIFICATION.md).

## 4. Output transformer, load and supply

Macak and Schimmel (DAFx-11) compared a linear transformer with Fröhlich and Jiles-Atherton core
models in a push-pull stage and concluded that the nonlinear core had a minor effect, only at very
low frequencies, for a large cost. Cohen and Hélie used linear coupled inductors with published
values for a hi-fi single-ended transformer. Speaker loads are modelled as a resistor-inductor
pair plus one parallel resonance; nobody publishes fitted values for a guitar 4x12, and Aiken's
well-known article gives the topology and scaling rules but not his values. Supply sag appears in
the literature as a series resistance and a reservoir capacitor.

*Our choice:* linear transformer (ideal ratio, magnetising and leakage inductance, copper
resistance, winding capacitance), an RLC speaker model inside the same implicit system, and the
complete supply ladder (reservoir, 400 ohm screen feed, preamp droppers) as part of the
power-amplifier cluster so that plate, screen and preamp rails sag with the right time constants.

## 5. What other products do

Open source:

| project | licence | method | usable by us |
|---------|---------|--------|--------------|
| guitarix | GPL-2+ | tube stages as tables precomputed from a Koren-based script; filters between | no code, no tables |
| LiveSPICE | MIT | symbolic MNA of an arbitrary schematic, JIT-compiled; a community circuit of the reference head's preamp exists for it | ideas only; a useful cross-check of topology |
| ACME.jl | MIT | Holters' generalised state-space method in Julia | ideas |
| chowdsp_wdf | BSD-3 | wave digital filter library | not needed |
| NeuralAmpModelerCore, AIDA-X | MIT, GPL-3 | black-box neural captures | the capture format is our measurement target, not our method |
| BYOD, SwankyAmp, Schrammel OJD | GPL-3 | mixed | no code |

Commercial (claims):

- One maker of amp plug-ins describes "full circuit simulation" by solving the nonlinear differential
  equations, with neural networks approximating the nonlinear part in one engine, and the power amplifier
  with a reactive load solved in the same oversampling cycle as the preamp.
- The developer of a free model of this very amplifier family describes simulating the actual
  circuit and names the cold-biased stage and the local-feedback stage as sources of numerical
  trouble. Both are in our netlist (V2A and V5A) and both converge in our tests.
- Another maker speaks publicly about robot-driven data collection and does not claim a white-box
  circuit model; a third lists a third-party amp technology. Others publish nothing verifiable.
  (Products are not named here.)

## 6. This amplifier in the literature

There is no DAFx or AES paper on it and no published SPICE analysis with per-stage gains or
responses. Forum descriptions of the stage order contradict each other. Our netlist comes from
the factory drawing alone (CIRCUIT.md), and our SPICE deck is, as far as we can tell, the first
checkable reference for it.

## 7. References

- N. Koren, "Improved vacuum tube models for SPICE simulations", Glass Audio 8(5), 1996.
- K. Dempwolf, U. Zölzer, "A physically-motivated triode model for circuit simulations", DAFx-11.
- D. T. Yeh, J. S. Abel, J. O. Smith, "Automated physical modeling of nonlinear audio circuits for real-time audio effects", IEEE TASLP 18(4), 2010; D. T. Yeh, PhD thesis, Stanford, 2009.
- M. Holters, U. Zölzer, "A generalized method for the derivation of non-linear state-space models from circuit schematics", EUSIPCO 2015 (and "Physical modelling of a wah-wah effect pedal...", DAFx-11).
- J. Macak, J. Schimmel, "Real-time guitar tube amplifier simulation using an approximation of differential equations", DAFx-10; "Real-time guitar preamp simulation using modified blockwise method and approximations", EURASIP JASP 2011; "Simulation of a vacuum-tube push-pull guitar power amplifier", DAFx-11.
- J. Macak, M. Holters, J. Schimmel, "Simulation of Fender type guitar preamp using approximation and state-space model", DAFx-12.
- W. R. Dunkel et al., "The Fender Bassman 5F6-A family of preamplifier circuits: a wave digital filter case study", DAFx-16.
- K. J. Werner et al., "Wave digital filter adaptors for arbitrary topologies and multiport linear elements", DAFx-15.
- I. Cohen, T. Hélie, "Real-time simulation of a guitar power amplifier", DAFx-10.
- M. Holters, "Antiderivative antialiasing for stateful systems", DAFx-19; J. Parker, V. Zavalishin, E. Le Bivic, "Reducing the aliasing of nonlinear waveshaping using continuous-time convolution", DAFx-16.
- J. Pakarinen, D. T. Yeh, "A review of digital techniques for modeling vacuum-tube guitar amplifiers", Computer Music Journal 33(2), 2009.
- S. D'Angelo, J. Pakarinen, V. Välimäki, "New family of wave-digital triode models", IEEE TASLP 21(2), 2013.
- D. Reefman, "SPICE models for vacuum tubes using the uTracer" (theory note for the extended Koren models).
