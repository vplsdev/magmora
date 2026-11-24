# Pedal specification for the engine (XP2)

What the C++ port has to implement. The executable form is `proto/blockamp/pedal.py` (`build2`, `split`,
`system2`, `run2`, `newton2`, `dev_eval`, `taper`); where this text and the prototype disagree, the prototype wins
and this text has a bug. Where the numbers come from, and how well they hold, is in `docs/research/PEDAL.md`.
The port follows PEDAL.md: the pedal solved inside the head's oversampled loop, one value set (the reference unit's),
and the transistor and op-amp detail of PEDAL.md 5.1.

## 1. What it is

The overdrive pedal as a circuit, solved inside the head's oversampled loop ahead of cluster A, at the head's
internal rate, adding no latency. Input: volts at the pedal's input jack. Output: volts at its output jack, loaded
by the head's 1 M grid leak; that voltage is the head's input. One version (the 1979 output network), no switch.

Parameters (`ChainParams.h`): `kPedalOn` (22), `kPedalDrive` (23), `kPedalTone` (24), `kPedalLevel` (25), each
0 to 10 as panel positions. Bypass is a crossfade (12 ms, as the head's relays), not a circuit.

## 2. Component values

The reference unit's values (PEDAL.md 7.6); nominal values in brackets where they differ. `PHYS2` in the prototype.

| element | between | value |
|---|---|---|
| supply `vcc`, bias rail `vb` | ideal sources | 9 V, 4.5 V |
| `Cin` | in, a | 22 nF |
| `Rin` | a, b1 | 1 k |
| `Rb1` | b1, vb | 510 k |
| Q1 | base b1, emitter e1, collector vcc | section 3 |
| `Re1` | e1, ground | 10 k |
| `Cc1` | e1, p1 | 1 uF |
| `Rp1` | p1, vb | 10 k |
| op-amp A | + p1, - n1, out o1 | section 3 |
| `Cg`, `Rg` | n1, m1; m1, ground | 43.42 nF (47 nF); 4.655 k (4.7 k) |
| `Rf` | n1, f1 | 50.32 k (51 k) |
| Drive pot | f1, o1 | 481 k total (500 k), section 4 |
| `Cf` | n1, o1 | 49.96 pF (51 pF) **plus 1.74 pF** (two diode capacitances): 51.70 pF |
| clipping diodes | n1, o1 | section 3 |
| `Rlp`, `Clp` | o1, t; t, ground | 1.007 k (1 k); 0.2536 uF (0.22 uF) |
| `Rp2` | t, vb | 10 k |
| op-amp B | + t, - n2, out o2 | section 3 |
| Tone pot | t, wiper w, n2 | 19.76 k total (20 k), section 4 |
| `Ct`, `Rt` | w, w2; w2, ground | 0.2063 uF (0.22 uF); 220 ohm |
| `Rfb2` | o2, n2 | 1 k |
| `Cc2`, `Rlv` | o2, x; x, lvt | 1 uF; 1 k |
| Level pot | lvt, wiper lw, vb | 100 k total, section 4 |
| `Rsw` | lw, vb | 510 k |
| `Cc3` | lw, b2 | 1 uF |
| `Rb2` | b2, vb | 510 k |
| Q2 | base b2, emitter e2, collector vcc | section 3 |
| `Re2` | e2, ground | 10 k |
| `Ros`, `Cout`, `Rosh` | e2, oc; oc, out; out, ground | 100 ohm; 10 uF; 10 k |
| load | out, ground | 1 M (the head's input; if the head's input network is ever placed in this cluster instead, remove it) |
| bias currents | from p1, n1, t and n2 to ground | 25 nA each (current leaves the node) |

Pot sections never go below 1 ohm.

## 3. Devices: seven elements, nine scalar ports

Each device is a scalar function `i(u)` of one control voltage `u`, a linear combination of node voltages, and
injects multiples of `i` into nodes. `dev_eval` gives `i` and `di/du`. VT = 25.693 mV.

| device | control u | i(u) | injection | parameters |
|---|---|---|---|---|
| clipping diodes (`dpair`) | v(n1) - v(o1) | Is1 (exp(u/nVT) - 1) - Is2 (exp(-u/nVT) - 1) | +i into o1, -i into n1 | Is1 = 3.9798 nA, Is2 = 4.6711 nA (4.3116 nA split by an 8 mV mismatch), nVT = 49.952 mV (n = 1.9442) |
| op-amp input pair (`tanh`), A and B | v(+) - v(-) | Imax tanh((u + Vos)/Vs) | +i into the op-amp's internal node X | Imax = 30 uA, Vs = Imax/gm1 = 56.14 mV, Vos = 0 |
| op-amp output saturation (`clamp`), A and B | v(X) | Is (exp((u - Vhi)/VT) - exp((Vlo - u)/VT)) | -i into X | Is = 1 pA, Vhi = 6.45 V, Vlo = 2.55 V (2.5 V either side of the bias rail less a 0.55 V clamp drop) |
| transistor, collector part, Q1 and Q2 | (1 - k) v(b) - v(e) + k v(vcc) | Is exp(u/nVT) | +i into e (drawn from vcc) | Is = 0.15479 pA, nVT = 29.079 mV (n = 1.1318), k = nVT/VAF = 4.4737e-4 (VAF 65 V) |
| transistor, base part, Q1 and Q2 | v(b) - v(e) | (Is/beta) exp(u/nVT) | +i into e, -i into b | beta = 200 |

The rest of each op-amp is linear: gm1 = 2 pi GBW Cc = 534.4 uS (GBW 2.835 MHz, Cc 30 pF) is the tanh's slope at
zero; `R1` = A0/gm1 = 187.1 M and `Cc` = 30 pF from X to ground (A0 = 1e5); the output is a controlled current
source v(X)/Ro into the output node with `Ro` = 100 ohm from the output node to ground. The slew rate is Imax/Cc
= 1 V/us.

Overflow guard: every exponential is continued as a straight line above an argument of 40.

## 4. Knob laws (`pedal.taper`)

x is the panel position divided by 10. `alog(x, m) = (b^x - 1)/(b - 1)` with `b = (1/m - 1)^2` is the audio law
through (0.5, m).

| knob | resistance |
|---|---|
| Drive | f1 to o1: 481 k x alog(x, 0.15) |
| Tone | t to w: 19.76 k x W(x); w to n2: 19.76 k x (1 - W(x)); W(x) = 0.5 alog(2x, 0.10) for x <= 0.5, else 1 - 0.5 alog(2(1 - x), 0.10). Tone 10 puts the wiper at the inverting input (treble up) |
| Level | lvt to lw: 100 k x (1 - alog(x, 0.10)); lw to vb: 100 k x alog(x, 0.10) |

Controls are smoothed as rotations (one pole, 30 ms) and the affected cluster's matrices rebuilt every 32 host
samples while moving, as PLAN 3.4 does for the head. Drive lives in O2; Tone in O2 and O3; Level in O3 and O4.

## 5. Four clusters

Solved in this order within one oversampled sample. Nothing is delayed.

| cluster | input | contents | ports | states | output to the next |
|---|---|---|---|---|---|
| O1 | v(in) | `Cin`, `Rin`, `Rb1`, Q1, `Re1`, `Cc1`, `Rp1`, bias current at p1 | 2 | 2 | v(p1) |
| O2 | v(p1) as an ideal source | op-amp A, `Cg`, `Rg`, `Rf`, Drive, `Cf`, clipping diodes, bias current at n1; **plus a linear copy of the tone stage as its load**: `Rlp`, `Clp`, `Rp2`, Tone pot, `Ct`, `Rt`, `Rfb2`, bias currents at t and n2, and op-amp B with its tanh replaced by gm1 and no clamp | 3 | 6 | v(o1) |
| O3 | v(o1) as an ideal source | the tone stage with the real op-amp B; **plus a linear copy of the level network as its load**: `Cc2`, `Rlv`, Level pot, `Rsw`, `Cc3`, and `Rb2` in parallel with the follower's small-signal input resistance (beta + 1)(nVT/Ie + 5 k), Ie = 0.31 mA | 2 | 5 | v(o2) |
| O4 | v(o2) as an ideal source | `Cc2`, `Rlv`, Level pot, `Rsw`, `Cc3`, `Rb2`, Q2, `Re2`, `Ros`, `Cout`, `Rosh`, load | 2 | 3 | v(out) |

- The O1/O2 cut is exact: the op-amp's input draws only the bias current, which O1 carries.
- The copies make the other two cuts exact while the copied op-amp or follower is in its linear range, and close
  otherwise. Measured against the one-cluster model: -92 dB or better on every signal tried (PEDAL.md 8).
- **Start-up:** each cluster's input source must be set to the previous cluster's DC output before its operating
  point is computed. Without that, a 0.7 mV step at the start took a second to settle.
- `reset()` returns every cluster to its operating point, as for the head.

## 6. Per sample, per cluster

Exactly the head's scheme (PLAN 3.3) with scalar devices; `run2` and `newton2` are normative.

```
p  = Pv u + Ps s
solve v = p + K i(v)        Newton, start 2 v[n-1] - v[n-2]
y  = Yv u + Ys s + Yi i     (old s)
s <- Ss s + Sv u + Si i
```

Matrices from the nodal K-method with trapezoidal companions (`system2`): `Nv` holds each device's control row,
`Bi` its injection column, `K = Nv A Bi`. Only one source changes per sample; fold the others into offsets.

Newton rules:

1. Full step from the linearisation; then scale the whole step so that no device moves more than: 0.15 V
   (clipping diodes), 0.10 V (transistor parts), 0.5 V (clamp), 3 Vs = 0.168 V (tanh).
2. From iteration 10 the step is also halved, from iteration 25 quartered.
3. Converged when the largest scaled step is below the tolerance. The prototype uses 1e-7 V; the head's 1e-4 V with
   the last step applied to the currents to first order gave -126 dB against the prototype (PEDAL.md 8).
4. On reaching the cap (60): return to the last accepted solution and walk `p` from its last accepted value in 8
   equal steps; count a failure only if that fails too. Full-band noise at 3 V needs about 66 iterations per sample
   in O2: bound the work per block as the head does.

## 7. Prewarping

In the discrete model only, the capacitors `Clp` and `Ct` (wherever they appear: O2's copy and O3) are multiplied
by `(pi f0/fs) / tan(pi f0/fs)` with f0 = 20 kHz and fs the internal rate (0.9842 at 288 kHz). No other capacitor,
and in particular not the op-amps' `Cc`: that failed the harmonic gate. Without this the small-signal gate fails
at 20 kHz (PEDAL.md 5.2).

## 8. The cheap mode (how to reach the cost target without dropping physics)

While an op-amp's input difference is small and its internal node is clear of the rails, its tanh is its tangent
and its clamp carries nothing. `split(linear_opamps=True)` builds O2 with one port (the clipping diodes) and O3
with none, with the **same states**, and gives each two guard outputs: the op-amp's input difference and its
internal node voltage.

Per sample, for O2 and O3: run the cheap cluster; if `|vd| > 2 mV` or `|v(X) - 4.5 V| > 1.5 V`, discard that
result and redo the sample with the full cluster on the same states, seeding its ports from the guard values and
the cheap cluster's diode voltage. The states are committed once. A private C port of exactly this measured 4.6 to
5.3 % of a core at 288 kHz with the head's tolerance, on a busy machine; the ceiling is 5 %, the aim 3 %.
Still open to the port, all exact: one exponential per transistor instead of two (the two controls differ by
`k (v(vcc) - v(b))`, a small argument), tabulated device laws, a closed-form solution for a cluster with a single
junction.

## 9. What the port is checked against

- **G-P2:** every probe (`out`, `o1`, `o2`, `e1`, `e2`, `p1`) within 1e-6 V of `pedal.SplitSim` at the same rate with
  closed-form devices and the prototype's tolerance; with the cheap mode and the product tolerance, within 1e-4 V
  at `out` on the committed signals. Golden matrices and device samples: export them from `pedal.split()` and
  `system2()` for a few settings and two rates, as `eval/export_golden.py` does for the head.
- **G-P1** (`eval/check_pedal_spice.py`, which should gain a `--cli` backend like `check_spice.py`): on the
  prototype, operating point 0.0005 mV; small-signal 0.156 / 0.087 dB (gates 0.25 / 0.1); harmonics 0.440 / 0.208 dB
  (gates 1.0 / 0.5); split identical at Standard.
- **G-P1r** and the validity table (scored privately against the pedal reference): PEDAL.md 7.6.
- **G-P3** (chain aliasing with the pedal engaged), **G-X1** (pedal off: the head bit for bit), **G-X3** (CPU:
  5 % ceiling for the pedal at Standard, 48 kHz).

## 10. Not to do

- No linear followers, no one-pole op-amps without the tanh, no dropping of the Early term.
- No table or filter fitted to the pedal reference: the reference informed component values and the knob law, and
  nothing derived from it may enter the repository.
- No delay between the four clusters, and none between O4 and the head's cluster A beyond the head's own rule for
  a low source impedance into a large resistor.
