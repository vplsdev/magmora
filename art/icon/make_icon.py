"""The app icon: render the badge tile in Blender and write every file made from it.

  python art/icon/make_icon.py [--render badge.png] [--samples 384]

Writes:
  plugin/resources/icon/AppIcon.png    the 1024 master (macOS grid: 824 tile, 100 px margin, shadow)
  plugin/resources/icon/AppIcon.icns   16 to 512 pt at @1x and @2x, made with iconutil; the stand-alone app's icon
                                       (plugin/scripts/product.py puts it in the app bundle)
  docs/images/magmora-icon.png         256 px, for the README (shown at 128)

Without --render it renders art/icon/render_icon.py at 2048 px into the art build folder (deps.build_dir()),
which needs `blender` on the path (or $BLENDER). Every size is reduced from the 2048 render, with a light
sharpening for the 16 and 32 px images. Needs numpy and Pillow, and macOS for iconutil.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.dirname(HERE)
REPO = os.path.dirname(ART)
sys.path[:0] = [ART, HERE]

import deps  # noqa: E402

deps.ensure()

from PIL import Image, ImageFilter  # noqa: E402

from icon_grid import tile, with_shadow  # noqa: E402

UNDER = (10, 9, 9)            # laid under the render, so the bezel's rolled edge never shows the backdrop
ICONSET = [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)]


def render(samples):
    out = os.path.join(deps.build_dir(), "icon", "badge_2048.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run([os.environ.get("BLENDER", "blender"), "-b", "--factory-startup", "--python-exit-code", "1",
                    "-P", os.path.join(HERE, "render_icon.py"), "--", "--out", out, "--size", "2048",
                    "--samples", str(samples)], check=True, stdout=subprocess.DEVNULL)
    return out


def master(path):
    """The full-resolution icon: the render on its squircle, with the shadow."""
    im = Image.open(path).convert("RGBA")
    flat = Image.alpha_composite(Image.new("RGBA", im.size, UNDER + (255,)), im).convert("RGB")
    return with_shadow(tile(flat))


def reduce(icon, size):
    small = icon.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")
    if size <= 32:      # sharpen the colour only: a sharpened alpha rings on light backdrops
        alpha = small.getchannel("A")
        small = small.convert("RGB").filter(ImageFilter.UnsharpMask(radius=0.6, percent=50, threshold=0))
        small.putalpha(alpha)
    return small


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--render", help="use this 2048 render instead of rendering")
    ap.add_argument("--samples", type=int, default=384)
    args = ap.parse_args()
    icon = master(args.render or render(args.samples))

    out = os.path.join(REPO, "plugin", "resources", "icon")
    os.makedirs(out, exist_ok=True)
    reduce(icon, 1024).save(os.path.join(out, "AppIcon.png"), optimize=True)
    reduce(icon, 256).save(os.path.join(REPO, "docs", "images", "magmora-icon.png"), optimize=True)
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.mkdir(iconset)
        for points, scale in ICONSET:
            name = f"icon_{points}x{points}{'@2x' if scale == 2 else ''}.png"
            reduce(icon, points * scale).save(os.path.join(iconset, name), optimize=True)
        icns = os.path.join(tmp, "AppIcon.icns")
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", icns], check=True)
        shutil.copyfile(icns, os.path.join(out, "AppIcon.icns"))
    print("wrote plugin/resources/icon/AppIcon.png, AppIcon.icns and docs/images/magmora-icon.png")


if __name__ == "__main__":
    main()
