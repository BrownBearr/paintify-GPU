"""Verify full-frame video repainting, per-frame variation, and saved looks.
Run after building: python tools/motion_repaint_audit.py --exe build-review/paintify.exe
Requires ffmpeg/ffprobe on PATH. Outputs stay under audit/motion-repaint.
"""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def run(*args):
    result = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True,
                            text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="build/paintify.exe")
    args = parser.parse_args()
    exe = (ROOT / args.exe).resolve()
    out = ROOT / "audit/motion-repaint"
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / "assets/sample.jpg"
    clip = out / "static.mkv"
    run("ffmpeg", "-hide_banner", "-loglevel", "error", "-loop", "1", "-i", source,
        "-vf", "scale=256:190", "-frames:v", "4", "-r", "4", "-c:v", "ffv1", "-y", clip)
    hashes = {}
    def export(name, *options):
        target = out / (name + ".mp4")
        run(exe, "--video", clip, "--out", target, "--crf", "0", "--vpreset",
            "ultrafast", "--no-audio", *options)
        text = run("ffmpeg", "-v", "error", "-i", target, "-f", "framemd5", "-")
        values = [line.rsplit(",", 1)[-1].strip() for line in text.splitlines()
                  if line and not line.startswith("#")]
        assert len(values) == 4, (name, len(values))
        hashes[name] = values
        return values
    repeatable = export("repeatable", "--style", "cezanne", "--temporal-diff", "0",
                        "--no-jitter-per-frame")
    varied = export("varied", "--style", "cezanne", "--temporal-diff", "0",
                    "--jitter-per-frame", "--save-params", out / "varied.sbr")
    loaded = export("loaded", "--params", out / "varied.sbr")
    overridden = export("overridden", "--params", out / "varied.sbr", "--no-jitter-per-frame")
    assert len(set(repeatable)) == 1, "Repeatable repaint changed a static frame"
    assert len(set(varied)) > 1, "Per-frame variation did not change the painting"
    assert loaded == varied, "Saved motion settings were lost or changed the render"
    assert overridden == repeatable, "Explicit deterministic override was ignored"
    settings = (out / "varied.sbr").read_text(encoding="utf-8")
    assert "temporal-diff 0" in settings and "jitter-per-frame 1" in settings
    (out / "results.json").write_text(json.dumps(hashes, indent=2), encoding="utf-8")
    print("PASS: whole-frame repaint; per-frame variation; saved-look reproduction; explicit override")
    print("Four decoded frames verified for each of four lossless exports.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
