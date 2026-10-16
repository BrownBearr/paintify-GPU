"""Headless full-pipeline direction audit. Requires Python, NumPy and Pillow.

Run: python tools/direction_render_audit.py
All generated fixtures, saved looks, images and metrics go to audit/direction-deep.
The currently open application is never accessed.
"""
from __future__ import annotations
import argparse
import copy
import csv
import json
import math
from pathlib import Path
import re
import subprocess
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "audit" / "direction-deep"
FIELDS = dict(fieldMode=33, fieldMix=34, fallbackAngle=35, coherenceMin=36,
              minFieldMag=37, vortexX=38, vortexY=39, vortexSpiral=40,
              vortexRadius=41, patchCell=42, patchJitter=43, patchQuant=44,
              perturb=45, perturbScale=46, curlScale=47, waveLen=48,
              waveAmp=49, waveAngle=50, autoVortex=128)
MODES = ["image", "swirl", "patches", "curl", "waves", "fixed"]


def run(exe, *args):
    proc = subprocess.run([str(exe), *map(str, args)], cwd=ROOT,
                          capture_output=True, text=True, timeout=90)
    if proc.returncode:
        raise RuntimeError(f"{args}\n{proc.stdout}\n{proc.stderr}")
    if re.search(r"(?:GL ERROR|GL_INVALID|shader.*error)", proc.stdout + proc.stderr, re.I):
        raise RuntimeError(proc.stdout + proc.stderr)
    return proc.stdout + proc.stderr


def load_look(path):
    params = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith("#"):
            key, _, value = line.partition(" ")
            params[key] = value
    params["style-block"] = list(map(float, params["style-block"].split()))
    assert len(params["style-block"]) == 260
    return params


def write_look(path, params):
    lines = []
    for key, value in params.items():
        if key == "style-block":
            value = " ".join(format(v, ".9g") for v in value)
        lines.append(f"{key} {value}")
    path.write_text("\n".join(lines) + "\n")


def fixture(name, size=160):
    y, x = np.mgrid[:size, :size].astype(float)
    if name == "flat":
        v = np.full((size, size), .35)
    elif name == "hotspot":
        v = .25 + .12 * x / max(1, size - 1) + .4 * np.exp(
            -((x / max(1, size - 1) - .8)**2 + (y / max(1, size - 1) - .25)**2) / .008)
    else:
        v = .25 + .2 * (y if name == "ramp_y" else x) / max(1, size - 1)
    pixels = np.repeat(np.round(v * 255).clip(0, 255).astype(np.uint8)[..., None], 3, 2)
    path = OUT / f"source_{name}_{size}.png"
    Image.fromarray(pixels).save(path)
    return path


def geometry(pixels):
    # The white ground separates stroke silhouettes from the low contrast motif.
    # A mask edge's normal is perpendicular to the stroke's long axis.
    mask = (pixels.mean(2) < 240).astype(float)
    gx = mask[1:-1, 2:] - mask[1:-1, :-2]
    gy = mask[2:, 1:-1] - mask[:-2, 1:-1]
    xx, yy, xy = (float(np.sum(a)) for a in (gx*gx, gy*gy, gx*gy))
    total = xx + yy
    axis = (math.degrees(.5 * math.atan2(2*xy, xx-yy)) + 90) % 180 if total else None
    coherence = math.hypot(xx-yy, 2*xy) / total if total else 0
    return dict(coverage=float(mask.mean()), silhouette_axis_deg=axis,
                silhouette_coherence=coherence)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", help="application executable (default: detect build/paintify.exe, then build-review/paintify.exe)")
    args = parser.parse_args()
    if args.exe:
        exe = (ROOT / args.exe).resolve()
    else:
        candidates = [ROOT / "build" / "paintify.exe",
                      ROOT / "build-review" / "paintify.exe"]
        exe = next((path for path in candidates if path.is_file()), candidates[0])
    if not exe.is_file():
        parser.error(f"application executable not found: {exe}; build the app or pass --exe")
    OUT.mkdir(parents=True, exist_ok=True)
    source = fixture("ramp_x")
    base_path = OUT / "base.sbr"
    run(exe, "--headless", "--in", source, "--style", "pop", "--radii", "3",
        "--no-underpaint", "--max-len", "8", "--min-len", "8",
        "--save-params", base_path, "--out", OUT / "base.png")
    base = load_look(base_path)
    base.update({"threshold": "1", "curvature": "1", "grid-factor": "2.5",
                 "opacity": "1", "jitter-hue": "0", "jitter-sat": "0",
                 "jitter-val": "0", "brush-texture": "0", "passes": "1"})
    records, images, failures = [], {}, []

    def check(ok, message):
        print(("PASS " if ok else "FAIL ") + message, flush=True)
        if not ok:
            failures.append(message)

    def render(name, mode=0, strength=1, brush=1, source_path=source, edits=None,
               params=None, rotation=0, layers=None, enabled=1):
        look = copy.deepcopy(base)
        look.update(params or {})
        block = look["style-block"]
        block[0] = enabled
        block[1] = brush
        block[FIELDS["fieldMode"]] = mode
        block[FIELDS["fieldMix"]] = strength
        block[228+2] = rotation
        for key, value in (edits or {}).items():
            block[FIELDS[key]] = value
        if layers:
            look.update(layers)
        path = OUT / f"{name}.sbr"
        write_look(path, look)
        log = run(exe, "--headless", "--in", source_path, "--params", path,
                  "--save-params", OUT / f"{name}.saved.sbr", "--out", OUT / f"{name}.png")
        (OUT / f"{name}.log").write_text(log)
        if enabled:
            saved = load_look(OUT / f"{name}.saved.sbr")
            check(np.allclose(saved["style-block"], block, rtol=1e-5, atol=1e-6), f"{name} saved control values")
        pixels = np.asarray(Image.open(OUT / f"{name}.png").convert("RGB"), dtype=float)
        stats = re.search(r"seeds: (\d+)\s+drawn: (\d+)\s+mean points/stroke: ([\d.]+)", log)
        record = dict(name=name, mode=mode, strength=strength, brush=brush, **geometry(pixels))
        if stats:
            record.update(seeds=int(stats[1]), drawn=int(stats[2]), mean_points=float(stats[3]))
        records.append(record)
        images[name] = pixels
        return pixels

    # Mode switching is exercised by independent fully resolved looks, avoiding
    # image comparison false positives caused by changes to unrelated controls.
    for brush in (0, 1):
        prefix = "classic" if brush == 0 else "procedural"
        original = render(f"{prefix}_image", brush=brush)
        for mode in range(1, 6):
            for strength in (0, .5, 1):
                name = f"{prefix}_{MODES[mode]}_{strength}"
                pixels = render(name, mode, strength, brush)
                if strength == 0:
                    check(np.array_equal(original, pixels), f"{name} exactly matches Follow image")
    classic_off = render("classic_style_disabled", brush=0, enabled=0)
    check(np.array_equal(classic_off, images["classic_image"]), "style-off classic matches classic Follow image")

    # Analytic stroke geometry checks, beyond merely noticing image changes.
    for angle in (0, 45, 90, -45, 180):
        name = f"fixed_angle_{angle}"
        render(name, 5, edits={"fallbackAngle": angle})
        metric = records[-1]
        distance = abs(((metric["silhouette_axis_deg"] - angle + 90) % 180) - 90)
        check(distance < 12 and metric["silhouette_coherence"] > .15,
              f"{name} rendered silhouette follows requested axis ({distance:.3f} deg error)")
    render("follow_y", source_path=fixture("ramp_y"))
    check(records[-1]["silhouette_axis_deg"] is not None and
          min(records[-1]["silhouette_axis_deg"], 180-records[-1]["silhouette_axis_deg"]) < 12,
          "Follow image y ramp produces horizontal stroke geometry")
    render("layer_rotation", 5, edits={"fallbackAngle": 0}, rotation=math.pi/2,
           layers={"layer 0": "8 8 1 1 1 2.5"})
    check(abs(records[-1]["silhouette_axis_deg"] - 90) < 12, "per-layer rotation reaches raster geometry")
    for strength in (.499, .5, .501):
        render(f"orthogonal_fixed_{strength}", 5, strength, edits={"fallbackAngle": 0})

    sweeps = [(0, "perturb", 0, 1.5), (1, "vortexSpiral", -1.5, 1.5),
              (1, "vortexRadius", .05, 1), (2, "patchCell", 4, 160),
              (2, "patchJitter", 0, 1.5), (2, "patchQuant", 0, 1.5),
              (3, "curlScale", 8, 600), (4, "waveLen", 8, 1000),
              (4, "waveAmp", 0, 2), (4, "waveAngle", 0, math.pi/2)]
    for mode, key, low, high in sweeps:
        a = render(f"slider_{key}_low", mode, edits={key: low})
        b = render(f"slider_{key}_high", mode, edits={key: high})
        check(np.mean(np.abs(a-b)) > .05, f"{key} endpoints affect rendered paths")
    hotspot = fixture("hotspot")
    a = render("swirl_auto_off", 1, source_path=hotspot, edits={"autoVortex": 0})
    b = render("swirl_auto_on", 1, source_path=hotspot, edits={"autoVortex": 1})
    check(np.mean(np.abs(a-b)) > .05, "automatic swirl centre follows off-centre brightness")
    # Full-range angle endpoints represent the same axis and orientation.
    a = render("wave_minus180", 4, edits={"waveAngle": -math.pi})
    b = render("wave_plus180", 4, edits={"waveAngle": math.pi})
    check(np.mean(np.abs(a-b)) < .1, "wave angle endpoints are periodic")
    for name, low, high in [
            ("tensor_amount", {"tensor-sigma": ".5"}, {"tensor-sigma": "8"}),
            ("etf_passes", {"etf-iterations": "0"}, {"etf-iterations": "8"}),
            ("etf_radius", {"etf-iterations": "2", "etf-radius": "1"},
             {"etf-iterations": "2", "etf-radius": "12"})]:
        a = render(name+"_low", source_path=hotspot, params=low)
        b = render(name+"_high", source_path=hotspot, params=high)
        check(np.mean(np.abs(a-b)) > .01, f"{name} endpoints reach renderer")

    for smooth_name, smooth in [("tensor", {"tensor-sigma": "3"}),
                                ("etf", {"etf-iterations": "2", "etf-radius": "5"}),
                                ("both", {"tensor-sigma": "3", "etf-iterations": "2"})]:
        original = render(f"{smooth_name}_image", params=smooth)
        for mode in range(1, 6):
            zero = render(f"{smooth_name}_{MODES[mode]}_zero", mode, 0, params=smooth)
            check(np.array_equal(original, zero), f"{smooth_name} {MODES[mode]} zero preserves full pipeline")
            render(f"{smooth_name}_{MODES[mode]}_full", mode, params=smooth)

    flat = fixture("flat")
    for mode in range(6):
        render(f"flat_{MODES[mode]}", mode, source_path=flat)
        check(records[-1].get("drawn", 0) == 0 if mode == 0 else records[-1].get("drawn", 0) > 0,
              f"flat {MODES[mode]} directionless/image or painted/pattern semantics")
    for size in (1, 3, 17, 1024):
        tiny = fixture("ramp_x", size)
        for mode in range(6):
            pixels = render(f"size_{size}_{MODES[mode]}", mode, source_path=tiny)
            check(pixels.shape == (size, size, 3), f"size {size} {MODES[mode]} output dimensions")

    (OUT / "metrics.json").write_text(json.dumps(dict(records=records, failures=failures), indent=2))
    with (OUT / "metrics.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)
    print(f"Cases: {len(records)}. Failures: {len(failures)}. Results: {OUT}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
