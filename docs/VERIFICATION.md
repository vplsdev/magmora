# Verification

Every acceptance gate of the project, the measured value, the limit, and the command that reproduces it. Machine: Apple M4 Pro, macOS 26.5; load average at measurement is stated
where timing matters. A failing gate is listed as failing; no gate was moved to pass.

Code state: plug-in 0.1.0, engine 0.1.0, value set "prototype" (Revised, model 2df6d3425ed981c7, netlist
f52a2e6a59f4a414) unless a row says "as-drawn" (model c7bc8b0442f69913). Dates: 22 and 23 Nov 2026.

Name: the product had a different working name until 24 Nov 2026. Results dated before the rename were measured on
the same code under the old name and AU codes; the codes are now `MgAp` (subtype) / `Mgmr` (manufacturer). The rename changed no DSP; the release check at the end of this file reran
the gates under the new name.

## Summary

| gate | what | result |
|---|---|---|
| G1 | engine against ngspice | **fails** in 2 of 73 cases at Standard and 1 of 55 at High (details below) |
| G1b | C++ against the Python prototype | passes (2.1e-7 V against 1e-6 V) |
| G2 | against the real amplifier | **fails** on spectral loss with a front high-pass and on band balance (private captures; details below) |
| G3 | CPU | passes at Standard; High **fails** on full-scale noise at 96 kHz |
| G4 | aliasing against an established simulator's measured level | passes at Standard, High and Ultra |
| G5 | automation without zipper noise | passes |
| G6 | latency and formats | passes |
| G7 | licence audit | passes, zero findings |
| G8 | plug-in validation | passes on VST3; AU validation **not run** |
| G9 | UI and art | passes what was measured; GPU memory and registration **not measured** |
| G-X4 | the whole chain in the plug-in (XP7) | passes, except isolated soak blocks over real time (details below) |
| G-X4b | a response file as a cabinet slot's source | passes, except isolated soak blocks over real time |
| G-X4c | the built-in modelled cabinet as the default | passes (whole chain under G-X3's 40 %, no soak block over) |
| G-X4d | every section switchable, the head's own on/off | passes (see below for the soak) |
| G-T | the pitch transposer in the chain and the plug-in | passes (details below, revised 24 Nov 2026) |

## G1: engine against ngspice

`$BLOCKAMP_PY eval/check_spice.py --cli <build>/dsp/tools/magmora-probe --osf 4` (and `--osf 8`). The engine is
run at a 192 kHz internal rate (osf 4), lower than shipped Standard at a 48 kHz host (288 kHz), so these
figures are pessimistic for that case. Reference: ngspice 45.2 on the same netlist.

Gates are Standard / High. Standard columns: 192 kHz internal (`g1c-<set>-4.log`, stage cases `g1d-<set>-4-stage.log`);
High columns: 384 kHz internal, final engine (`g1e-<set>-8-*.log`).

| quantity | gate | Revised, Standard | As drawn, Standard | Revised, High | As drawn, High |
|---|---|---|---|---|---|
| DC operating point, every node | 50 mV | 0.0 mV | 0.0 mV | 0.0 mV | 0.0 mV |
| small-signal, to 10 kHz | 0.5 / 0.2 dB | **0.513 dB fails** (lead, lead post 0) | **0.505 dB fails** (same case) | 0.146 dB | 0.144 dB |
| small-signal, 10 to 20 kHz | 1.5 / 0.5 dB | 1.175 dB | 1.198 dB | 0.351 dB | 0.345 dB |
| worst harmonic within 60 dB, below 20 kHz | 1.5 / 1.0 dB | **2.57 dB fails** (lead hot, 440 Hz, 0.5 V, speaker) | 0.84 dB | **1.01 dB fails** (same case) | 0.40 dB |
| waveform error after alignment, worst | -12 / -20 dB | -19.4 dB | -18.9 dB | -26.7 dB | -25.5 dB |
| burst recovery | 1.0 dB | 0.00 dB | 0.00 dB | 0.00 dB | 0.00 dB |
| supply rails during a chord | 1.5 V | 0.02 V | 0.01 V | 0.00 V | 0.00 V |
| each cluster alone (stage cases) | -30 dB | 18 of 18 pass, worst -59.6 dB | 18 of 18 pass, worst -59.0 dB | - | - |
| cases failing | | 2 of 73 | 1 of 73 | 1 of 55 | 0 of 55 |

- Lead hot: the monolithic solver at the same rate is 2.46 dB off on the same harmonic, about 45 dB down; the
  error is the trapezoidal rule on a hard-clipped output stage at this rate, not the partition. The gate stays at
  1.5 dB and is reported as failing at Standard. At High the same case is 1.01 dB against a
  1.0 dB gate, with the Revised values only; the Python prototype gives the same 1.01 dB, so this is the model,
  not the C++ port.
- The stage cases in `g1c-*-4.log` (13 of 18 failing) were written before a fix to the stage harness and are
  superseded by `g1d-<set>-4-stage.log`.
- Log files are in `$BLOCKAMP_PRIVATE/build/dsp/`.

## G1b: C++ against the prototype

The C++ probe (`<build>/dsp/tools/magmora-probe`), closed-form devices, against the correctly rounded prototype:
2.1e-7 V worst on the re-run cases (gate 1e-6 V). In ctest, `test_solver` checks the discretisation against the
prototype's matrices (`reference/golden`, written by `eval/export_golden.py`) and `test_graph` the clusters against
`reference/tran`.
`ctest` of the DSP library: 11 of 11 (reported by the engine package; not re-run in this round).

## G2: against the real amplifier

Two neural captures of real block-letter heads (a main capture and a second one), rendered from the same DI through
the same cabinet IR. The model's input gain, knobs and switches are fitted to each capture (and, as a diagnostic, one
first-order high-pass in front of the amp); the score is a multi-resolution spectral loss and the level error in
eight bands from 40 Hz to 12 kHz. The captures and the fitting scripts are private, so this gate cannot be rerun from
the repository. The spectral-loss gate is relative to established commercial simulators fitted under the same
freedoms; those comparisons are not published.

- Spectral loss: passes on both captures with knobs and input gain only (the main capture by 0.001); **fails** on
  both with a front high-pass.
- Band balance, every band within 1.5 dB (main) and 2 dB (second): **fails**. Main, knobs only: 200-400 Hz
  -1.9 dB and 6.4-12 kHz -1.6 dB; Revised on main: 3.2-6.4 kHz -1.3 dB, 40-100 Hz -1.2 dB. Second capture, Revised:
  100-200 Hz +2.1 dB, 40-100 Hz +2.1 dB. The 3.2 to 6.4 kHz band (1.56 dB in earlier tables) and the
  100 to 400 Hz bands on the second capture are carried as failing gates.
- The Revised values were chosen with both captures in view (docs/MODEL.md section 2), so their scores are not an
  independent test.

## G3: CPU

`<build>/dsp/tools/magmora-bench`, 48 kHz host, 64-sample blocks, best of 3, load average 1.9 to 2.2
(`$BLOCKAMP_PRIVATE/build/dsp/g3-final.log`):

| quality | case | core % | worst block % | gate |
|---|---|---|---|---|
| Standard (6x) | typical | 29.9 | 38.6 | 40 % |
| Standard | hot | 30.8 | 39.0 | 50 % |
| Standard | torture | 60.9 | 73.1 | 90 % |
| Standard | knob sweep | 45.9 | 58.5 | 90 % |
| High (8x) | typical | 39.5 | 50.8 | 85 % |
| High | torture | 76.3 | 91.8 | - |

- **High is not real-time safe on full-scale noise at a 96 kHz host rate**: it needs about 3 % more than one
  core (measured by the engine package). High at 176.4 and 192 kHz runs as Standard for the same reason.
- Through the plug-in (`magmora-host <bundle> cost`, plucked-string test signal, load average 1.7): 25.0 % of
  one core at 64-sample blocks, 30.4 % at 1-sample blocks; worst block while one knob is swept 0 to 10 in
  200 ms: 30 to 43 % (post lead 43 %).

## G4: aliasing

`eval/aliasing.py` through the engine; the target is the level that the better of two established commercial
simulators reaches in the same test (measured privately). Strongest non-harmonic below 20 kHz relative to the
strongest harmonic, worst over 10 mV / 0.1 V / 0.5 V, lead channel, knobs at 7, 48 kHz host:

| fundamental | target | Standard (6x) | High (8x) | Ultra (16x, earlier engine state) |
|---|---|---|---|---|
| to 2.7 kHz | -44.8 dB | -59.5 | -62.7 | -74.6 |
| to 6.3 kHz | -40.0 dB | -49.1 | -53.6 | -65.7 |
| to 10 kHz | -36.2 dB | -40.0 | -51.3 | -59.9 |

As drawn at Standard: -60.9 / -50.6 / -38.2 dB. All pass.

## G5: automation

`magmora-host <bundle> automation` (plug-in test host, both formats). Each smoothed parameter swept over its
whole range in 50 ms during a clean 220 Hz tone, automation sent only through the host's processing call.
Measured as the strongest component above 12 kHz in 10 ms frames, relative to the tone, against the same
measure with the knob held: every parameter within 1.2 dB of its held value, at -79 dB or lower (pre rhythm
-79.1 dB held -78.0). This is a proxy for the gate's "no component outside the harmonics", which needs finer
frequency resolution than 10 ms frames allow. Relay switches (input jack, channel, bright, crunch, standby,
power) on silent input: no step above -200 dBFS. Bypass switching: largest sample step 0.215 against the
signal's own 0.213. The engine package's own G5 test is part of its ctest.

## G6: latency and formats

`magmora-host <bundle> audio`, both formats, 67 of 67 checks:

- Reported latency equals the measured impulse delay exactly at every rate, quality and phase: Standard
  79 / 49 / 19 / 19 / 0 / 0 samples at 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz; Minimum phase 0 everywhere;
  48 kHz Standard 49 samples (gate at most 64). A latency change is announced to the host.
- Layouts 1-1, 1-2, 2-2 at all six rates and block sizes 1, 7, 64, 512, 4096: no silence, no non-finite
  sample; 2-1 refused. Output independent of block size to -240 dBFS; in-place and short blocks identical.
- Ultra plays at High in real time (same latency and CPU) and at Ultra offline (latency 50, 1.8 times the CPU).
- Audio-thread resets and state loads from a worker thread while audio runs: slowest block 2.4 ms.

## G7: licence audit

`python3 scripts/license_audit.py --cmake-build <build>/dsp --plugin-build <build> --notices THIRD_PARTY_NOTICES.md`:
312 compiled files, all in plug-in targets, 253 of them third-party; 3 bundled fonts; 790 files git would
publish. **license audit: OK**, zero findings. The vendored iPlug2 carries one patch (BUILD.md), reproduced
byte for byte by `scripts/vendor_iplug2.sh`.

## G8: plug-in validation

- pluginval 1.0.4, strictness 10, VST3, rates 44.1 / 48 / 96 / 192 kHz, blocks 1 to 4096: **SUCCESS**
  (25 test groups). `plugin/scripts/validate_mac.sh`.
- Test host, VST3 / AU: audio 67 / 67, automation 22 / 22, state 15 / 14, UI 57 / 53, all passing. State:
  50 random states bit exact on save, load, save; two instances of one state null; version 1 states load;
  unknown keys skipped; 2 000 corrupted and every truncated state refused without a change; the value set and
  editor size restored.
- Soak, VST3, 64-sample blocks, random automation of every automatable parameter, 1 800 s of audio in six
  slices of 300 s (seeds 1 to 6), the test host's render thread at QOS_CLASS_USER_INTERACTIVE as a host's audio
  thread is: no non-finite sample, peak 3.2 full scale. Worst block in thread CPU time: 64 to 91 % of the
  64-sample budget in four slices. Two slices had blocks over the budget: seed 5, 42 blocks (worst 134 %), run
  while the system was freeing about 6 GB of swap at a load average of 6 to 9; seed 2, 2 blocks (worst 121 %).
  Run again on the quieter machine, seed 5 had none (worst 64 %) and seed 2 had one, at a different block
  (101 %). The overruns are therefore the machine, not the engine; earlier soak overruns were the render thread
  running on efficiency cores, about three times slower, before it was given the audio-thread QoS. The engine
  package measures the engine's own worst block at 58.9 % on a replay of the soak and 88 % on noise stress.
  Logs: `$BLOCKAMP_PRIVATE/build/plugin/logs/soak-*.log`.
- Solver fallbacks were not counted during the soak (the plug-in does not expose them).

## G9: UI and art

- `art/check_assets.py`: passes (art package).
- Screenshots of the editor's own drawable, both formats, 1x and 2x, 75 / 100 / 125 / 150 %, knobs at 0, 5 and
  10, both channels, standby, power off, menus and the about page: `plugin/scripts/ui_snapshot.sh`.
- Bar and plate meet without a seam: the rows on either side differ by at most 1 of 255.
- Bundle size: AU 8.7 MB, VST3 9.2 MB, app 9.3 MB (gate 25 MB).
- **Not measured:** GPU memory per instance (estimate 28 MB of textures plus 12.7 MB of frame buffer at 2x,
  gate 40 MB), misregistration in points, the originality review by a person with reference photos.

## G-X4: the whole chain in the plug-in (XP7, 23 Nov 2026)

Pedal, head, EQ, cabinet (virtual microphone on the user's own pack) and the response-file slot: 69 parameters,
of which `cab_floor` is hidden (VST3 kIsHidden and read-only, left out of the AU parameter list: auval publishes
68) and not automatable.

- pluginval 1.0.4, strictness 10: VST3 at 44.1 / 48 / 96 / 192 kHz, blocks 1 to 4096, **SUCCESS**; AU **SUCCESS**.
  `auval` (former codes): **AU VALIDATION SUCCEEDED** (the one warning, "Preset name is not retained in
  retrieved class data", was there before XP7). The AU ran from a temporary copy in
  ~/Library/Audio/Plug-Ins/Components, removed afterwards. Logs: `$BLOCKAMP_PRIVATE/build/plugin/logs/xp7-pluginval/`.
- Test host, VST3 / AU: audio 69 / 69, state 22 / 21, automation 24 / 23, UI 67 / 63, all passing.
  - State: the session is Chain's container ("SCCH"), whose "head" section is the head chunk unchanged. A 0.1.0
    session (`dsp/tests/data/head-state-0.1.0.bin`) and a head-only version 2 session load with every block off
    and save as a chain state; a section of a later version ("zzzz") is kept and written back; the pack folder
    and the response file are saved as references, and a missing one is reported on its page and its reference
    kept; a session with every block on renders the same after save and load (-130 dBFS, as two instances set
    alike); factory presets: a full-chain preset sets its blocks, a head preset leaves them, Bedrock clears them.
  - Automation: every one of the 46 automatable block parameters swept through the processing call alone, with
    the whole chain on the harness's synthetic pack: output finite, value saved, and 43 change the sound; the
    other 3 have no effect by design (listed by the test: the EQ position while the chain is linear, alignment
    of a pack declared aligned, the empty FX slot).
  - Latency: unchanged by the blocks (49 samples at 48 kHz Standard with and without every block on, Chain's
    `latencySamples` is the head's); a one-sample response file in the slot leaves the output unchanged
    (-163 dBFS). The head's load setting is unchanged by the cabinet.
  - UI: the section words switch pages; a knob and a switch on each page drag or toggle their parameter with one
    gesture; a page covers the amp, whose knobs do not move beneath it, and respond again on the amp page.
- Soak with every block on (pedal, EQ, both microphones on the synthetic pack, a response file), random automation
  of all 69 parameters, 64-sample blocks, render thread at QOS_CLASS_USER_INTERACTIVE: VST3 seeds 101 and 102, AU
  seed 201, 150 s each (450 s; the 1 800 s of G8 was not repeated). **No allocation or free inside the plug-in's
  render call** in 337 500 blocks (malloc, calloc, realloc, posix_memalign and free interposed on that thread);
  no non-finite sample. Mean 36 to 38 % of the budget. **Blocks over the budget in thread CPU time: 1, 10 and 3**
  (worst 110, 177, 131 %). They come in bursts on consecutive blocks after unrelated parameters and do not repeat
  with the same seed (seed 101 over 20 s, three runs: 0, 8 and 33 blocks over, at different places), at a load
  average of about 3 from other work on the machine, so they are attributed to the machine; this is not proven.
- Screenshots of each page, both formats: `$BLOCKAMP_PRIVATE/renders/ui/xp7/` (`*-page-pedal`, `-eq`, `-cab`,
  `-cab-menu`, `-cab-missing`, `-ir`, `1x-100-page-cab-empty`). The pages are placeholders until their scenes are
  rendered (XP8).
- Licence audit after rebuilding `$BLOCKAMP_PRIVATE/build/core` and the plug-ins: zero findings.

## G-X4b: a response file as a cabinet slot's source (23 Nov 2026)

71 parameters: `mic1_source` and `mic2_source` (pack microphone or response file; stepped, not automatable, saved
in the session and in presets) appended; `cab_floor`, `user_ir_on` and `user_ir_level` hidden and not automatable
(VST3 kIsHidden, left out of the AU list: auval publishes 68). Each slot's file is a session reference, loaded on
the main thread after a state load, its error shown in the slot's menu; a missing one is kept on save.

- pluginval 1.0.4, strictness 10: VST3 (44.1 to 192 kHz, blocks 1 to 4096) **SUCCESS**, AU **SUCCESS**; auval
  **AU VALIDATION SUCCEEDED** (the old preset-name warning only). The AU ran from a temporary copy in
  ~/Library/Audio/Plug-Ins/Components, removed afterwards and checked to be gone. Logs:
  `$BLOCKAMP_PRIVATE/build/plugin/logs/xp8b-pluginval/`.
- Test host, VST3 / AU: audio 69 / 69, state 24 / 23, automation 24 / 24, UI 76 / 72, all passing. New checks:
  an XP7 session with the series file on ("cabp" version 1 and "uir ") opens with the file as slot 2's source, on,
  at the old level + 3.01 dB, slot 1 off, and renders as a session made that way now (-157 dBFS VST3, -147 AU; the
  file is heard: -10 dBFS against the head alone); missing pack and slot files keep their references; a
  one-sample file as slot 1's source, centred at +3.01 dB, leaves the output unchanged and adds no latency
  (-187 dBFS); the three hidden parameters are hidden and not automatable; on the page, a file slot's position knob
  is inert and its pan works; the source menu puts a pack microphone in a file slot and the slot's loaded file back,
  one gesture each. Of the 44 automatable block parameters, 37 change the sound; 7 have no effect by design on the
  harness's one-distance, one-angle pack: eq_position, mic1/2 distance and angle, mic_align, fx_on.
- Soak, whole chain (slot 1 a pack microphone, slot 2 a file), random automation of all 71 parameters, a slot's
  source switched every 750 blocks and a session with other files in the slots loaded every 3 000: VST3 seed 301
  and AU seed 401, 150 s each. **No allocation or free inside the plug-in's render call** in 225 000 blocks with
  224 source switches and 74 file-loading session loads. Mean 41 to 42 % of the budget. **Blocks over budget in
  thread CPU time: 5 and 4** (worst 145 %, 104 %), each a burst of consecutive blocks, away from the switches and
  loads, at a load average of about 2.4: the pattern of G-X4, attributed to the machine, not proven.
- Presets: none sets the cabinet or a file, so the new pan law changes none; they were not re-levelled. Their
  `user_ir` objects (all off) were removed; generate.py now refuses hidden keys and a `user_ir` object.
- Screenshots, both formats: `$BLOCKAMP_PRIVATE/renders/ui/xp8b/` (`*-page-cab` two microphones, `*-page-cab-ir`
  slot 2 holding a file, `2x-100-page-cab-ir-menu` its source menu open, plus the other pages and sizes).
- Licence audit after rebuilding `$BLOCKAMP_PRIVATE/build/core` and the plug-ins: zero findings.

## G-X4c: the built-in modelled cabinet as the default (23 Nov 2026)

A new instance has the cabinet on with the built-in pack (r8a, fingerprint 1a1824ea7a9a9376; 108 responses, 1.77 MB,
compiled in). Bundles after the change: AU 20 MB, VST3 21 MB, app 21 MB (gate 25 MB; the AU's executable 4.8 MB).

- ctest 19 / 19 (build/core and build/dsp). New in `test_cab`: a pack in memory plays bit for bit as the same files in
  a folder, and its reference survives the state. Changed in `test_chain`: the presets that now switch the cabinet on
  (Bedrock, Cinder Push, Cinder Lead, Scooped Wall) are expected to, the others to leave it off; with no pack loaded
  the cabinet passes the head through, so every preset without pedal or EQ still renders as the engine, bit for bit.
- eval/cab: `test_virtualmic.py` 12 (new: every built-in name reads back to its speaker and position; the grid is
  within the 12.7 mm limit), the cabinet harness's own tests 15; all pass.
- `plugin/scripts/validate_mac.sh --temporary-au-copy`: pluginval 1.0.4 strictness 10 **SUCCESS** on VST3 (44.1 to
  192 kHz, blocks 1 to 4096) and AU; auval **AU VALIDATION SUCCEEDED** (the old preset-name warning only); the
  temporary copy was removed. Test host VST3 / AU: audio 69 / 69, state 25 / 24, automation 24 / 23 (the hidden
  parameter check is VST3 only), UI 76 / 72, soak 3 / 3, all passing. Soak (60 s each, whole chain, random
  automation): no allocation on the render thread in 45 000 blocks each, **0 blocks over real time** (worst 82 % and
  84 % of the budget). New state check: a new instance plays the built-in cabinet (-6.0 dBFS against the head alone)
  and a session saved from it restores it (-168.6 / -174.6 dBFS). Changed checks, each to keep its meaning now that
  the cabinet is on by default: "the head alone" instances switch the cabinet off; the XP7 migration fixture is
  saved with the cabinet off as XP7 saved it; 0.1.0 and head-only sessions are also checked to load with the cabinet
  off; Bedrock is checked to set the blocks as a new instance has them (pedal and EQ off, cabinet on).
- CPU (`magmora-bench --quality standard --chain-cab`, load 1.9 to 2.4): a new instance 24.0 % typical, worst block
  29.9 %; pedal, EQ and two microphones 29.3 % / 33.7 % (gate 40 %).
- Licence audit after rebuilding build/core and the plug-ins: zero findings.
- Installed: AU, VST3 and app; `auval` (former codes) on the installed AU: AU VALIDATION SUCCEEDED.

## G-X4d: every section can be switched off (23 Nov 2026)

`amp_on` (id 71; 72 parameters) and a power toggle on each tab of the bar. No gate was changed; new checks only.

- ctest 20 / 20 in build/core and 20 / 20 in build/dsp (new: `test_sections`, 9 cases). Head off, every block off: the
  input delayed by the reported latency, worst error -84.3 dB re the peak at 48 kHz Standard, -109.8 dB at High,
  -83.6 dB at 44.1 kHz, -84.7 dB at 96 kHz (gate -60); a dirac lands at the reported latency; 110 Hz / 1 kHz / 4 kHz
  within 0.01 dB. Input trim, gate and output level still apply, power and standby do not; the EQ (+6.0 dB bell), a
  one-sample cabinet response (unity, no delay) and the pedal (no offset, -8.9 dB at its defaults) act as behind the
  head. Latency identical for all 16 on/off combinations of the four sections at 44.1 to 192 kHz, Standard and High,
  both phase modes. CPU of the chain with the head off: 0.7 % of the head-on time. A chain state saved before this change (fixture
  `dsp/tests/data/chain-state-a31.bin`) and a 0.1.0 head state load with the head on; amp_on round-trips; every preset
  plays the head. `test_automation`: amp_on flipped under the tone, above 4 kHz -108.3 / -108.5 dB re the tone (gate
  -80, as the pedal and EQ switches); in silence, knobs at 5 and 10, pedal off and on, at most -96.2 dBFS (gate -60).
  `test_chain` unchanged and passing (with the head on the engine is the head, bit for bit).
- eval/cab: `test_virtualmic.py` and the cabinet harness's tests, 27 tests, pass (unittest).
- generate.py and `--check` pass (the first preset still equals a new instance; every preset carries amp_on = 1).
- `plugin/scripts/validate_mac.sh --temporary-au-copy`: pluginval strictness 10 SUCCESS on VST3 (25 test groups) and
  AU; auval AU VALIDATION SUCCEEDED (the old preset-name warning only). Test host VST3 / AU: audio 73 / 73, state
  26 / 25, automation 25 / 24, UI 92 / 88, all passing. New: each section through its parameter (head off and all off
  is the input at the latency, -84.1 dB; latency 49 in every combination; each section alone changes the sound;
  amp_on automated off and on while playing, the head back within 0.08 dB), amp_on saved, older chain and head-only sessions
  load with the head on, Bedrock sets amp_on = 1, amp_on switches in silence at -240 dBFS; in the UI, each of the four
  toggles switches its parameter with one gesture and nothing else, leaves the page as it was, lights with its
  parameter (amber 156 against 20 off), the toggle's and the name's mouse targets meet without overlap, and the pedal's
  footswitch and the EQ's button light their toggles. The tab positions in the host's UI tests moved with the bar.
- Soak (60 s, whole chain, random automation of all 72 parameters): AU 0 of 45 000 blocks over real time (worst 74 %),
  no allocation. VST3: 0 of 45 000 over (worst 76 %), no allocation, on a rerun: the first run inside
  validate_mac.sh had 3 639 blocks over (worst 217 %) while an unrelated Xcode archive held the machine at load 28
  to 46; the rerun was made at load 3.8.
- CPU (`magmora-bench --quality standard --cases typical --chain-cab <builtin_r8a WAV copy>`, load 2.0): a new instance
  25.1 % typical, worst block 30.9 %; pedal, EQ and both microphones 30.0 % / 34.9 % (G-X3: 40 %); head off (cabinet
  on) 0.5 % / 0.8 %.
- Licence audit after the rebuild: OK, zero findings (351 compiled files, 3 fonts, 969 files).
- Snapshots, both formats: `$BLOCKAMP_PRIVATE/renders/ui/a32/` (the bar in every shot; `*-page-*` with sections on).
- Installed: AU, VST3 and app replaced; `auval` (former codes) on the installed AU: AU VALIDATION SUCCEEDED.

## G-T: the pitch transposer in the chain (24 Nov 2026)

Parameters `transpose` (id 72, -12..+12 st, automatable) and `transpose_mode` (id 73, Tight / Standard, structural),
both session settings. No gate was changed; new checks only (`dsp/tests/test_transpose_chain.cpp`, a case in
`test_realtime.cpp`, the transposer cases in `plugin/tests/host.mm`). The core's crossfade between two shifts now
runs its equal-power curve along a raised-cosine path (no corner at the ends): its switches went from -82.4..-90.1 dB
to -91.6..-105.3 dB above 4 kHz re the tone (gate -80), every `test_transpose` case still passing.

- Latency (chain and plug-in, 44.1 / 48 / 96 kHz, Standard and High quality): the head's at 0 st; the head's + 234 /
  255 / 513 samples (Tight, 5.3 ms) or + 472 / 512 / 1024 (Standard, 10.7 ms) with the transposer in; unchanged between
  non-zero values and through a rebuild; three host notices for in, mode, out. The bypass is aligned with it (561
  samples at 48 kHz, Standard, measured 561).
- 0 st: the chain renders bit for bit as the head alone (float and double; mono, sum, dual mono; also after the
  transposer was in and taken out again).
- Click-free (G-E2's switch criterion, 220 Hz tone, above 4 kHz re the tone, read on the head's input with the head
  off): in -91.5 to -104.6 dB, out -92.2 to -104.8 dB (also with the structure applied 33 ms after the parameter, as an
  idle timer does), mode changes -91.5 / -92.6 dB, shifts -92.2 / -103.2 dB; through the clean amp within 10 dB of
  that (the amp's treble lifts the residue by about 5 dB). Plug-in, while playing: in -90.8, mode -91.5, out
  -104.1 (VST3) / -104.4 dB (AU).
- Automation between non-zero values every 200 ms across -12..+12: -90.4 dB (Tight) and -101.7 dB (Standard) re the
  tone, no latency change, no path change; through process alone in the plug-in: -90.5 / -103.8 dB, no notice.
- The tuner reads the strings: with +5 st its note is the A2 played (pixel-identical), a played D3 differs.
- State: round trip, latency restored; the A31 chain fixture and a 0.1.0 head chunk load at 0 st, Tight; factory
  presets, user presets (written and read) and preset loads never touch either parameter.
- No allocation on the audio thread with the path switched in and out, modes changed and shifts automated (every
  routing, float and double, resets in between). A path change from another thread while audio runs: finite.
- CPU (`magmora-bench --quality standard --cases typical --chain-cab <builtin_r8a>`, load 1.6 to 1.9): a new instance
  25.6 % / worst block 31.9 %; with -5 st Tight 26.6 / 32.3 %, Standard 25.2 / 31.7 %; pedal, EQ, both microphones and
  Standard -5 st 31.7 / 37.4 % (gate 40 %). The transposer alone: 0.79 % Tight, 0.50 % Standard.
- ctest 26 / 26 (build/transpose, Release) and 26 / 26 under ASan and UBSan (build/transpose-asan). generate.py
  `--check`, `art/check_assets.py` (asset contract OK) and the licence audit (OK, zero findings; 397 compiled files,
  922 files git would publish) pass.
- `plugin/scripts/validate_mac.sh --temporary-au-copy` (the installed component moved aside first): pluginval strictness
  10 SUCCESS on VST3 and AU (25 test groups each); auval AU VALIDATION SUCCEEDED (the old preset-name warning only). Test
  host VST3 / AU: audio 83 / 83, state 36 / 36, automation 27 / 26, UI 157 / 153, threads 2 / 2, all passing.
- Soak (60 s, random automation of all 74 parameters): VST3 and AU 0 of 45 000 blocks over real time (worst 70 % and
  68 %), no allocation.
- Snapshots, both formats, 75 to 150 % at 1x and 2x, the bar at -2 st and +3 st Standard, both hint lines and the
  engine menu: `$BLOCKAMP_PRIVATE/renders/ui/transpose/`.

### G-T revision: the transposer's peak floor, onsets and Tight's FFT (24 Nov 2026)

What changed in `dsp/src/transpose`: a peak may now be up to 80 dB below the frame's largest bin (was 60), with no
absolute floor (was -120 dBFS); onsets no longer reset the partials' accumulated phases (the code is removed);
Tight zero-pads its FFT to twice the analysis length (4096 at 48 kHz; N, L and hop unchanged). Latencies unchanged:
Tight 234 / 255 / 513 samples, Standard 472 / 512 / 1024 (44.1 / 48 / 96 kHz). Default mode still Tight.

- Silence and tiny inputs without the absolute floor: silence in gives exactly zero out (a silent frame has no bin
  above 0, so no peak); noise at -300 and -380 dBFS gives finite output without subnormals and costs what full-scale
  noise costs (Tight 2.1 %, Standard 0.65 % of a core: the per-hop peak count is capped); no absolute guard was
  needed. New check: the same riff 120 dB down (scaled by 2^-20) comes out bit for bit 2^-20 times the loud output.
- New check: a 220 Hz tone with a partial at 5 kHz 70 dB below it: the partial comes out at 5 kHz x r within 0.001
  cent and 0.39 dB at -12, -5, -2 and +3 st, both modes, 44.1 / 48 / 96 kHz (with the 60 dB floor it moved with the
  220 Hz region, about -80 dB at its own place). Sines B0-E6, -12..+12 st, 44.1 / 48 / 96 kHz: worst 0.25 cents,
  0.08 dB (unchanged; Tight now with the padded FFT). The delay test also pins the FFT sizes.
- Changed checks, with the reason in each:
  - onset position: Tight within 20 % of the latency as before; Standard within 30 % (was 20 %): without the phase
    reset, Standard -12 st at 44.1 kHz lands 29 % late (every other case within 20 %).
  - switch and automation criterion (core, chain and the host's process-only automation): -80 dB above 4 kHz re the
    tone, or within 1 dB of what the shifts involved leave when held without any switch. The lower floor also moves
    some of Tight's short-window sidelobes, so a pure 220 Hz tone shifted up in Tight leaves -73.8 to -79.2 dB above
    4 kHz (+5 to +12 st) by itself; downward shifts, +2 st and every Standard shift stay under -80 (-86.7 to -105.8).
    A new check pins those held levels (Tight above +2 st: under -72 dB; the rest: under -80 dB). Switches measured:
    core Tight -5 -> +7 -78.7 dB (held -79.2), -12 -> +12 -73.6 (held -73.8), every other switch -90.0 dB or lower;
    random shifts every 4 ms -75.7 (Tight, worst held -73.8) and -102.3 (Standard). Chain, head off: +3 -> +7 Tight
    -79.1 (held -79.5), automated every 200 ms -74.6 (Tight) / -86.9 dB (Standard). Plug-in, while playing: in -84.1 /
    -83.9, mode -84.7 / -85.7, out -103.8 dB (VST3 / AU, gate -80 unchanged); automation through process -72.0 dB
    (Tight, worst held shift -72.1 / -72.5), -86.4 / -86.8 dB (Standard).
  - the transposer's own CPU target on the development machine: Tight at most 2 % (was 1 %): its FFT is twice as
    long. Measured 1.64 % Tight, 0.50 % Standard (-5 st, riff, 64-sample blocks).
- CPU (`magmora-bench --quality standard --cases typical --chain-cab <builtin_r8a>`, load 2.2 to 2.4): a new instance
  26.1 % / worst block 32.9 %; with -5 st Tight 27.6 / 32.9 %, Standard 26.8 / 33.6 %; pedal, EQ, both microphones and
  -5 st Standard 31.7 / 37.1 %, Tight 32.5 to 33.5 / 39.6 to 40.1 % (G-X3: 40 % typical).
- ctest 26 / 26 (build/transpose, Release) and 26 / 26 under ASan and UBSan (build/transpose-asan). generate.py
  `--check`, `art/check_assets.py` (asset contract OK) and the licence audit (OK, zero findings; 397 compiled files,
  922 files git would publish) pass.
- `plugin/scripts/validate_mac.sh --temporary-au-copy`: pluginval strictness 10 SUCCESS on VST3 and AU; auval AU
  VALIDATION SUCCEEDED (the old preset-name warning only). Test host VST3 / AU: audio 83 / 83, state 36 / 36,
  automation 27 / 26, UI 157 / 153, threads 2 / 2, all passing.
- Soak (60 s, random automation of all 74 parameters): VST3 and AU 0 of 45 000 blocks over real time (worst 73 % and
  80 %), no allocation.
- Bundle identifiers are now `dev.vpls.{app,audiounit,vst3}.Magmora` (AU type, subtype and manufacturer codes
  unchanged: aufx MgAp Mgmr), and the hint line's manual links point at the README on the project's repository.
- Release archive: the three bundles with debug symbols stripped (`strip -S`: no build-machine paths) and signed ad
  hoc again; `codesign --verify --deep --strict` passes on each, also after unpacking the archive. Installed from it:
  auval AU VALIDATION SUCCEEDED, test host audio 83 / 83 on the installed VST3 and AU.

## AU validation

- `auval` (former codes): AU VALIDATION SUCCEEDED, and pluginval at strictness 10 on the AU passes (23 Nov 2026,
  `plugin/scripts/validate_mac.sh --temporary-au-copy`, which removes the temporary copy afterwards).

## Not run

- The stand-alone app was built and its signature checked, but not launched: it opens the audio input and output.
- Windows: the VST3 project and the DSP build were not built on Windows. Linux: the DSP build was not run.
- CI (`.github/workflows/ci.yml`) has never run.
- Ultra aliasing on the latest engine.

## Release check 0.1.0 (24 Nov 2026, after the rename to Magmora)

- Publishable set: 979 files (`git ls-files --others --exclude-standard`). Copied alone into a throwaway folder and
  built there from nothing, nothing fetched: CMake + Ninja, `ctest` 20 / 20, `plugin/scripts/build_mac.sh` APP, AU
  and VST3; licence audit on that copy OK. Scanned for home paths, e-mail addresses, private folder names, the
  private cabinet packs' and captures' names, commercial products, secrets, `.DS_Store` and large files: none
  (the built binaries carry no local path either).
- Real tree: `ctest` 20 / 20; eval/cab unit tests 12 + 15 pass; `generate.py --check` passes; `art/check_assets.py`
  passes; licence audit OK, zero findings (382 compiled files, 351 in the plug-ins, 253 of those third-party,
  3 fonts, 979 files).
- `plugin/scripts/validate_mac.sh --temporary-au-copy`: pluginval strictness 10 SUCCESS on VST3 and AU; auval
  `aufx MgAp Mgmr` AU VALIDATION SUCCEEDED (the old preset-name warning only). Test host VST3 / AU: audio 73 / 73,
  state 26 / 25, automation 25 / 24, UI 92 / 88, soak (60 s) 3 / 3, all passing.
- Art after the rename: the head's badge, the pedal's maker line and the EQ's brand were re-rendered with the
  existing scripts (`art/render_passes.py --since`, `render_page_passes.py`) and packed; outside those words the
  images differ from the previous ones by at most sampling noise (mean below 1.2 of 255).
- **Soak, G8, 1 800 s** (VST3, whole chain, Standard, 64-sample blocks, random automation of all 72 parameters,
  four slices of 450 s, seeds 1 to 4, load average 1.0 to 1.6): no non-finite sample, no allocation on the render
  thread. In thread CPU time, seed 2 had no block over the budget (worst 77 %); seeds 1, 3 and 4 had one burst
  each of 6 to 7 consecutive blocks over (worst 126 %, 184 %, 155 %), 19 of 1 350 000 blocks in all, after
  different parameters (presence, input jack, post lead). **The gate is not met.** Seed 3 run again had none (worst
  77 %), so the bursts do not repeat at the same block; whether they are the engine or the machine is not settled.
  Logs: `$BLOCKAMP_PRIVATE/work/release-check/logs/soak-*.log`.
- Installed: the AU, VST3 and app under the former name removed; Magmora AU, VST3 and app installed;
  `auval -v aufx MgAp Mgmr` on the installed AU: AU VALIDATION SUCCEEDED.

## Windows VST3 beta, 0.1.2 (25 Nov 2026)

On the Windows Server 2025 virtual machine (Visual Studio 2022 Build Tools, MSVC 14.44, x64), from the 0.1.2 source:

- DSP, tests and tools: all 26 test suites pass (`ctest -C Release`); the VST3 builds with 0 warnings and 0 errors.
- pluginval 1.0.4 (Tracktion's Windows release, the same download checked from two machines), strictness 10, run in
  the interactive desktop session (session 0, where SSH runs, has no desktop for its own window): **SUCCESS**,
  including Editor, Open editor whilst processing and Editor Automation.
- The machine has no graphics card: Windows' own OpenGL is 1.1. Before the fix, opening the editor crashed the host
  in `glnvg__createShader` (a call through a null OpenGL 2.0 function pointer; iPlug2 does not check what glad
  loaded). The editor now checks for OpenGL 2.0 first and offers no editor without it, so pluginval's editor tests
  ran that path. **The NanoVG editor itself has not been drawn on Windows**: that needs a machine with OpenGL 2.0.
- Licences: `scripts/license_audit.py --msvc-tlog CL.read.1.tlog --msvc-tlog link.read.1.tlog` (MSVC's tracking
  logs of the VST3 build, copied from the machine) adds the Windows-only files to the audit (403 files, OK); the
  notices now include glad and khrplatform.h.
- Not played in a Windows DAW.
