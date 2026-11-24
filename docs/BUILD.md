# Building Magmora

Two build systems, on purpose: **CMake** builds the portable DSP library, its tests and the
developer tools on macOS, Linux and Windows; **Xcode and Visual Studio projects** build the plug-ins, because
iPlug2 at the pinned commit has no CMake support.

## Where things go: `scripts/env.sh`

Every script sources `scripts/env.sh`. It reads `scripts/env.local.sh` if that exists (git-ignored; copy
`scripts/env.local.example.sh`) and otherwise uses defaults:

| variable | default | what |
|---|---|---|
| `BLOCKAMP_PRIVATE` | `.tools` in the checkout (git-ignored) | build trees, Python venv, ngspice, renders, private working data |
| `BLOCKAMP_PY` | `$BLOCKAMP_PRIVATE/venv/bin/python` | Python with numpy, scipy, numba, soundfile (`scripts/bootstrap_tools.sh` makes it) |
| `NGSPICE` | `$BLOCKAMP_PRIVATE/tools/ngspice/bin/ngspice` | reference simulator, only for verification |
| `BLOCKAMP_ASSETS`, `BLOCKAMP_IR` | unset | your own recordings: the folder that the paths in `eval/local_assets.json` are relative to (copy `eval/local_assets.example.json`; it names a DI track), and a cabinet IR file. Only preset levelling and the kernel benchmark read them |

Nothing a build produces is written into the source tree except `plugin/generated/` (below).

## DSP library, tests and tools (any platform)

```
cmake -S . -B build -G Ninja            # Windows: cmake -S . -B build -A x64
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Targets: `magmora_dsp` (static library), one `test_<name>` per file in `dsp/tests`, and the tools
`magmora-render` (a WAV through the whole engine, exactly as the plug-in runs it), `magmora-probe` (single
circuit nodes at the internal rate, for the SPICE comparison) and `magmora-bench` (CPU per quality). Options:
`-DBUILD_TESTING=OFF`, `-DMAGMORA_BUILD_TOOLS=OFF`, `-DMAGMORA_BUILD_BENCH=OFF`. C++17; warnings are errors in `dsp/`.

Verification tools (`scripts/bootstrap_tools.sh` builds a private venv and ngspice) are developer-only and
never linked into a plug-in.

## Plug-ins on macOS

```
plugin/scripts/build_mac.sh [APP] [AU] [VST3]     # default: all three
```

It runs `plugin/scripts/generate.py`, builds the DSP library with CMake into `$BLOCKAMP_PRIVATE/build/plugin/dsp`,
then the Xcode project `plugin/projects/Magmora-macOS.xcodeproj`, and leaves the bundles in
`$BLOCKAMP_PRIVATE/build/plugin/products/Release/`. Signing is ad hoc; nothing is installed or registered.
Environment: `MAGMORA_BUILD_DIR` (build root), `MAGMORA_ARCHS` (default `arm64`; `"arm64 x86_64"` for a universal
build), `MAGMORA_CONFIG` (`Release` or `Debug`). macOS 11 or later is the deployment target.

The product's name, IDs and version have one definition, `plugin/config.h`. `plugin/scripts/product.py`
writes the Info.plists and copies `plugin/resources/img` and `plugin/resources/fonts` into the bundles from it,
so a rename is one line there (plus the badge in the art).

### Generated files

`plugin/scripts/generate.py` writes `plugin/generated/`:

- `Layout.inc` from `plugin/resources/img/layout.json`, the art package's contract (positions, cells, frames);
- `Presets.inc` from `presets/*.json`, checked against `dsp/include/magmora/Params.h`;
- `Resources.rc`, the Windows resource list of every image and font.

They are committed, because the Visual Studio project has no step that runs Python.
`generate.py --check` fails if they are out of date; CI runs it.

### Testing the bundles without installing them

```
plugin/scripts/validate_mac.sh            # pluginval (strictness 10) on the VST3, the test host on VST3 and AU
plugin/scripts/ui_snapshot.sh [folder]    # screenshots of the editor at 1x/2x and 75-150 %
```

The test host (`plugin/tests`, built by `plugin/scripts/build_test_host.sh`) loads the bundles from the build
folder: the VST3 directly, the AU by registering it inside its own process. It opens no audio device, shows no
window, and does not move the mouse pointer. Its user-preset checks run in a folder of its own (it sets `MAGMORA_PRESETS_DIR`, which the
plug-in reads instead of `~/Library/Audio/Presets/Magmora`); `magmora-host <bundle> smoke-presets` is the one command that
touches the real folder, refuses if it exists already, and removes what it made. `auval`, and pluginval on the AU, need the component in
`~/Library/Audio/Plug-Ins/Components`; `validate_mac.sh --temporary-au-copy` makes that copy and removes it
again.

`plugin/scripts/level_presets.py` levels the factory presets on the real engine (it needs a DI recording of your own).

## VST3 on Windows

The DSP library builds with CMake as above. The plug-in project is `plugin/projects/Magmora-vst3.vcxproj`:

```
msbuild plugin\projects\Magmora-vst3.vcxproj /p:Configuration=Release /p:Platform=x64 ^
        /p:MAGMORA_DSP_LIB=%CD%\build\dsp\Release\magmora_dsp.lib
```

It uses iPlug2's GL2 back end and links the images and fonts into the binary through `plugin/generated/Resources.rc`.
This builds in CI and on a Windows Server 2025 machine with Visual Studio 2022 Build Tools (MSVC 14.44). The plug-in
has not yet been loaded and played in a Windows host. MSVC is slow on one translation unit of the engine (about 20
minutes and 4 GB of memory on 4 cores).

## The vendored iPlug2

`third_party/iPlug2` is a copy of iPlug2 at commit `de5a4fb14fd964247f0c70f2c90b32876a3bfc7c` with only the
subtrees the build needs, plus the MIT-licensed VST3 SDK 3.8.1 subset. It is made by

```
scripts/vendor_iplug2.sh <path-to-an-iPlug2-checkout>
```

which takes tracked files from `git archive` of the pinned commit, checks the VST3 SDK subset against a pinned
SHA-256, then **applies our patches** (`scripts/iplug2_patches/apply.py`) and writes `VENDORED.md` and
`MANIFEST.sha256`. Today there is one patch: the macOS text-entry number formatter, whose upstream code has no
stated licence, is replaced by our own MIT implementation (`scripts/iplug2_patches/formatter_*.mm`). The patch
locates the upstream code by its structure and fails loudly if upstream changes shape. Never edit files under
`third_party/iPlug2` by hand: `scripts/license_audit.py` checks every file against the manifest.
`IPLUG2_VENDOR_DEST=<folder>` writes the copy elsewhere, to compare it with the committed one.

## Licence audit

```
python3 scripts/license_audit.py [--cmake-build DIR] [--plugin-build DIR] [--notices THIRD_PARTY_NOTICES.md]
```

It collects the files actually compiled and bundled (from the compile database, Ninja's dependency log and the
Xcode build's dependency files), checks each against `third_party/LICENSES.json`, rejects copyleft and other
markers, private data and protected names, and writes `THIRD_PARTY_NOTICES.md`. Defaults:
`$BLOCKAMP_PRIVATE/build/core` and `$BLOCKAMP_PRIVATE/build/plugin`.
