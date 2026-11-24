"""Contract check for plugin/resources/img.

  python art/check_assets.py [folder]

Runs under any python3: without numpy and Pillow it restarts under $BLOCKAMP_PY (see deps.py).
Exits non-zero and lists every violation.
"""
import json
import math
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import deps  # noqa: E402

deps.ensure()

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import image_io  # noqa: E402
import layout as L  # noqa: E402

MAX_SIDE = 4096
MAX_GPU_MB = 40.0
ANGLE_TOLERANCE = 1.0


def pointer_angle(rgb, cx, cy, scale, r0=8.0, r1=17.0):
    """Clockwise degrees from twelve o'clock of the pale line through (cx, cy)."""
    lum = np.asarray(rgb.convert("L"), np.float64)
    radius = np.linspace(r0, r1, 46) * scale
    degrees = np.arange(-180, 180, 0.1)

    def score(offset):
        a = np.radians(degrees)[:, None]
        x = cx + radius * np.sin(a) + offset * np.cos(a) - 0.5
        y = cy - radius * np.cos(a) + offset * np.sin(a) - 0.5
        i, j = np.floor(x).astype(int), np.floor(y).astype(int)
        fx, fy = x - i, y - j
        return ((lum[j, i] * (1 - fx) + lum[j, i + 1] * fx) * (1 - fy)
                + (lum[j + 1, i] * (1 - fx) + lum[j + 1, i + 1] * fx) * fy).mean(1)

    on = sum(score(o * scale) for o in (-0.5, -0.25, 0, 0.25, 0.5)) / 5
    beside = (score(-2.6 * scale) + score(2.6 * scale)) / 2
    contrast = on - beside
    near = np.flatnonzero(contrast > 0.9 * contrast.max())
    near = near[np.abs((degrees[near] - degrees[contrast.argmax()] + 180) % 360 - 180) < 8]
    return float(np.average(degrees[near], weights=contrast[near]))


def check(folder):
    bad = []
    want = L.manifest()
    path = os.path.join(folder, "layout.json")
    if not os.path.exists(path):
        return [path + " is missing"], 0.0
    with open(path, encoding="utf-8") as f:
        got = json.load(f)
    if got != json.loads(json.dumps(want)):
        bad.append("layout.json differs from art/layout.py")

    gpu = 0
    expected = {"layout.json"}
    for asset in want["assets"].values():
        for scale in (L.SCALE, 1):
            file = image_io.names(folder, asset["file"], scale)
            expected.add(os.path.basename(file))
            if not os.path.exists(file):
                bad.append(f"missing {file}")
                continue
            im = Image.open(file)
            size = (asset["w"] * scale, asset["h"] * scale)
            if im.size != size:
                bad.append(f"{file}: {im.size}, expected {size}")
                continue
            if max(size) > MAX_SIDE:
                bad.append(f"{file}: wider or taller than {MAX_SIDE}")
            if scale == L.SCALE:
                gpu += size[0] * size[1] * 4
            if "cell" in asset:
                bad += check_atlas(file, im, asset, scale)
            elif im.mode == "RGBA" and np.asarray(im)[..., 3].min() < 255:
                bad.append(f"{file}: must be opaque")
            if asset["kind"] == "plate":
                row = np.asarray(im.convert("RGB")).astype(float)[0]
                tone = [int(L.BAR_COLOUR[i:i + 2], 16) for i in (1, 3, 5)]
                if np.abs(row - tone).mean(0).max() > 2.5 or np.abs(row - tone).max() > 9:
                    bad.append(f"{file}: top row does not meet the utility bar's colour")
    page_bad, page_memory, page_files = check_pages(folder)
    bad += page_bad
    expected |= page_files
    bad += [f"{f}: not part of the contract, yet it would be bundled"
            for f in sorted(os.listdir(folder)) if not f.startswith(".") and f not in expected]
    for name, mb in page_memory.items():
        print(f"page {name}: {mb:.1f} MB of textures at 2x (the amp's are released while it shows)")

    ids = [c["id"] for c in want["controls"]]
    if len(ids) != len(set(ids)):
        bad.append("duplicate control ids")
    for c in want["controls"]:
        if not (c["w"] / 2 <= c["x"] <= L.CANVAS_W - c["w"] / 2
                and L.BAR_H + c["h"] / 2 <= c["y"] <= L.CANVAS_H - c["h"] / 2):
            bad.append(f"{c['id']}: cell leaves the canvas")
    for c in want["controls"]:
        if any(v != int(v) for v in (c["x"] - c["w"] / 2, c["y"] - c["h"] / 2)):
            bad.append(f"{c['id']}: cell does not start on a whole point, so 1x would be blurred")
    targets = [c for c in want["controls"] if c["hit"][0]]
    for i, a in enumerate(targets):
        for b in targets[i + 1:]:
            if (abs(a["x"] - b["x"]) * 2 < a["hit"][0] + b["hit"][0]
                    and abs(a["y"] - b["y"]) * 2 < a["hit"][1] + b["hit"][1]):
                bad.append(f"mouse targets of {a['id']} and {b['id']} overlap")
    megabytes = gpu / 1e6
    if megabytes > MAX_GPU_MB:
        bad.append(f"{megabytes:.1f} MB of textures at 2x, limit {MAX_GPU_MB}")
    return bad, megabytes


def check_atlas(file, im, asset, scale):
    if im.mode != "RGBA":
        return [f"{file}: no alpha channel"]
    bad = []
    kind, cols, count = asset["kind"], asset["columns"], asset["frames"]
    w, h = (v * scale for v in asset["cell"])
    for i in range(count):
        x, y = (i % cols) * w, (i // cols) * h
        frame = im.crop((x, y, x + w, y + h))
        px = np.asarray(frame).astype(int)
        alpha = px[..., 3]
        border = np.concatenate((alpha[0], alpha[-1], alpha[:, 0], alpha[:, -1]))
        if border.max() > 8:
            bad.append(f"{file}: frame {i} reaches the edge of its cell ({border.max()})")
        if kind in ("knob", "button", "toggle", "rocker") and alpha.max() < 250:
            bad.append(f"{file}: frame {i} is empty")
        # a transparent pixel beside coloured ones carries colour; beside a black shadow it may be black
        solid = alpha >= image_io.TRUSTED_ALPHA
        coloured = solid & (px[..., :3].sum(2) > 24)
        ring = np.zeros_like(solid)
        ring[1:] |= coloured[:-1]
        ring[:-1] |= coloured[1:]
        ring[:, 1:] |= coloured[:, :-1]
        ring[:, :-1] |= coloured[:, 1:]
        ring &= alpha == 0
        if ring.any() and (px[ring][:, :3].sum(1) == 0).mean() > 0.2:
            bad.append(f"{file}: frame {i} has no colour bleed")
        if kind == "knob" and scale == L.SCALE and i in (0, count // 2, count - 1):
            solid_frame = Image.new("RGBA", frame.size, (0, 0, 0, 255))
            solid_frame.alpha_composite(frame)
            found = pointer_angle(solid_frame, w / 2, h / 2 + L.knob_axis_offset() * scale, scale)
            target = L.knob_degrees(i / (count - 1))
            if not math.isclose(found, target, abs_tol=ANGLE_TOLERANCE):
                bad.append(f"{file}: frame {i} points to {found:.2f}, expected {target:.1f} degrees")
    return bad


def check_pages(folder):
    """The block pages (pages.json): files, sizes, frames, cells, pointer angles, and each page's texture memory."""
    path = os.path.join(folder, "pages.json")
    if not os.path.exists(path):
        return ["pages.json is missing"], {}, set()
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    bad, memory, files = [], {}, {"pages.json"}
    for name in doc["order"]:
        page = doc["pages"][name]
        gpu = 0
        for key, a in page["assets"].items():
            for scale in (L.SCALE, 1):
                file = image_io.names(folder, a["file"], scale)
                files.add(os.path.basename(file))
                if not os.path.exists(file):
                    bad.append(f"{name}: missing {file}")
                    continue
                im = Image.open(file)
                if im.size != (a["w"] * scale, a["h"] * scale):
                    bad.append(f"{name}: {file} is {im.size}, expected {(a['w'] * scale, a['h'] * scale)}")
                    continue
                if max(im.size) > MAX_SIDE:
                    bad.append(f"{name}: {file} wider or taller than {MAX_SIDE}")
                if scale == L.SCALE:
                    gpu += im.size[0] * im.size[1] * 4
                if "cell" not in a:
                    continue
                if im.mode != "RGBA":
                    bad.append(f"{name}: {file} has no alpha")
                    continue
                w, h = (v * scale for v in a["cell"])
                for i in range(a["frames"]):
                    x, y = (i % a["columns"]) * w, (i // a["columns"]) * h
                    alpha = np.asarray(im.crop((x, y, x + w, y + h)))[..., 3]
                    border = np.concatenate((alpha[0], alpha[-1], alpha[:, 0], alpha[:, -1]))
                    if border.max() > 8 and not key.endswith("_glow"):
                        bad.append(f"{name}: {file} frame {i} reaches the edge of its cell ({border.max()})")
                    if alpha.max() < 200:
                        bad.append(f"{name}: {file} frame {i} is empty")
                if scale == L.SCALE and "knob" in key:
                    # The cap's centre sits a little off the cell's with the camera's elevation. One pivot, shared by
                    # the three frames, is searched on a half-point grid; every frame must then point within tolerance.
                    r = a["cell"][0] / 2
                    picks = []
                    for i in (0, a["frames"] // 2, a["frames"] - 1):
                        x, y = (i % a["columns"]) * w, (i // a["columns"]) * h
                        frame = Image.new("RGBA", (w, h), (0, 0, 0, 255))
                        frame.alpha_composite(im.crop((x, y, x + w, y + h)))
                        picks.append((frame, L.knob_degrees(i / (a["frames"] - 1)), i))
                    best = None
                    for dx in np.arange(-3, 3.01, 0.5):
                        for dy in np.arange(-6, 6.01, 0.5):
                            errs = [abs((pointer_angle(fr, w / 2 + dx * scale, h / 2 + dy * scale, scale, r * 0.25, r * 0.55)
                                         - t + 180) % 360 - 180) for fr, t, _ in picks]
                            if best is None or max(errs) < max(best[0]):
                                best = (errs, dx, dy)
                    for err, (_, t, i) in zip(best[0], picks):
                        if err > ANGLE_TOLERANCE:
                            bad.append(f"{name}: {file} frame {i} points {err:.2f} degrees off {t:.0f}")
        for c in page["controls"]:
            if c["kind"] == "window":
                continue
            if any(v != int(v) for v in (c["x"] - c["w"] / 2, c["y"] - c["h"] / 2)):
                bad.append(f"{name}: {c['id']} does not start on a whole point")
            if not (0 <= c["x"] - c["w"] / 2 and c["x"] + c["w"] / 2 <= L.CANVAS_W and L.BAR_H <= c["y"] - c["h"] / 2
                    and c["y"] + c["h"] / 2 <= L.CANVAS_H):
                bad.append(f"{name}: {c['id']} leaves the canvas")
        memory[name] = gpu / 1e6
        if gpu / 1e6 > MAX_GPU_MB:
            bad.append(f"{name}: {gpu / 1e6:.1f} MB of textures at 2x, limit {MAX_GPU_MB}")
    return bad, memory, files


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "plugin", "resources", "img")
    problems, mb = check(target)
    for problem in problems:
        print("FAIL", problem)
    print(f"asset contract: {'FAILED' if problems else 'OK'}; {mb:.1f} MB of textures at 2x")
    sys.exit(1 if problems else 0)
