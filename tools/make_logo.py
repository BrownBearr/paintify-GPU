"""Render the Paintify logo with Paintify itself: an impasto sphere painted
with the classic brush, its strokes swirling around the form.

Draws a shaded cerulean circle, paints it headless with the given
paintify.exe, cuts the painted sphere out with a soft circular mask and
writes assets/icon/paintify.ico (16-256 px, for the exe, taskbar and Start
menu) and assets/icon/paintify.png (256 px, drawn in the app's top bar).
Rendering is deterministic, so rerunning reproduces the same icon.

    python tools/make_logo.py build/paintify.exe
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

N = 1200                       # source / render size
R = N * 0.34                   # sphere radius in the source
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "assets", "icon")

# The look: classic brush, full paint height and relief, light from the upper
# left, and an edge-aware flow field so strokes wrap around the sphere.
PAINT_ARGS = ["--style", "none", "--impasto", "1.0", "--impasto-light", "1.0",
              "--light-angle", "135", "--tensor-sigma", "4", "--etf", "3"]


def source_image(path):
    """A lit cerulean sphere on warm paper; its shading sets the stroke flow."""
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    c = N / 2
    u, v = (xx - c) / R, (yy - c) / R
    z = np.sqrt(np.clip(1 - u * u - v * v, 0, 1))
    lam = np.clip(-0.45 * u - 0.55 * v + 0.70 * z, 0, 1)[..., None]
    light = np.array([0x5C, 0xB8, 0xE0], np.float32)
    dark = np.array([0x00, 0x3E, 0x66], np.float32)
    body = dark + (light - dark) * lam
    paper = np.array([0xF2, 0xEF, 0xE8], np.float32)
    a = np.clip(R - np.hypot(xx - c, yy - c) + 0.5, 0, 1)[..., None]
    img = paper * (1 - a) + body * a
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)


def cut_sphere(painted):
    """Keep the painted sphere, fade the paper and outer rings to transparent."""
    rgb = np.asarray(painted.convert("RGB"), np.float32)
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    d = np.hypot(xx - N / 2, yy - N / 2)
    edge = R * 0.995
    alpha = np.clip((edge - d) / 3.0 + 0.5, 0, 1)
    rgba = np.dstack([rgb, alpha * 255]).astype(np.uint8)
    img = Image.fromarray(rgba, "RGBA")
    m = int(N / 2 - R * 1.04)
    return img.crop((m, m, N - m, N - m))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    exe = os.path.abspath(sys.argv[1])
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "source.png")
        out = os.path.join(tmp, "painted.png")
        source_image(src)
        subprocess.run([exe, "--headless", "--in", src, "--out", out] + PAINT_ARGS,
                       check=True, cwd=os.path.dirname(exe),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        icon = cut_sphere(Image.open(out)).resize((256, 256), Image.LANCZOS)
    icon.save(os.path.join(OUT, "paintify.png"))
    sizes = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48),
             (64, 64), (128, 128), (256, 256)]
    icon.save(os.path.join(OUT, "paintify.ico"), sizes=sizes)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
