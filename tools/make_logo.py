"""Render the Brushkit logo: a small textured cerulean paint drop.

Writes assets/icon/brushkit.ico (16-256 px, for the exe, taskbar and Start
menu) and assets/icon/brushkit.png (256 px, drawn in the app's top bar).
Deterministic: rerunning produces identical files.

    python tools/make_logo.py
"""
import math
import os

import numpy as np
from PIL import Image

N = 1024                      # supersampled canvas
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "assets", "icon")

rng = np.random.default_rng(7)


def smooth_noise(shape, scale):
    """Value noise: coarse random grid, bilinearly upsampled."""
    gh, gw = max(2, shape[0] // scale + 2), max(2, shape[1] // scale + 2)
    g = rng.random((gh, gw))
    img = Image.fromarray((g * 255).astype(np.uint8)).resize(
        (shape[1], shape[0]), Image.BICUBIC)
    return np.asarray(img, np.float32) / 255.0


def drop_mask():
    """Teardrop: x = sin t * sin(t/2), y = -cos t, point at the top, with a
    slightly wobbly, hand-painted edge."""
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    cx, cy, s = N * 0.5, N * 0.53, N * 0.42
    u, v = (xx - cx) / s, (yy - cy) / s          # v: -1 top .. +1 bottom
    # For a given v, the teardrop half-width is sin t * sin(t/2) with cos t = -v.
    t = np.arccos(np.clip(-v, -1.0, 1.0))
    half = np.sin(t) * np.sin(t / 2.0) * 0.92
    wobble = (smooth_noise((N, N), 220) - 0.5) * 0.022
    d = np.abs(u) - (half + wobble)
    inside = (np.abs(v) <= 1.0)
    mask = np.clip(-d * s / 2.0 + 0.5, 0.0, 1.0) * inside
    return mask, u, v


def render():
    mask, u, v = drop_mask()

    # Cerulean body: lighter upper-left, deeper lower-right (matches the UI
    # accent #0077A6).
    light = np.array([0x3F, 0xA9, 0xD6], np.float32) / 255.0
    mid = np.array([0x00, 0x77, 0xA6], np.float32) / 255.0
    deep = np.array([0x00, 0x46, 0x6E], np.float32) / 255.0
    k = np.clip(0.5 + 0.45 * v + 0.35 * u, 0.0, 1.0)[..., None]
    col = np.where(k < 0.5, light + (mid - light) * (k / 0.5),
                   mid + (deep - mid) * ((k - 0.5) / 0.5))

    # Brush texture: long slanted bristle streaks plus a little grain.
    ang = math.radians(-28)
    along = u * math.cos(ang) + v * math.sin(ang)
    across = -u * math.sin(ang) + v * math.cos(ang)
    idx = np.clip(((across + 1.5) / 3.0) * 511, 0, 511).astype(np.int32)
    broad = smooth_noise((1, 512), 18)[0][idx] - 0.5      # wide strokes
    fine = smooth_noise((1, 512), 4)[0][idx] - 0.5        # bristle lines
    fade = smooth_noise((N, N), 140)                       # load varies
    streaks = 0.7 * broad + 0.3 * fine * (0.4 + 0.6 * fade)
    grain = smooth_noise((N, N), 5) - 0.5
    col = col * (1.0 + 0.42 * streaks[..., None] + 0.06 * grain[..., None])

    # Soft specular highlight, upper left, like wet paint.
    hx, hy = -0.22, 0.12
    hl = np.exp(-(((u - hx) / 0.075) ** 2 + ((v - hy) / 0.17) ** 2))
    col = col + hl[..., None] * 0.32

    col = np.clip(col, 0.0, 1.0)
    rgba = np.dstack([col, mask])
    return Image.fromarray((rgba * 255).astype(np.uint8), "RGBA")


def main():
    os.makedirs(OUT, exist_ok=True)
    big = render()
    # Trim to the drop and pad to a square with a small margin.
    box = big.getbbox()
    crop = big.crop(box)
    side = int(max(crop.size) * 1.08)
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(crop, ((side - crop.size[0]) // 2, (side - crop.size[1]) // 2))
    sq.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "brushkit.png"))
    sizes = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48),
             (64, 64), (128, 128), (256, 256)]
    sq.resize((256, 256), Image.LANCZOS).save(
        os.path.join(OUT, "brushkit.ico"), sizes=sizes)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
