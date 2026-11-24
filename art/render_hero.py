"""The review render: the whole head with controls and lit lamps, the 1280 version and a 100 % strip.

  python art/render_hero.py OUT_DIR [--samples 1024]

Needs Pillow, and `blender` on the path (or $BLENDER).
"""
import argparse
import os
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import deps  # noqa: E402

deps.ensure()

from PIL import Image  # noqa: E402

import layout as L  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--samples", type=int, default=1024)
    ap.add_argument("--threads", type=int, default=4)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    w, h = L.RENDER_W * L.SCALE, L.RENDER_H * L.SCALE
    full = os.path.join(args.out, f"hero_{w}x{h}.png")
    partial = os.path.join(args.out, ".hero.tmp.png")
    subprocess.run([os.environ.get("BLENDER", "blender"), "-t", str(args.threads), "-b", "--factory-startup",
                    "--python-exit-code", "1", "-P", os.path.join(HERE, "build_scene.py"), "--",
                    "--check", "--render", partial, "--scale", str(L.SCALE), "--samples", str(args.samples)],
                   check=True, stdout=subprocess.DEVNULL)
    os.replace(partial, full)

    image = Image.open(full).convert("RGB")
    small = os.path.join(args.out, f"hero_{L.RENDER_W}x{L.RENDER_H}.png")
    image.resize((L.RENDER_W, L.RENDER_H), Image.LANCZOS).save(small)
    strip = tuple(round(v * L.SCALE) for v in (L.HEAD[0], L.TRIM[1] - 3, L.HEAD[2], L.HEAD[3] + 6))
    crop = os.path.join(args.out, "hero_control_strip_100.png")
    image.crop(strip).save(crop)
    print("\n".join((full, small, crop)))


if __name__ == "__main__":
    main()
