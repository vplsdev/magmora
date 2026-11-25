# Changelog

All notable changes to Magmora are listed here. Versions follow [Semantic Versioning](https://semver.org/);
before 1.0 a minor version may change parameters, state or sound.

## 0.1.1

### Fixed

- **Cabinet packs lettered dark to bright** (ML Sound Lab's A to E, for one) played the wrong way round: the Position
  knob went darker towards the cap. A step order that only the file names give now runs from the bright end at the
  cap, whichever way the files are lettered; an order set in a `magmora-positions.json` mapping file is kept.
- **The pack's name was missing** from the Pack menu (", 80 files, ..."): a chosen folder's path ends in a slash.

### Changed

- **A slot on a pack microphone shows the pack and the microphone** at the top of its cone, as a slot with a
  response file shows the file's name.

## 0.1.0 (first public release)

First release: an AU, VST3 and stand-alone application for macOS. Code under the MIT licence. The engine and its
tests also build and pass on Windows (MSVC) and Linux (GCC), and the Windows VST3 builds; there is no Windows
download yet.

### Included

- **Head.** A two-channel, 120 W all-valve high-gain head of the early 1990s solved as a circuit in real time:
  five 12AX7s, four 6L6GCs, the output transformer, a reactive speaker load and the power supply. Every control of
  the original (Rhythm pre-gain, Bright, Crunch, Lead pre-gain, Low, Mid, High, Rhythm and Lead post-gain,
  Resonance, Presence, Standby, Power, High and Normal inputs). Two circuit value sets: Revised (default) and
  As drawn (1992).
- **Pedal.** An overdrive in front of the head (off until switched on).
- **EQ.** A four-band EQ with high-pass and low-pass after the head (off until switched on).
- **Cabinet.** Two microphone slots summed in parallel, each with level, pan and phase. Each slot plays a virtual
  microphone placed on a cabinet pack, or an impulse response file of your own. The default pack is the built-in
  **Trad 4x12 (modelled)**, a closed-back 4x12 with a 57-type microphone computed by Magmora's own physical model; you
  can choose a folder with your own pack instead.
- **Section toggles.** Pedal, Amp, EQ and Cab each have a power toggle on their tab, visible from every page. With
  the Amp off the signal passes pedal, EQ and cabinet at unity and at the same latency. The head's switch is the
  automatable parameter "Amp".
- **Bar.** Input trim, a simple noise gate, output level and meters; preset browser; engine menu.
- **Quality.** Standard (6x oversampling at 44.1 and 48 kHz), High (12x), and Ultra for offline rendering.
  Linear-phase filters (49 samples of latency at 48 kHz in Standard) or minimum-phase filters with zero latency.
- **Channels.** Mono, mono-to-stereo and stereo (left only, summed, or two independent amps).
- **19 factory presets**, level-matched through the built-in cabinet; three of them also set the pedal and the EQ.
  None needs a pack or a file.
- **User presets.** Save, Save as, rename, delete and import, inside the preset menu (no system dialogs except the
  import file picker). Plain JSON files in `~/Library/Audio/Presets/Magmora/`, one level of folders; every section is
  saved, session settings never; the cabinet's files are found where saved, next to the preset or in the last
  impulse-response folder, or kept as missing references. Files are written atomically; damaged or foreign files are
  refused; numbers are written the same in every locale. Prev/next step through factory presets, then the user's.
  The session remembers the user preset in use.
- **Transpose.** A pitch transposer at the input, -12 to +12 semitones, in the bar between Input and Gate (drag,
  scroll a semitone per notch, type, double-click for 0). At 0 st it is out of the signal: no latency, no CPU, the
  sound bit for bit as without it. Otherwise it adds 5.3 ms (Tight, for playing) or 10.7 ms (Standard, smoother low
  notes) at any rate. Every partial down to 80 dB below the strongest moves to its own shifted pitch, at any input
  level, so the quiet upper harmonics of a picked note stay in tune with it; Tight moves partials on a frequency grid
  of 11.7 Hz at 48 kHz (twice as fine as its analysis length alone gives). The latency changes only when it goes in
  or out or the mode changes, is reported to the host, and the change crossfades without a click. Automatable; mono
  like the head (each side with dual-mono routing). A session setting like the input trim: saved with the session,
  never in a preset, kept when a preset loads; older sessions load at 0 st, Tight. The tuner reads the input before it. The engine menu shows what it adds.
- **Tuner.** A chromatic tuner in the plug-in and the stand-alone app (tuning fork in the bar): note, cents needle,
  frequency, A4 from 430 to 450 Hz, output muted while open (switchable), reading from A0 (27.5 Hz). The audio thread
  only hands the input to a lock-free ring; the pitch is found on the editor's timer.
- **Section gestures.** Double-click or right-click a tab's name to switch that section on or off (the power symbol
  flashes); the power symbols' mouse targets are wider; the names of switched-off sections are easier to read.
- **Meters.** Clip marks on Input and Output that stay lit for 10 seconds or until clicked; a target band on the
  Input meter (-18 to -6 dBFS, explained in the README).
- **Hint line.** Under the bar: the name, value and purpose of the control under the pointer, with a link to the
  manual once its address is set.
- **Smaller things.** A double click resets the cabinet's microphone markers and the EQ points, as on the knobs;
  Quality, Routing and window size carry captions; arrows step through the response files of a slot's folder; the
  stand-alone app's audio settings are also on the about page.
- **Tail.** The tail reported to the host covers the head's ring-out and the cabinet response in use, so an offline
  bounce keeps the decay of a long impulse response.

### Known limits

- **The built-in cabinet is modelled, not recorded, and it is not IR-level.** Its broad tone is in range of a real
  4x12, but the fine structure of a real cabinet's response is not reproduced, and in blind listening real
  impulse responses still sound more real. For the most realistic result, load an impulse response or a pack you
  trust. See README, "The built-in cabinet".
- **macOS only** (macOS 11 or later). The release build is Apple Silicon only (arm64); an Intel or universal
  build can be made from source (`MAGMORA_ARCHS="arm64 x86_64"`), but has not been tested. The Windows VST3
  project is included but has never been built or tested. No Linux plug-in.
- **Unsigned and not notarised.** The bundles carry only Xcode's ad-hoc signature, so macOS Gatekeeper blocks them
  until you allow them (see README, "Install").
- Some verification gates fail (a harmonic gate with the lead channel driven hard, one small-signal gate by
  0.013 dB, several band-balance gates against the captures; High quality is not real-time safe on full-scale
  noise at 96 kHz). Details in [docs/VERIFICATION.md](docs/VERIFICATION.md).
- In a 30-minute random-automation soak at Standard (64-sample blocks), three short bursts of blocks went over
  real time (19 of 1.35 million blocks); see docs/VERIFICATION.md, "Release check 0.1.0".
- Not modelled: the effects loop, mains hum, microphonics, valve warm-up and ageing, transformer core saturation,
  relay switching transients.
- The stand-alone application has been built and its signature checked, but it has not been tested with an audio
  interface by the project.
