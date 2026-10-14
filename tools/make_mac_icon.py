"""Build the macOS app icon (assets/icon/paintify.icns) from paintify.png.

macOS icons sit on a rounded-square tile with a soft shadow (the Big Sur
grid: an 824 px tile centred in 1024, corner radius ~185 px), so the
painted sphere is placed on a paper-white tile in the app's own chrome
colour rather than shipped as a bare circle.

    python tools/make_mac_icon.py
"""
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ICON = os.path.join(ROOT, "assets", "icon")

S = 1024
TILE = 824
RADIUS = 185
PAPER_TOP = (250, 250, 248)      # a touch lighter than the chrome (#F4F4F2)
PAPER_BOTTOM = (232, 232, 228)


def rounded_mask(size, box, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m


def main():
    off = (S - TILE) // 2
    box = (off, off, off + TILE - 1, off + TILE - 1)
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # Drop shadow under the tile.
    shadow = rounded_mask(S, (box[0], box[1] + 12, box[2], box[3] + 12), RADIUS)
    shadow = shadow.filter(ImageFilter.GaussianBlur(14)).point(lambda v: v * 0.30)
    canvas.paste((0, 0, 0, 255), (0, 0), shadow)

    # The paper tile: a gentle top-to-bottom gradient.
    grad = Image.new("RGBA", (S, S))
    px = grad.load()
    for y in range(S):
        t = min(max((y - off) / TILE, 0.0), 1.0)
        c = tuple(int(PAPER_TOP[i] + (PAPER_BOTTOM[i] - PAPER_TOP[i]) * t) for i in range(3))
        for x in range(S):
            px[x, y] = c + (255,)
    tile = rounded_mask(S, box, RADIUS)
    canvas.paste(grad, (0, 0), tile)

    # Hairline edge so the tile reads on a white Finder window.
    edge = rounded_mask(S, box, RADIUS)
    inner = rounded_mask(S, (box[0] + 2, box[1] + 2, box[2] - 2, box[3] - 2), RADIUS - 2)
    ring = ImageChops.subtract(edge, inner).point(lambda v: v * 0.10)
    canvas.paste((0, 0, 0, 255), (0, 0), ring)

    # The painted sphere, with its own contact shadow.
    sphere = Image.open(os.path.join(ICON, "paintify.png")).convert("RGBA")
    side = int(TILE * 0.80)
    sphere = sphere.resize((side, side), Image.LANCZOS)
    sx = (S - side) // 2
    sy = (S - side) // 2 - 8
    alpha = sphere.split()[3]
    contact = Image.new("L", (S, S), 0)
    contact.paste(alpha, (sx, sy + 18))
    contact = contact.filter(ImageFilter.GaussianBlur(18)).point(lambda v: v * 0.28)
    contact = ImageChops.multiply(contact, tile)
    canvas.paste((20, 30, 40, 255), (0, 0), contact)
    canvas.alpha_composite(sphere, (sx, sy))

    out = os.path.join(ICON, "paintify.icns")
    canvas.save(out, sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256),
                            (512, 512), (1024, 1024)])
    print("wrote", out)


if __name__ == "__main__":
    main()
