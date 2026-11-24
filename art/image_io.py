"""Writing assets: straight alpha, colour bleed, and the 1x set made from the 2x set."""
import os

import numpy as np
from PIL import Image

BLEED_PX = 4
TRUSTED_ALPHA = 24      # fainter pixels lose their colour when unpremultiplied to 8 bits
JPEG = dict(quality=92, subsampling=0, optimize=True)


def bleed(image, steps=BLEED_PX):
    """Grow the colour of visible pixels into transparent ones. Alpha is untouched."""
    px = np.asarray(image.convert("RGBA")).astype(np.float32)
    known = px[..., 3] >= TRUSTED_ALPHA
    rgb = px[..., :3] * known[..., None]
    for _ in range(steps + 1):
        total, count = np.zeros_like(rgb), np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if not (dx or dy):
                    continue
                shifted = np.roll(known, (dy, dx), (0, 1))
                # nothing wraps round from the far side of the cell
                if dy:
                    shifted[0 if dy > 0 else -1, :] = False
                if dx:
                    shifted[:, 0 if dx > 0 else -1] = False
                total += np.roll(rgb, (dy, dx), (0, 1)) * shifted[..., None]
                count += shifted
        edge = ~known & (count > 0)
        rgb[edge] = total[edge] / count[edge][:, None]
        known = known | edge
    px[..., :3] = rgb
    return Image.fromarray(np.clip(px + 0.5, 0, 255).astype(np.uint8), "RGBA")


def names(folder, file, scale):
    stem, ext = os.path.splitext(file)
    return os.path.join(folder, f"{stem}@{scale}x{ext}" if scale > 1 else file)


def _clear_border(cell):
    """Faint resampling ringing on the outermost pixels would show as a hairline."""
    px = np.array(cell.convert("RGBA"))
    alpha = px[..., 3]
    for edge in (np.s_[0], np.s_[-1], np.s_[:, 0], np.s_[:, -1]):
        line = alpha[edge]
        line[line < TRUSTED_ALPHA] = 0
    return Image.fromarray(px, "RGBA")


def save_atlas(folder, file, frames, columns, scale=2):
    """Frames are RGBA at the retina scale. Each is reduced alone, so cells cannot leak into each other."""
    for k in (scale, 1):
        w, h = (v * k // scale for v in frames[0].size)
        sheet = Image.new("RGBA", (columns * w, -(-len(frames) // columns) * h))
        for i, frame in enumerate(frames):
            cell = frame if k == scale else frame.resize((w, h), Image.LANCZOS)
            sheet.paste(bleed(_clear_border(cell)), ((i % columns) * w, (i // columns) * h))
        sheet.save(names(folder, file, k), optimize=True)


def save_opaque(folder, file, image, scale=2):
    options = JPEG if file.endswith(".jpg") else dict(optimize=True)
    image = image.convert("RGB" if file.endswith(".jpg") else "RGBA")
    image.save(names(folder, file, scale), **options)
    image.resize((image.width // scale, image.height // scale), Image.LANCZOS).save(
        names(folder, file, 1), **options)
