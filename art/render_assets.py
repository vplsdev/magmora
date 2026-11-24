"""Render and pack every file under plugin/resources/img.

  python3 art/render_assets.py [--only static,knobs,refs] [--state NAME ...] [--passes DIR] [--pack-only]

Every Blender run is capped (--cap seconds) and stops itself below 2.5 GB free on the internal
disk. Packing refuses passes that are older than the scene's source files.

The passes come from render_passes.py (Cycles). This script turns them into the files of the
contract: the plate, knob atlases and their per-position light overlays, switch and lamp sprites
with solved alpha, and the additive valve glow.
"""
import argparse
import glob
import os
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import deps  # noqa: E402

deps.ensure()

import numpy as np  # noqa: E402
from PIL import Image, ImageFilter  # noqa: E402

import image_io  # noqa: E402
import layout as L  # noqa: E402

K = L.SCALE
FRAMES = L.ATLASES["knob"][2]


def load(path, mode="RGB"):
    return np.asarray(Image.open(path).convert(mode)).astype(np.float32)


def rect(x, y, kind):
    w, h = (v * K for v in L.CELLS[kind])
    left, top = round(x * K - w / 2), round(y * K - h / 2)
    return np.s_[top:top + h, left:left + w]


def smoothstep(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def blend_top(rgb):
    """Bring the first row to the utility bar's tone without laying a haze over the wall.

    The slow left-to-right gradient is shifted to the tone over BLEND_TOP_PT; the fine texture
    only needs to go in the first few rows.
    """
    tone = np.array([int(L.BAR_COLOUR[i:i + 2], 16) for i in (1, 3, 5)], np.float32)
    top = rgb[:4 * K].mean(0)
    sigma = 45 * K
    x = np.arange(-3 * sigma, 3 * sigma + 1, dtype=np.float32)
    kernel = np.exp(-(x / sigma) ** 2 / 2)
    kernel /= kernel.sum()
    padded = np.pad(top, ((len(x) // 2, len(x) // 2), (0, 0)), mode="edge")
    slow = np.stack([np.convolve(padded[:, i], kernel, "valid") for i in range(3)], -1)
    y = np.maximum(np.arange(rgb.shape[0], dtype=np.float32) - 1, 0) / K
    shift = (1 - smoothstep(y / L.BLEND_TOP_PT))[:, None, None]
    flatten = (1 - smoothstep(y / 5.0))[:, None, None]
    out = rgb + (tone - slow[None]) * shift
    out = out + (slow[None] - rgb) * flatten
    return np.clip(out, 0, 255)


def feather(shape, width):
    h, w = shape
    x = np.minimum(np.arange(w), np.arange(w)[::-1]).astype(np.float32)
    y = np.minimum(np.arange(h), np.arange(h)[::-1]).astype(np.float32)
    return smoothstep(np.minimum(x[None, :], y[:, None]) / width)


def changed(before, after, coverage=None):
    """Where a pass really differs from the one before it: not noise, not antialiased print."""
    diff = Image.fromarray(np.clip(np.abs(after - before).max(2), 0, 255).astype(np.uint8))
    smooth = np.asarray(diff.filter(ImageFilter.MedianFilter(5)).filter(ImageFilter.GaussianBlur(1.2)))
    weight = smoothstep((smooth.astype(np.float32) - 3) / 6)
    if coverage is not None:
        grown = Image.fromarray(coverage.astype(np.uint8)).filter(ImageFilter.MaxFilter(5))
        weight = np.maximum(weight, np.asarray(grown.filter(ImageFilter.GaussianBlur(0.8))) / 255.0)
    return weight * feather(weight.shape, 4)


def solve(base, target, coverage=None):
    """Straight-alpha pixels that turn `base` into `target`, with the least alpha that can."""
    d = target - base
    up = np.where(d > 0, d / np.maximum(255 - base, 1e-3), 0)
    down = np.where(d < 0, -d / np.maximum(base, 1e-3), 0)
    alpha = np.maximum(up, down).max(2)
    alpha = np.where(np.abs(d).max(2) < 1, 0, alpha)
    if coverage is not None:
        alpha = np.maximum(alpha, coverage / 255.0)
    alpha = np.ceil(np.clip(alpha, 0, 1) * 255) / 255
    colour = np.clip((target - (1 - alpha[..., None]) * base) / np.maximum(alpha[..., None], 1e-6), 0, 255)
    colour *= (alpha > 0)[..., None]
    return rgba(colour, alpha * 255)


def rgba(colour, alpha):
    return Image.fromarray((np.dstack((colour, alpha)) + 0.5).clip(0, 255).astype(np.uint8), "RGBA")


def composite(sprite, base):
    a = sprite[..., 3:] / 255.0
    return sprite[..., :3] * a + base * (1 - a)


def fit_light(shared, exact):
    """Per pixel m and c such that shared * m + c is closest to exact over all frames."""
    n = len(shared)
    s, t = shared.sum(0), exact.sum(0)
    ss, st, tt = (shared * shared).sum(0), (shared * exact).sum(0), (exact * exact).sum(0)
    best = np.full(s.shape[:2], np.inf)
    keep, add = np.ones(s.shape[:2], np.float32), np.zeros(s.shape, np.float32)
    for m in np.linspace(1, 0, 101):
        c = np.clip((t - m * s) / n, 0, 255 * (1 - m))
        error = (m * m * ss + n * c * c + tt + 2 * m * c * s - 2 * m * st - 2 * c * t).sum(2)
        better = error < best - n * 0.25
        best[better] = error[better]
        keep[better], add[better] = m, c[better]
    return keep, add


def pack_plate(src, out, assets):
    clean = load(os.path.join(src, "plate.png"))
    image = Image.fromarray((blend_top(clean) + 0.5).astype(np.uint8))
    image_io.save_opaque(out, assets["bg"]["file"], image, K)
    return clean, load(image_io.names(out, assets["bg"]["file"], K))


def pack_knobs(src, out, assets, plate):
    cells = [rect(x, L.CY, "knob") for _, x, _ in L.KNOBS]
    w, h = (v * K for v in L.CELLS["knob"])
    frames = np.empty((len(cells), FRAMES, h, w, 4), np.uint8)
    for i in range(FRAMES):
        image = np.asarray(Image.open(os.path.join(src, "knobs", f"{i:02d}.png")).convert("RGBA"))
        for j, cell in enumerate(cells):
            frames[j, i] = image[cell]

    keys = [key for key, *_ in L.KNOBS]
    for name, key in L.KNOB_ATLASES.items():
        sprites = [Image.fromarray(f, "RGBA") for f in frames[keys.index(key)]]
        image_io.save_atlas(out, assets[name]["file"], sprites, L.ATLASES["knob"][0], K)

    lights = []
    for j, (key, x, _) in enumerate(L.KNOBS):
        source = keys.index(L.KNOB_ATLASES[L.knob_atlas(x)])
        if source == j:
            lights.append(Image.new("RGBA", (w, h)))
            continue
        base = plate[cells[j]]
        shared = composite(frames[source].astype(np.float32), base)
        exact = composite(frames[j].astype(np.float32), base)
        keep, add = fit_light(shared, exact)
        inside = np.maximum(frames[source][..., 3].max(0), frames[j][..., 3].max(0)) > 0
        alpha = (1 - keep) * inside
        colour = np.clip(add / np.maximum(alpha[..., None], 1e-6), 0, 255) * (alpha > 0)[..., None]
        lights.append(rgba(colour, alpha * 255))
    image_io.save_atlas(out, assets["knob_light"]["file"], lights, L.ATLASES["knob_light"][0], K)


def pack_switches(src, out, assets, plate):
    """Sprites are solved on print-free passes and laid over the printed plate.

    Where the switch itself is, its own colour is used; round it, its shadow darkens whatever is
    printed underneath by the same ratio as it darkens bare metal.
    """
    bare = load(os.path.join(src, "bare.png"))
    thrown = [load(os.path.join(src, f"switch_{v}.png")) for v in (0, 1)]
    masks = [load(os.path.join(src, f"mask_{v}.png"), "RGBA")[..., 3] for v in (0, 1)]
    items = [(key, x, y, "button", "button_" + key) for key, x, y, _, _ in L.BUTTONS]
    items += [(*L.TOGGLE, "toggle", "toggle")]
    items += [(key, x, y, "rocker", "rocker_" + key) for key, x, y, _ in L.ROCKERS]
    canvas = plate.copy()
    for key, x, y, kind, name in items:
        cell = rect(x, y, kind)
        frames = []
        for value, (image, mask) in enumerate(zip(thrown, masks)):
            cover = (mask[cell] / 255.0)[..., None]
            weight = changed(bare[cell], image[cell], mask[cell])[..., None]
            ratio = np.clip((image[cell] + 2) / (bare[cell] + 2), 0, 1.6)
            beside = plate[cell] + (plate[cell] * ratio - plate[cell]) * weight
            target = image[cell] * cover + beside * (1 - cover)
            sprite = solve(plate[cell], target, mask[cell])
            frames.append(sprite)
            if value == 1:
                canvas[cell] = composite(np.asarray(sprite).astype(np.float32), canvas[cell])
        image_io.save_atlas(out, assets[name]["file"], frames, L.ATLASES[kind][0], K)
    return bare, thrown[1], canvas


def pack_lamps(src, out, assets, bare_on, canvas):
    """Light added to the switched-on panel, so a lamp crossfades over whatever is drawn below it."""
    for cid, colour, x, y, _, _ in L.LAMPS:
        cell = rect(x, y, "glow")
        lit = load(os.path.join(src, f"lamp_{cid}.png"))[cell]
        weight = changed(bare_on[cell], lit)[..., None]
        target = np.clip(canvas[cell] + (lit - bare_on[cell]) * weight, 0, 255)
        image_io.save_atlas(out, assets["glow_" + colour]["file"], [solve(canvas[cell], target)], 1, K)


def pack_valves(src, out, assets, clean):
    x, y, w, h = (v * K for v in L.TUBE_GLOW)
    cell = np.s_[y:y + h, x:x + w]
    delta = np.maximum(load(os.path.join(src, "warm.png"))[cell] - clean[cell], 0)
    delta = delta * feather(delta.shape[:2], 7 * K)[..., None]
    image_io.save_opaque(out, assets["glow_tubes"]["file"], Image.fromarray((delta + 0.5).astype(np.uint8)), K)


def stale_passes(folder):
    """Passes the packer needs that are missing or older than any scene source file."""
    sources = glob.glob(os.path.join(HERE, "*.py")) + glob.glob(os.path.join(HERE, "parts", "*.py"))
    newest = max(os.path.getmtime(f) for f in sources if not f.endswith(("render_assets.py", "composite.py",
                                                                        "check_assets.py", "image_io.py",
                                                                        "make_placeholders.py", "deps.py", "render_passes.py",
                                                                        "render_hero.py")))
    needed = ["plate.png", "warm.png", "bare.png", "switch_0.png", "switch_1.png", "mask_0.png", "mask_1.png"]
    needed += [f"lamp_{cid}.png" for cid, *_ in L.LAMPS]
    needed += [os.path.join("knobs", f"{i:02d}.png") for i in range(FRAMES)]
    return [n for n in needed if not os.path.exists(os.path.join(folder, n))
            or os.path.getmtime(os.path.join(folder, n)) < newest]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--passes", default=os.path.join(deps.build_dir(), "passes"))
    ap.add_argument("--out", default=os.path.join(REPO, "plugin", "resources", "img"))
    ap.add_argument("--pack-only", action="store_true", help="reuse the passes that are already rendered")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--only", default="static,knobs,refs", help="which passes to render before packing")
    ap.add_argument("--state", nargs="*", help="reference states to render (default: all)")
    ap.add_argument("--cap", type=int, default=570, help="seconds before a Blender run is stopped")
    args = ap.parse_args()
    if not args.pack_only:
        command = [os.environ.get("BLENDER", "blender"), "-t", str(args.threads), "-b", "--factory-startup",
                   "--python-exit-code", "1", "-P", os.path.join(HERE, "render_passes.py"), "--",
                   args.passes, "--only", args.only] + (["--state", *args.state] if args.state else [])
        log = os.path.join(args.passes, "render.log")
        os.makedirs(args.passes, exist_ok=True)
        with open(log, "w", encoding="utf-8") as f:
            done = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT, timeout=args.cap)
        if done.returncode:
            sys.exit(f"render_passes failed ({done.returncode}); see {log}")
    stale = stale_passes(args.passes)
    if stale:
        message = f"{len(stale)} passes missing or older than the scene's source, e.g. {', '.join(stale[:4])}"
        if args.pack_only:
            sys.exit(message)
        print(message + "; nothing packed yet")
        return
    os.makedirs(args.out, exist_ok=True)
    assets = L.manifest()["assets"]
    clean, plate = pack_plate(args.passes, args.out, assets)
    pack_knobs(args.passes, args.out, assets, plate)
    _, bare_on, canvas = pack_switches(args.passes, args.out, assets, plate)
    pack_lamps(args.passes, args.out, assets, bare_on, canvas)
    pack_valves(args.passes, args.out, assets, clean)
    L.write_manifest(os.path.join(HERE, "layout.json"), os.path.join(args.out, "layout.json"))
    print("assets written to", args.out)


if __name__ == "__main__":
    main()
