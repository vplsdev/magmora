#!/usr/bin/env python3
"""Level the factory presets to the default one, on the real engine.

    $BLOCKAMP_PY plugin/scripts/level_presets.py [--write] [--seconds S]

Each preset is rendered with dsp/tools/magmora-render (the plug-in's engine) on the private DI named in
eval/local_assets.json, at output level 0 dB, then through the cabinet the preset plays in a new instance: since A30
the built-in modelled cabinet at its default settings (on by default; Bedrock and the full-chain presets set it on;
a head preset leaves it on), its response built by dsp/tools/magmora-cab from the WAV copy of the built-in pack that
proto/cab/make_builtin_pack.py writes privately (checked against plugin/generated/BuiltinCab.json). A preset that
switches the cabinet off would be measured without it. Before A30 the cabinet was off everywhere and every preset was
measured through the private cabinet IR ($BLOCKAMP_IR). Loudness is ITU-R BS.1770-4 integrated loudness (K-weighting,
400 ms blocks, -70 LUFS absolute and -10 LU relative gates). The target is the loudness of the first preset, which
equals the defaults and keeps output level 0 dB, so a new instance and every preset play at one loudness, except that
no preset may peak above -1 dBFS (the amp alone, before the cabinet) or leave the output level's range: clean presets,
whose crest factor is high, then stay below the target and the table says by how much.
--write stores the output levels, rounded to 0.1 dB, in presets/*.json.
Developer tool: reads private files, writes only presets/ and a scratch folder under $BLOCKAMP_PRIVATE.
"""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys

import numpy as np
import soundfile as sf
from scipy import signal

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "eval"))
import common as C  # noqa: E402

PEAK_CEILING = -1.0  # dBFS


def k_weight(x, rate):
    # BS.1770-4 pre-filter (high shelf) and RLB high-pass, designed for the given rate.
    f0, g, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = np.tan(np.pi * f0 / rate)
    vh, vb = 10 ** (g / 20), 10 ** (g / 20) ** 0.4996667741545416
    a0 = 1 + k / q + k * k
    b1 = [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0]
    a1 = [1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    f0, q = 38.13547087602444, 0.5003270373238773
    k = np.tan(np.pi * f0 / rate)
    a0 = 1 + k / q + k * k
    b2 = [1, -2, 1]
    a2 = [1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    return signal.lfilter(b2, a2, signal.lfilter(b1, a1, x))


def lufs(x, rate):
    y = k_weight(x, rate)
    block, hop = int(0.4 * rate), int(0.1 * rate)
    power = np.array([np.mean(y[i:i + block] ** 2) for i in range(0, len(y) - block, hop)])
    loud = -0.691 + 10 * np.log10(np.maximum(power, 1e-20))
    kept = power[loud > -70]
    relative = -0.691 + 10 * np.log10(np.mean(kept)) - 10
    kept = power[(loud > -70) & (loud > relative)]
    return -0.691 + 10 * np.log10(np.mean(kept))


PACK = "expansion/cab/builtin_r8a"  # under $BLOCKAMP_PRIVATE: make_builtin_pack.py's WAV copy of the built-in pack
DEFAULT_CAB_ON = 1                  # a new instance (plugin/BuiltinCab.h)


def builtin_response(private, work, rate):
    """Slot 1 of a new instance: the built-in pack's microphone at the default position, centre pan, pack gain."""
    folder = private / PACK
    manifest = json.loads((ROOT / "plugin/generated/BuiltinCab.json").read_text())
    names = sorted(p.name for p in folder.glob("*.wav"))
    data = b"".join(sf.read(folder / n, dtype="float32")[0].astype("<f4").tobytes() for n in names)
    if len(names) != manifest["files"] or hashlib.sha256(data).hexdigest() != manifest["samples_sha256"]:
        sys.exit(f"{folder} is not the built-in pack of BuiltinCab.json: run proto/cab/make_builtin_pack.py")
    tool = private / "build/dsp/dsp/tools/magmora-cab"
    prefix = work / "builtin-default"
    report = subprocess.run([str(tool), "--pack", str(folder), "--rate", str(rate), "--out", str(prefix)],
                            check=True, timeout=120, capture_output=True, text=True).stdout
    gain = json.loads(report)["pack_gain"]
    return np.fromfile(str(prefix) + ".L.f64", dtype="<f8") * gain


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--write", action="store_true")
    a.add_argument("--seconds", type=float, default=40.)
    args = a.parse_args()
    private = pathlib.Path(os.environ["BLOCKAMP_PRIVATE"])
    render = private / "build/dsp/dsp/tools/magmora-render"
    work = private / "build/plugin/level"
    work.mkdir(parents=True, exist_ok=True)

    di, rate = sf.read(C.asset("di"), always_2d=True)
    di = di[: int(args.seconds * rate), 0]
    source = work / "di.wav"
    sf.write(source, di, rate, subtype="FLOAT")
    ir = builtin_response(private, work, rate)
    files = sorted((ROOT / "presets").glob("*.json"))
    rows = []
    for path in files:
        preset = json.loads(path.read_text())
        params = {k: v for k, v in preset["params"].items() if k != "output_level"}
        out = work / (path.stem + ".wav")
        cmd = [str(render), str(source), str(out)]
        for k, v in params.items():
            cmd += ["--set", f"{k}={v}"]
        # The blocks a full-chain preset sets (pedal, EQ), through magmora::Chain. The cabinet is linear and last
        # (the EQ is linear too), so it is applied here, with the response the plug-in builds for its defaults.
        chain = [f"{k}={v}" for block in ("pedal", "eq", "fx") for k, v in preset.get(block, {}).items()]
        cmd += ["--chain-set", *chain, "cab_on=0"]
        subprocess.run(cmd, check=True, timeout=300, stdout=subprocess.DEVNULL)
        amp, _ = sf.read(out, always_2d=True)
        cab_on = preset.get("cab", {}).get("cab_on", 0 if "cab" in preset else DEFAULT_CAB_ON)
        cab = signal.fftconvolve(amp[:, 0], ir)[: len(amp)] if cab_on else amp[:, 0]
        rows.append((path, preset, lufs(cab, rate), 20 * np.log10(np.max(np.abs(amp[:, 0])) + 1e-12)))

    target = rows[0][2]
    print(f"target {target:.2f} LUFS (first preset, output level 0 dB; DI {args.seconds:.0f} s, through the built-in cabinet)")
    for path, preset, loudness, peak in rows:
        level = 0.0 if path == rows[0][0] else round(min(target - loudness, PEAK_CEILING - peak, 12.0), 1)
        print(f"{preset['name']:<14} {loudness:7.2f} LUFS  amp peak {peak:6.1f} dBFS  output level {level:+5.1f} dB"
              f"  peak after {peak + level:6.1f} dBFS  vs target {loudness + level - target:+5.1f} LU")
        if args.write and path != rows[0][0]:
            preset["params"]["output_level"] = level
            text = json.dumps(preset, indent=2) + "\n"
            path.write_text(text)


if __name__ == "__main__":
    main()
