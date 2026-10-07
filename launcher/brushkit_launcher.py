"""Brushkit launcher: a point-and-click front end for brushkit.exe.

Pick a picture or a video, pick a style, see it painted, then save it,
export the video, play it painted in a window, or paint a live
TouchDesigner feed. The renderer does all of the painting; this window
only drives it:

  previews and stills   one warm `brushkit --serve` process
                        (it starts in ~1 s, then paints a preview in ~30 ms)
  video export          `--video in --out out.mp4`, progress read from stdout
  play / fine-tune      the renderer's own window (`--in ... [--play]`)
  live                  `--live-spout`, stopped through a stop file

Needs Python 3 with Tkinter and Pillow. Settings and scratch files live in
%LOCALAPPDATA%\\Brushkit\\launcher.
"""
from __future__ import annotations

import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from collections import OrderedDict, deque
from dataclasses import dataclass
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

try:
    from PIL import Image, ImageOps, ImageTk
except ImportError:                       # pythonw has no console to print to
    _r = tk.Tk()
    _r.withdraw()
    messagebox.showerror("Brushkit", "The launcher needs Pillow.\n\nInstall it with:\n"
                         "    python -m pip install pillow")
    sys.exit(1)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
EXE = ROOT / "build" / "brushkit.exe"
BUILD_BAT = ROOT / "tools" / "build.bat"
TD_README = ROOT / "touchdesigner" / "README.md"
SAMPLE = HERE / "sample.jpg"
DATA = Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir()) / "Brushkit" / "launcher"
CACHE = DATA / "cache"
SETTINGS = DATA / "settings.json"

NO_WINDOW = 0x08000000 if os.name == "nt" else 0   # CREATE_NO_WINDOW

VIDEO_EXT = {".mp4", ".mov", ".m4v", ".mkv", ".avi", ".webm", ".wmv", ".mpg", ".mpeg", ".gif"}
IMAGE_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".tga", ".webp", ".tif", ".tiff", ".psd"}
STB_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".tga", ".psd"}   # read by the renderer itself

PREVIEW_SIDE = 1000     # styles are tuned for ~1000 px; the saved file is full size
THUMB_SIDE = 360
SPOUT_IN, SPOUT_OUT = "Brushkit Input", "Brushkit Output"

C = {
    "bg": "#121215", "panel": "#1a1a1f", "card": "#23232a", "card_hover": "#2d2d36",
    "well": "#0d0d10", "line": "#2c2c35", "text": "#ececf0", "muted": "#a0a0ab",
    "faint": "#6a6a76", "accent": "#e3a33b", "accent_hover": "#f1b75a",
    "accent_text": "#1b1408", "ok": "#72c476", "err": "#ef6b6b", "danger": "#c9483f",
    "danger_hover": "#dc5a50",
}


# ── small helpers ──────────────────────────────────────────────────────────

def fmt_time(seconds: float) -> str:
    seconds = max(0, int(round(seconds)))
    m, s = divmod(seconds, 60)
    h, m = divmod(m, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


def file_key(path: Path) -> str:
    st = path.stat()
    return hashlib.sha1(f"{path}|{st.st_mtime_ns}|{st.st_size}".encode()).hexdigest()[:12]


def find_ffmpeg() -> tuple[str | None, str | None]:
    ff, fp = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if ff and fp:
        return ff, fp
    for d in (Path("C:/ffmpeg/bin"), ROOT / "ffmpeg" / "bin"):
        if (d / "ffmpeg.exe").exists() and (d / "ffprobe.exe").exists():
            return str(d / "ffmpeg.exe"), str(d / "ffprobe.exe")
    return None, None


def load_picture(path: Path) -> tuple[Image.Image, bool]:
    """The picture upright (phone EXIF rotation applied), and whether it needed it."""
    with Image.open(path) as im:
        rotated = im.getexif().get(0x0112, 1) not in (0, 1)
        return ImageOps.exif_transpose(im).convert("RGB"), rotated


def probe_video(ffprobe: str, path: Path, env) -> dict:
    r = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries",
         "stream=codec_type,width,height,avg_frame_rate,r_frame_rate:format=duration",
         "-of", "json", str(path)],
        capture_output=True, text=True, env=env, creationflags=NO_WINDOW, timeout=60)
    data = json.loads(r.stdout or "{}")
    streams = data.get("streams", [])
    v = next((s for s in streams if s.get("codec_type") == "video"), None)
    if not v:
        raise RuntimeError("no video stream in this file")

    def rate(s: str) -> float:
        n, _, d = (s or "0").partition("/")
        try:
            return float(n) / float(d) if d and float(d) else float(n)
        except ValueError:
            return 0.0

    return {
        "width": int(v.get("width", 0)), "height": int(v.get("height", 0)),
        "fps": rate(v.get("avg_frame_rate")) or rate(v.get("r_frame_rate")),
        "duration": float(data.get("format", {}).get("duration") or 0.0),
        "audio": any(s.get("codec_type") == "audio" for s in streams),
    }


def video_frame(ffmpeg: str, path: Path, t: float, env) -> Image.Image:
    out = CACHE / f"frame_{uuid.uuid4().hex}.bmp"
    for tt in (t, 0.0):
        subprocess.run([ffmpeg, "-v", "error", "-ss", f"{tt:.3f}", "-i", str(path),
                        "-frames:v", "1", "-y", str(out)],
                       capture_output=True, env=env, creationflags=NO_WINDOW, timeout=120)
        if out.exists():
            with Image.open(out) as im:
                img = im.convert("RGB")
            out.unlink(missing_ok=True)
            return img
    raise RuntimeError("ffmpeg could not read a frame from this video")


def ascii_alias(path: Path, key: str) -> str:
    """The renderer takes ANSI paths; give it a plain-ASCII name for anything else."""
    if str(path).isascii():
        return str(path)
    alias = CACHE / f"alias_{key}{path.suffix.lower()}"
    if not alias.exists():
        try:
            os.link(path, alias)
        except OSError:
            shutil.copy2(path, alias)
    return str(alias)


def clean_cache() -> None:
    CACHE.mkdir(parents=True, exist_ok=True)
    cutoff = time.time() - 2 * 86400
    for f in CACHE.iterdir():
        try:
            if f.is_file() and f.stat().st_mtime < cutoff:
                f.unlink()
        except OSError:
            pass


def reveal(path: Path) -> None:
    subprocess.Popen(["explorer", "/select,", str(path)])


@dataclass
class Source:
    path: Path
    kind: str                     # "image" | "video"
    sample: bool = False
    key: str = ""
    width: int = 0
    height: int = 0
    fps: float = 0.0
    duration: float = 0.0
    audio: bool = False
    rotated: bool = False
    probed: bool = False
    t: float = 0.0                # video: preview frame time


# ── the warm renderer ──────────────────────────────────────────────────────

class Renderer(threading.Thread):
    """Owns one `--serve` process. Stills first, then the newest preview,
    then gallery thumbnails; a newer preview replaces one still waiting."""

    def __init__(self, post, env):
        super().__init__(daemon=True)
        self.post = post
        self.env = env
        self.cv = threading.Condition()
        self.jobs = {"still": [], "preview": [], "thumb": []}
        self.alive = True
        self.proc = None
        self.err_tail = deque(maxlen=30)

    def submit(self, kind, job):
        with self.cv:
            if kind == "preview":
                self.jobs["preview"] = [job]
            else:
                self.jobs[kind].append(job)
            self.cv.notify()

    def set_thumbs(self, jobs):
        with self.cv:
            self.jobs["thumb"] = list(jobs)
            self.cv.notify()

    def stop(self):
        with self.cv:
            self.alive = False
            self.cv.notify()

    def _drain(self, stream):
        for line in stream:
            line = line.rstrip()
            if line:
                self.err_tail.append(line)
                self.post("log", "renderer: " + line)

    def _start(self):
        p = subprocess.Popen([str(EXE), "--serve"], cwd=str(ROOT), env=self.env,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, encoding="utf-8", errors="replace", bufsize=1,
                             creationflags=NO_WINDOW)
        threading.Thread(target=self._drain, args=(p.stderr,), daemon=True).start()
        styles = []
        for line in p.stdout:
            f = line.rstrip("\n").split("\t")
            if f[0] == "style" and len(f) >= 7:
                styles.append(dict(key=f[1], name=f[2], era=f[3], years=f[4],
                                   artists=f[5], summary=f[6]))
            elif f[0] == "ready":
                return p, styles, (f[1] if len(f) > 1 else "")
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
        time.sleep(0.2)
        raise RuntimeError("\n".join(self.err_tail) or "the renderer closed during start-up")

    def run(self):
        try:
            self.proc, styles, gpu = self._start()
            self.post("ready", styles, gpu)
        except Exception as e:  # noqa: BLE001 - shown to the user as is
            self.post("fatal", str(e))
            return
        while True:
            with self.cv:
                while self.alive and not any(self.jobs.values()):
                    self.cv.wait()
                if not self.alive:
                    break
                kind = next(k for k in ("still", "preview", "thumb") if self.jobs[k])
                job = self.jobs[kind].pop(0)
            self._render(kind, job)
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=3)
        except Exception:  # noqa: BLE001
            self.proc.kill()

    def _render(self, kind, job):
        try:
            if self.proc.poll() is not None:            # crashed: start another
                self.post("log", "renderer exited; restarting it")
                self.proc, _, _ = self._start()
            req = "\t".join(["render", job["inp"], job["out"], job["style"],
                             f"{job['mult']:.3f}", job.get("params") or ""])
            self.proc.stdin.write(req + "\n")
            self.proc.stdin.flush()
            reply = self.proc.stdout.readline()
            if not reply:
                raise RuntimeError("the renderer stopped. " + " ".join(list(self.err_tail)[-3:]))
            f = reply.rstrip("\n").split("\t")
            if f[0] != "ok":
                raise RuntimeError(f[1] if len(f) > 1 else reply.strip())
            img = None
            if job.get("load"):
                with Image.open(job["out"]) as im:
                    img = im.convert("RGB")
                Path(job["out"]).unlink(missing_ok=True)
            self.post("rendered", kind, job, img, float(f[4]) if len(f) > 4 else 0.0)
        except Exception as e:  # noqa: BLE001
            self.post("render_failed", kind, job, str(e))


# ── widgets ────────────────────────────────────────────────────────────────

class FlatButton(tk.Label):
    KINDS = {
        "primary": ("accent", "accent_hover", "accent_text"),
        "secondary": ("card", "card_hover", "text"),
        "ghost": ("panel", "card", "muted"),
        "danger": ("danger", "danger_hover", "text"),
        "ghost_bg": ("bg", "card", "muted"),
    }

    def __init__(self, parent, text, command, kind="secondary", font=None, padx=14, pady=7):
        super().__init__(parent, text=text, font=font, padx=padx, pady=pady, cursor="hand2", bd=0)
        self.command = command
        self.enabled = True
        self.kind = kind
        self._paint(False)
        self.bind("<Enter>", lambda e: self._paint(True))
        self.bind("<Leave>", lambda e: self._paint(False))
        self.bind("<ButtonRelease-1>", self._click)

    def _paint(self, hover):
        bg, hv, fg = self.KINDS[self.kind]
        if not self.enabled:
            self.configure(bg=C["card"], fg=C["faint"], cursor="arrow")
        else:
            self.configure(bg=C[hv] if hover else C[bg], fg=C[fg], cursor="hand2")

    def _click(self, e):
        if self.enabled and 0 <= e.x < self.winfo_width() and 0 <= e.y < self.winfo_height():
            self.command()

    def set(self, text=None, enabled=None, kind=None):
        if text is not None:
            self.configure(text=text)
        if enabled is not None:
            self.enabled = enabled
        if kind is not None:
            self.kind = kind
        self._paint(False)


class Check(tk.Frame):
    """A checkbox drawn on a canvas, so it looks the same on a dark theme."""

    def __init__(self, parent, text, var, size, font, command=None, bg=None):
        bg = bg or C["panel"]
        super().__init__(parent, bg=bg)
        self.var, self.command, self.size = var, command, size
        self.box = tk.Canvas(self, width=size, height=size, bg=bg, highlightthickness=0,
                             cursor="hand2")
        self.box.pack(side="left")
        self.label = tk.Label(self, text=text, bg=bg, fg=C["text"], font=font, cursor="hand2")
        self.label.pack(side="left", padx=(size // 2, 0))
        for w in (self.box, self.label):
            w.bind("<Button-1>", self.toggle)
        self.draw()

    def toggle(self, _=None):
        self.var.set(not self.var.get())
        self.draw()
        if self.command:
            self.command()

    def draw(self):
        c, s = self.box, self.size
        c.delete("all")
        if self.var.get():
            c.create_rectangle(1, 1, s - 1, s - 1, fill=C["accent"], outline=C["accent"])
            c.create_line(s * .24, s * .52, s * .43, s * .72, s * .78, s * .3,
                          fill=C["accent_text"], width=max(2, s // 8), capstyle="round",
                          joinstyle="round")
        else:
            c.create_rectangle(1, 1, s - 2, s - 2, outline=C["faint"], width=max(1, s // 10))


class StyleCard(tk.Frame):
    def __init__(self, parent, info, on_click, tw, th, fonts, placeholder):
        super().__init__(parent, bg=C["card"], highlightthickness=2,
                         highlightbackground=C["card"], highlightcolor=C["card"], cursor="hand2")
        self.info = info
        self.selected = False
        self.img = tk.Label(self, image=placeholder, bg=C["well"], bd=0)
        self.img.pack(fill="x")
        name = "Classic renderer" if info["key"] == "none" else info["name"]
        sub = "Hertzmann strokes, no style" if info["key"] == "none" else \
            f"{info['era']} · {info['years']}"
        self.name = tk.Label(self, text=name, bg=C["card"], fg=C["text"], font=fonts["card"],
                             anchor="w")
        self.name.pack(fill="x", padx=8, pady=(6, 0))
        self.sub = tk.Label(self, text=sub, bg=C["card"], fg=C["muted"], font=fonts["small"],
                            anchor="w")
        self.sub.pack(fill="x", padx=8, pady=(0, 7))
        for w in (self, self.img, self.name, self.sub):
            w.bind("<Button-1>", lambda e: on_click(info["key"]))
            w.bind("<Enter>", lambda e: self._paint(True))
            w.bind("<Leave>", lambda e: self._paint(False))
        self.photo = None

    def _paint(self, hover):
        bg = C["card_hover"] if hover and not self.selected else C["card"]
        edge = C["accent"] if self.selected else bg
        self.configure(bg=bg, highlightbackground=edge, highlightcolor=edge)
        self.name.configure(bg=bg, fg=C["accent"] if self.selected else C["text"])
        self.sub.configure(bg=bg)

    def set_selected(self, on):
        self.selected = on
        self._paint(False)

    def set_thumb(self, photo):
        self.photo = photo
        self.img.configure(image=photo)


# ── the window ─────────────────────────────────────────────────────────────

class App:
    def __init__(self, root: tk.Tk, initial: str | None):
        self.root = root
        self.events: queue.Queue = queue.Queue()
        self.u = root.winfo_fpixels("1i") / 96.0
        self.settings = self._load_settings()
        self.ffmpeg, self.ffprobe = find_ffmpeg()
        self.env = os.environ.copy()
        if self.ffmpeg and not shutil.which("ffmpeg"):
            self.env["PATH"] = str(Path(self.ffmpeg).parent) + os.pathsep + self.env.get("PATH", "")

        self.styles: list[dict] = []
        self.cards: dict[str, StyleCard] = {}
        self.style_key = self.settings.get("style", "turner")
        self.saved_look: Path | None = None
        self.mult = tk.DoubleVar(value=float(self.settings.get("mult", 1.0)))
        self.steady = tk.BooleanVar(value=bool(self.settings.get("steady", True)))
        self.keep_audio = tk.BooleanVar(value=bool(self.settings.get("audio", True)))
        self.mode = "file"

        self.source: Source | None = None
        self.sample = Source(SAMPLE if SAMPLE.exists() else ROOT / "assets" / "test.jpg",
                             "image", sample=True)
        self.prepared: dict | None = None
        self.gen = 0
        self.previews: OrderedDict = OrderedDict()   # (src, look, mult) -> PIL
        self.thumbs: dict = {}                       # (src, style, mult) -> PIL
        self.shown: Image.Image | None = None
        self.painted: Image.Image | None = None
        self.comparing = False
        self._fit_after = None
        self._live_after = None

        self.renderer: Renderer | None = None
        self.ready = False
        self.export: dict | None = None
        self.live: dict | None = None
        self.last_output: Path | None = None

        self._fonts()
        self._theme()
        self._build()
        self._start_renderer()
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.bind_all("<MouseWheel>", self._wheel)
        root.bind("<Control-o>", lambda e: self.open_dialog())
        root.bind("<Control-s>", lambda e: self.primary_action())
        root.after(40, self._pump)
        if initial:
            root.after(300, lambda: self.open_path(Path(initial)))

    # ── plumbing ───────────────────────────────────────────────────────────

    def px(self, n: float) -> int:
        return int(round(n * self.u))

    def post(self, *event):
        self.events.put(event)

    def _pump(self):
        try:
            while True:
                ev = self.events.get_nowait()
                try:
                    getattr(self, "on_" + ev[0])(*ev[1:])
                except Exception as e:  # noqa: BLE001 - one bad event must not stop the rest
                    self.log(f"! {ev[0]}: {e!r}")
        except queue.Empty:
            pass
        self.root.after(40, self._pump)

    def _load_settings(self) -> dict:
        try:
            return json.loads(SETTINGS.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def _save_settings(self):
        s = dict(style=self.style_key, mult=round(self.mult.get(), 2), steady=self.steady.get(),
                 audio=self.keep_audio.get(), last_dir=self.settings.get("last_dir", ""),
                 geometry=self.root.geometry())
        try:
            DATA.mkdir(parents=True, exist_ok=True)
            SETTINGS.write_text(json.dumps(s, indent=1), encoding="utf-8")
        except OSError:
            pass

    def _fonts(self):
        self.f = {
            "title": ("Segoe UI Semibold", 17), "h": ("Segoe UI Semibold", 12),
            "step": ("Segoe UI Semibold", 11), "body": ("Segoe UI", 10),
            "bold": ("Segoe UI Semibold", 10), "small": ("Segoe UI", 9),
            "card": ("Segoe UI Semibold", 9), "big": ("Segoe UI Semibold", 11),
            "style": ("Segoe UI Semibold", 14), "mono": ("Consolas", 9),
        }

    def _theme(self):
        st = ttk.Style(self.root)
        st.theme_use("clam")
        st.configure("Dark.Vertical.TScrollbar", background=C["card"], troughcolor=C["panel"],
                     bordercolor=C["panel"], arrowcolor=C["muted"], lightcolor=C["card"],
                     darkcolor=C["card"], gripcount=0, arrowsize=self.px(12))
        st.map("Dark.Vertical.TScrollbar", background=[("active", C["card_hover"])])
        st.configure("Dark.Horizontal.TProgressbar", background=C["accent"], troughcolor=C["card"],
                     bordercolor=C["card"], lightcolor=C["accent"], darkcolor=C["accent"],
                     thickness=self.px(8))

    def _label(self, parent, text="", font="body", fg="text", bg="panel", **kw):
        return tk.Label(parent, text=text, font=self.f[font], fg=C[fg], bg=C[bg],
                        justify="left", anchor="w", **kw)

    def _button(self, parent, text, command, kind="secondary", font="bold"):
        return FlatButton(parent, text, command, kind, font=self.f[font],
                          padx=self.px(14), pady=self.px(7))

    def _hint(self, parent, text, font="small", fg="faint"):
        lbl = self._label(parent, text, font, fg, wraplength=self.px(600))
        lbl.pack(fill="x", pady=(self.px(8), 0))
        self.hints.append(lbl)
        return lbl

    def _slider(self, parent, var, lo, hi, res, on_release):
        s = tk.Scale(parent, variable=var, from_=lo, to=hi, resolution=res, orient="horizontal",
                     showvalue=False, bg=C["accent"], troughcolor=C["card"], fg=C["text"],
                     activebackground=C["accent_hover"], highlightthickness=0, bd=0,
                     sliderrelief="flat", sliderlength=self.px(22), width=self.px(12),
                     cursor="hand2")
        s.bind("<ButtonRelease-1>", lambda e: on_release())
        return s

    def _panel(self, parent, step, title):
        outer = tk.Frame(parent, bg=C["panel"])
        head = tk.Frame(outer, bg=C["panel"])
        head.pack(fill="x", padx=self.px(16), pady=(self.px(14), self.px(10)))
        badge = tk.Label(head, text=step, font=self.f["step"], bg=C["accent"], fg=C["accent_text"],
                         width=2)
        badge.pack(side="left")
        self._label(head, title, "h").pack(side="left", padx=(self.px(10), 0))
        body = tk.Frame(outer, bg=C["panel"])
        body.pack(fill="both", expand=True, padx=self.px(16), pady=(0, self.px(14)))
        return outer, body, head

    # ── layout ─────────────────────────────────────────────────────────────

    def _build(self):
        r, px = self.root, self.px
        r.title("Brushkit")
        r.configure(bg=C["bg"])
        r.minsize(px(1180), px(720))
        r.geometry(self.settings.get("geometry") or f"{px(1480)}x{px(920)}")

        # header
        head = tk.Frame(r, bg=C["bg"])
        head.pack(fill="x", padx=px(20), pady=(px(14), px(10)))
        self._label(head, "Brushkit", "title", bg="bg").pack(side="left")
        self._label(head, "Paint pictures and video in the great painting styles — on your GPU",
                    "body", "muted", "bg").pack(side="left", padx=(px(14), 0), pady=(px(6), 0))
        self.hdr_retry = self._button(head, "Check again", self._start_renderer)
        self.hdr_build = self._button(head, "Build the renderer", self.build_renderer, "primary")
        self.hdr_status = self._label(head, "●  Starting the renderer…", "body", "muted", "bg")
        self.hdr_status.pack(side="right")

        # footer: status line + optional log
        foot = tk.Frame(r, bg=C["bg"])
        foot.pack(side="bottom", fill="x", padx=px(20), pady=(px(6), px(10)))
        self.status_lbl = self._label(foot, "", "small", "muted", "bg")
        self.status_lbl.pack(side="left")
        self.log_btn = FlatButton(foot, "Show details ▸", self.toggle_log, "ghost_bg",
                                  font=self.f["small"], padx=px(8), pady=px(3))
        self.log_btn.pack(side="right")
        self.log_frame = tk.Frame(r, bg=C["bg"])
        self.log_text = tk.Text(self.log_frame, height=9, bg=C["well"], fg=C["muted"],
                                font=self.f["mono"], bd=0, highlightthickness=0, wrap="word",
                                insertbackground=C["text"])
        self.log_text.pack(fill="both", expand=True)
        self.log_text.configure(state="disabled")

        body = tk.Frame(r, bg=C["bg"])
        body.pack(fill="both", expand=True, padx=px(20))

        body.columnconfigure(2, weight=1)
        body.rowconfigure(0, weight=1)

        left, lb, _ = self._panel(body, "1", "What to paint")
        left.configure(width=px(310))
        left.pack_propagate(False)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, px(12)))
        mid, mb, _ = self._panel(body, "2", "Pick a style")
        mid.configure(width=px(446))
        mid.pack_propagate(False)
        mid.grid(row=0, column=1, sticky="nsew", padx=(0, px(12)))
        right, rb, _ = self._panel(body, "3", "Preview")
        right.grid(row=0, column=2, sticky="nsew")

        self._build_source(lb)
        self._build_styles(mb)
        self._build_preview(rb)

    def _build_source(self, parent):
        px = self.px
        seg = tk.Frame(parent, bg=C["panel"])
        seg.pack(fill="x", pady=(0, px(14)))
        self.seg_file = self._button(seg, "Image or video", lambda: self.set_mode("file"))
        self.seg_live = self._button(seg, "Live (TouchDesigner)", lambda: self.set_mode("live"))
        self.seg_file.pack(side="left", fill="x", expand=True)
        self.seg_live.pack(side="left", fill="x", expand=True, padx=(px(2), 0))

        # file mode
        self.file_box = tk.Frame(parent, bg=C["panel"])
        self.open_btn = self._button(self.file_box, "Open an image or video…", self.open_dialog,
                                     "primary", "big")
        self.open_btn.pack(fill="x")
        self._label(self.file_box, "JPG, PNG, WEBP, TIFF · MP4, MOV, MKV, AVI, WEBM", "small",
                    "faint").pack(fill="x", pady=(px(4), px(14)))
        self.src_thumb = tk.Label(self.file_box, bg=C["well"], bd=0)
        self.src_name = self._label(self.file_box, "No file yet", "bold", wraplength=px(265))
        self.src_name.pack(fill="x", pady=(px(10), 0))
        self.src_info = self._label(self.file_box, "The preview shows a sample picture until you "
                                    "open one of your own.", "small", "muted", wraplength=px(265))
        self.src_info.pack(fill="x", pady=(px(2), 0))

        self.video_box = tk.Frame(self.file_box, bg=C["panel"])
        row = tk.Frame(self.video_box, bg=C["panel"])
        row.pack(fill="x")
        self._label(row, "Preview frame", "small", "muted").pack(side="left")
        self.time_lbl = self._label(row, "0:00", "small", "muted")
        self.time_lbl.pack(side="right")
        self.time_var = tk.DoubleVar(value=0.0)
        self.time_slider = self._slider(self.video_box, self.time_var, 0, 1, 0.1, self.on_time)
        self.time_slider.configure(command=lambda v: self.time_lbl.configure(
            text=fmt_time(float(v))))
        self.time_slider.pack(fill="x", pady=(px(4), 0))
        self._label(self.video_box, "Drag to preview another moment of the clip.", "small",
                    "faint").pack(fill="x", pady=(px(2), 0))

        self.clear_btn = FlatButton(self.file_box, "Close this file", self.close_source, "ghost",
                                    font=self.f["small"], padx=px(6), pady=px(3))
        self._label(self.file_box, "Tip: drop a file onto Brushkit.bat to open it straight away.",
                    "small", "faint", wraplength=px(265)).pack(side="bottom", fill="x")

        # live mode
        self.live_box = tk.Frame(parent, bg=C["panel"])
        lb = self.live_box
        self._label(lb, "Paint whatever TouchDesigner (or any Spout app) sends, and send the "
                    "painting straight back, in real time.", "body", wraplength=px(265)).pack(
            fill="x", pady=(0, px(12)))
        steps = [
            ("In TouchDesigner, add a Spout Out TOP and name it", SPOUT_IN),
            ("Press  Start live  on the right.", None),
            ("Add a Spout In TOP and pick the sender", SPOUT_OUT),
        ]
        for i, (text, name) in enumerate(steps, 1):
            box = tk.Frame(lb, bg=C["panel"])
            box.pack(fill="x", pady=(0, px(10)))
            tk.Label(box, text=str(i), font=self.f["step"], bg=C["card"], fg=C["accent"],
                     width=2).pack(side="left", anchor="n")
            col = tk.Frame(box, bg=C["panel"])
            col.pack(side="left", fill="x", expand=True, padx=(px(10), 0))
            self._label(col, text, "body", wraplength=px(225)).pack(fill="x")
            if name:
                nrow = tk.Frame(col, bg=C["panel"])
                nrow.pack(fill="x", pady=(px(4), 0))
                tk.Label(nrow, text=name, font=self.f["mono"], bg=C["well"], fg=C["accent"],
                         padx=px(8), pady=px(3)).pack(side="left")
                FlatButton(nrow, "Copy", lambda n=name: self.copy(n), "ghost",
                           font=self.f["small"], padx=px(8), pady=px(2)).pack(side="left",
                                                                              padx=(px(6), 0))
        self._label(lb, "Prefer a ready-made component? Build Brushkit.tox once from the "
                    "TouchDesigner guide.", "small", "muted", wraplength=px(265)).pack(
            fill="x", pady=(px(6), px(6)))
        self._button(lb, "Open the TouchDesigner guide", self.open_td_guide).pack(anchor="w")

        self.set_mode("file", initial=True)

    def _build_styles(self, parent):
        px = self.px
        self.tw, self.th = px(182), px(118)
        self.placeholder = ImageTk.PhotoImage(Image.new("RGB", (self.tw, self.th), C["well"]))

        wrap = tk.Frame(parent, bg=C["panel"])
        wrap.pack(fill="both", expand=True)
        self.gal_canvas = tk.Canvas(wrap, bg=C["panel"], highlightthickness=0, bd=0,
                                    width=px(392))
        sb = ttk.Scrollbar(wrap, orient="vertical", command=self.gal_canvas.yview,
                           style="Dark.Vertical.TScrollbar")
        self.gal_canvas.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.gal_canvas.pack(side="left", fill="both", expand=True)
        self.gal = tk.Frame(self.gal_canvas, bg=C["panel"])
        win = self.gal_canvas.create_window(0, 0, window=self.gal, anchor="nw")
        self.gal.bind("<Configure>", lambda e: self.gal_canvas.configure(
            scrollregion=self.gal_canvas.bbox("all")))
        self.gal_canvas.bind("<Configure>", lambda e: self.gal_canvas.itemconfigure(
            win, width=e.width))
        self.gal.columnconfigure(0, weight=1, uniform="c")
        self.gal.columnconfigure(1, weight=1, uniform="c")
        self.gal_wait = self._label(self.gal, "Loading styles…", "body", "muted")
        self.gal_wait.grid(row=0, column=0, columnspan=2, pady=px(20))

        # the chosen style, spelled out
        info = tk.Frame(parent, bg=C["card"])
        info.pack(fill="x", pady=(px(12), 0))
        inner = tk.Frame(info, bg=C["card"])
        inner.pack(fill="x", padx=px(14), pady=px(12))
        self.st_name = self._label(inner, "", "style", bg="card")
        self.st_name.pack(fill="x")
        self.st_era = self._label(inner, "", "small", "accent", "card", wraplength=px(380))
        self.st_era.pack(fill="x", pady=(px(2), px(6)))
        self.st_sum = self._label(inner, "", "body", "muted", "card", wraplength=px(380))
        self.st_sum.pack(fill="x")

        look = tk.Frame(parent, bg=C["panel"])
        look.pack(fill="x", pady=(px(10), 0))
        self.look_lbl = self._label(look, "Tuned a look in the editor?", "small", "muted")
        self.look_lbl.pack(side="left")
        self.look_clear = FlatButton(look, "Use styles again", self.clear_look, "ghost",
                                     font=self.f["small"], padx=px(8), pady=px(3))
        FlatButton(look, "Load saved look…", self.load_look, "ghost", font=self.f["small"],
                   padx=px(8), pady=px(3)).pack(side="right")

    def _build_preview(self, parent):
        px = self.px
        self.well = tk.Frame(parent, bg=C["well"])
        self.well.pack(fill="both", expand=True)
        self.well.bind("<Configure>", lambda e: self._refit())
        self.view = tk.Label(self.well, bg=C["well"], bd=0)
        self.view.place(relx=0.5, rely=0.5, anchor="center")
        self.badge = tk.Label(self.well, text="  Painting…  ", font=self.f["small"],
                              bg=C["accent"], fg=C["accent_text"])
        self.cmp_badge = tk.Label(self.well, text="  Original  ", font=self.f["small"],
                                  bg=C["text"], fg=C["bg"])
        self.caption = self._label(parent, "", "small", "muted")
        self.caption.pack(fill="x", pady=(px(6), 0))

        ctl = tk.Frame(parent, bg=C["panel"])
        ctl.pack(fill="x", pady=(px(10), 0))
        self.cmp_btn = self._button(ctl, "Hold to compare with the original", lambda: None)
        self.cmp_btn.bind("<ButtonPress-1>", lambda e: self.compare(True))
        self.cmp_btn.bind("<ButtonRelease-1>", lambda e: self.compare(False))
        self.cmp_btn.pack(side="left")
        FlatButton(ctl, "Reset", self.reset_size, "ghost", font=self.f["small"], padx=px(8),
                   pady=px(3)).pack(side="right")
        self.mult_lbl = self._label(ctl, "", "bold")
        self.mult_lbl.pack(side="right", padx=(px(8), px(4)))
        self.mult_slider = self._slider(ctl, self.mult, 0.5, 2.0, 0.05, self.on_mult)
        self.mult_slider.configure(length=px(200), command=lambda v: self._mult_text())
        self.mult_slider.pack(side="right")
        self._label(ctl, "Brush size", "body", "muted").pack(side="right", padx=(px(16), px(8)))
        self._mult_text()

        # 4: what to do with it
        act = tk.Frame(parent, bg=C["panel"])
        act.pack(fill="x", pady=(px(16), 0))
        self.hints = []
        act.bind("<Configure>", lambda e: [h.configure(wraplength=max(200, e.width - px(8)))
                                           for h in self.hints])
        head = tk.Frame(act, bg=C["panel"])
        head.pack(fill="x", pady=(0, px(10)))
        tk.Label(head, text="4", font=self.f["step"], bg=C["accent"], fg=C["accent_text"],
                 width=2).pack(side="left")
        self.act_title = self._label(head, "Save or play", "h")
        self.act_title.pack(side="left", padx=(px(10), 0))
        box = tk.Frame(act, bg=C["panel"])        # keeps the buttons above progress/result
        box.pack(fill="x")

        self.act_none = tk.Frame(box, bg=C["panel"])
        self._hint(self.act_none, "Open a picture or a video in step 1. Then you can save the "
                   "painting, export a painted video, or play it live.", "body", "muted")

        self.act_image = tk.Frame(box, bg=C["panel"])
        row = tk.Frame(self.act_image, bg=C["panel"])
        row.pack(fill="x")
        self.save_btn = self._button(row, "Save painting…", self.save_still, "primary", "big")
        self.save_btn.pack(side="left")
        self.edit_btn = self._button(row, "Fine-tune in the editor", self.open_editor)
        self.edit_btn.pack(side="left", padx=(px(10), 0))
        self._hint(self.act_image, "Saving paints the full-size picture. The editor has every "
                   "slider; its Save params button makes a look you can load back here.")

        self.act_video = tk.Frame(box, bg=C["panel"])
        row = tk.Frame(self.act_video, bg=C["panel"])
        row.pack(fill="x")
        self.export_btn = self._button(row, "Export painted video…", self.export_video, "primary",
                                       "big")
        self.export_btn.pack(side="left")
        self.play_btn = self._button(row, "Play it painted, in a window", self.open_editor)
        self.play_btn.pack(side="left", padx=(px(10), 0))
        opts = tk.Frame(self.act_video, bg=C["panel"])
        opts.pack(fill="x", pady=(px(10), 0))
        Check(opts, "Steady brushstrokes (recommended)", self.steady, px(16), self.f["body"]).pack(
            side="left")
        self.audio_chk = Check(opts, "Keep the sound", self.keep_audio, px(16), self.f["body"])
        self.audio_chk.pack(side="left", padx=(px(24), 0))
        self._hint(self.act_video, "Steady brushstrokes repaint only what moves and carry the "
                   "paint along with the camera. Turn it off for a flickering, "
                   "every-frame-repainted look.")

        self.act_live = tk.Frame(box, bg=C["panel"])
        row = tk.Frame(self.act_live, bg=C["panel"])
        row.pack(fill="x")
        self.live_btn = self._button(row, "Start live", self.toggle_live, "primary", "big")
        self.live_btn.pack(side="left")
        self.live_lbl = self._label(row, "Not running", "big", "muted")
        self.live_lbl.pack(side="left", padx=(px(16), 0))
        self._hint(self.act_live, "Changing the style or brush size while live restarts the "
                   "painter (about a second). The preview above shows the style on a still "
                   "picture.")

        # progress + result
        self.prog = tk.Frame(act, bg=C["panel"])
        self.prog_bar = ttk.Progressbar(self.prog, style="Dark.Horizontal.TProgressbar",
                                        maximum=1000)
        self.prog_bar.pack(fill="x", pady=(px(12), px(6)))
        prow = tk.Frame(self.prog, bg=C["panel"])
        prow.pack(fill="x")
        self.prog_lbl = self._label(prow, "", "body")
        self.prog_lbl.pack(side="left")
        self.cancel_btn = FlatButton(prow, "Cancel", self.cancel_export, "ghost",
                                     font=self.f["small"], padx=px(8), pady=px(3))
        self.cancel_btn.pack(side="right")

        self.result = tk.Frame(act, bg=C["panel"])
        self.result_lbl = self._label(self.result, "", "body", "ok")
        self.result_lbl.pack(side="left")
        FlatButton(self.result, "Show in folder", lambda: self.last_output and reveal(
            self.last_output), "ghost", font=self.f["small"], padx=px(8), pady=px(3)).pack(
            side="right")
        FlatButton(self.result, "Open", lambda: self.last_output and os.startfile(
            self.last_output), "ghost", font=self.f["small"], padx=px(8), pady=px(3)).pack(
            side="right", padx=(0, px(4)))
        self.refresh_actions()

    # ── renderer ───────────────────────────────────────────────────────────

    def _start_renderer(self):
        self.hdr_build.pack_forget()
        self.hdr_retry.pack_forget()
        if self.renderer:
            self.renderer.stop()
            self.renderer = None
        self.ready = False
        if not EXE.exists():
            self.header(f"●  The renderer is not built yet ({EXE.name} is missing)", "err")
            self.hdr_retry.pack(side="right", padx=(self.px(10), 0))
            self.hdr_build.pack(side="right", padx=(self.px(10), 0))
            self.status("Build it once with the button at the top right (about a minute), "
                        "then press Check again.")
            return
        self.header("●  Starting the renderer…", "muted")
        self.renderer = Renderer(self.post, self.env)
        self.renderer.start()

    def build_renderer(self):
        subprocess.Popen(["cmd", "/c", "start", "Build brushkit", "cmd", "/k",
                          str(BUILD_BAT)], cwd=str(ROOT))
        self.status("Building in a new console window. When it says it has linked "
                    "brushkit.exe, press Check again.")

    def on_ready(self, styles, gpu):
        self.ready = True
        self.styles = styles
        gpu = gpu.split("/")[0]
        self.header(f"●  Renderer ready · {gpu}", "ok")
        if not self.ffmpeg:
            self.status("ffmpeg was not found, so videos cannot be opened. Install ffmpeg and "
                        "add it to PATH (C:\\ffmpeg\\bin also works).", "err")
        self._make_cards()
        if self.style_key not in self.cards:
            self.style_key = "turner" if "turner" in self.cards else styles[0]["key"]
        self.select_style(self.style_key, render=False)
        self.load_source(self.current())

    def on_fatal(self, msg):
        self.header("●  The renderer could not start", "err")
        self.hdr_retry.pack(side="right", padx=(self.px(10), 0))
        self.status(msg.splitlines()[-1] if msg else "unknown error", "err")
        self.log(msg)

    def on_log(self, line):
        self.log(line)

    # ── styles ─────────────────────────────────────────────────────────────

    def _make_cards(self):
        for w in self.gal.winfo_children():
            w.destroy()
        self.cards.clear()
        gap = self.px(6)
        for i, info in enumerate(self.styles):
            card = StyleCard(self.gal, info, self.select_style, self.tw, self.th, self.f,
                             self.placeholder)
            card.grid(row=i // 2, column=i % 2, sticky="nsew",
                      padx=(0 if i % 2 == 0 else gap, gap if i % 2 == 0 else 0), pady=(0, gap * 2))
            self.cards[info["key"]] = card

    def style_info(self, key):
        return next((s for s in self.styles if s["key"] == key), None)

    def select_style(self, key, render=True):
        if key not in self.cards:
            return
        self.saved_look = None
        self.style_key = key
        for k, card in self.cards.items():
            card.set_selected(k == key)
        info = self.style_info(key)
        if key == "none":
            self.st_name.configure(text="Classic renderer")
            self.st_era.configure(text="Hertzmann 1998 painterly rendering")
        else:
            self.st_name.configure(text=info["name"])
            era = f"{info['era']} · {info['years']}"
            if info.get("artists"):
                era += f" · {info['artists']}"
            self.st_era.configure(text=era)
        self.st_sum.configure(text=info["summary"])
        self._look_row()
        self.root.after_idle(lambda: self._see_card(key))
        if render:
            self.request_preview()
            self.live_changed()

    def _see_card(self, key):
        card = self.cards.get(key)
        if not card:
            return
        self.root.update_idletasks()
        total, view = self.gal.winfo_height(), self.gal_canvas.winfo_height()
        if total <= view or view < 10:
            return
        y0, y1 = card.winfo_y(), card.winfo_y() + card.winfo_height()
        top = self.gal_canvas.canvasy(0)
        if y0 < top:
            self.gal_canvas.yview_moveto(max(0, y0 - self.px(6)) / total)
        elif y1 > top + view:
            self.gal_canvas.yview_moveto((y1 - view + self.px(6)) / total)

    def load_look(self):
        f = filedialog.askopenfilename(title="Load a look saved from the editor",
                                       filetypes=[("Brushkit look", "*.sbr"), ("All files", "*.*")],
                                       initialdir=self.settings.get("last_dir") or str(ROOT))
        if not f:
            return
        self.saved_look = Path(f)
        for card in self.cards.values():
            card.set_selected(False)
        self.st_name.configure(text=self.saved_look.stem)
        self.st_era.configure(text="Your saved look · made with the editor's Save params")
        self.st_sum.configure(text="Brush size is part of the saved look, so the slider does "
                                   "not change it. Pick any style card to go back.")
        self._look_row()
        self.request_preview()
        self.live_changed()

    def clear_look(self):
        self.select_style(self.style_key)

    def _look_row(self):
        if self.saved_look:
            self.look_lbl.configure(text=f"Using {self.saved_look.name}", fg=C["accent"])
            self.look_clear.pack(side="right", padx=(self.px(6), 0))
        else:
            self.look_lbl.configure(text="Tuned a look in the editor?", fg=C["muted"])
            self.look_clear.pack_forget()

    def look_key(self):
        if self.saved_look:
            try:
                return f"sbr:{self.saved_look}:{self.saved_look.stat().st_mtime_ns}"
            except OSError:
                return f"sbr:{self.saved_look}"
        return self.style_key

    def look_args(self) -> list[str]:
        if self.saved_look:
            return ["--params", str(self.saved_look)]
        return ["--style", self.style_key, "--style-scale-mult", f"{self.mult.get():.3f}"]

    def _job_look(self) -> dict:
        if self.saved_look:
            return dict(style="", params=str(self.saved_look), mult=1.0)
        return dict(style=self.style_key, params="", mult=round(self.mult.get(), 2))

    # ── sources ────────────────────────────────────────────────────────────

    def current(self) -> Source:
        return self.source or self.sample

    def open_dialog(self):
        if self.mode != "file":
            self.set_mode("file")
        f = filedialog.askopenfilename(
            title="Open an image or a video",
            initialdir=self.settings.get("last_dir") or str(Path.home() / "Pictures"),
            filetypes=[("Images and videos", " ".join("*" + e for e in sorted(IMAGE_EXT | VIDEO_EXT))),
                       ("Images", " ".join("*" + e for e in sorted(IMAGE_EXT))),
                       ("Videos", " ".join("*" + e for e in sorted(VIDEO_EXT))),
                       ("All files", "*.*")])
        if f:
            self.open_path(Path(f))

    def open_path(self, path: Path):
        if not path.is_file():
            self.status(f"Cannot find {path}", "err")
            return
        kind = "video" if path.suffix.lower() in VIDEO_EXT else "image"
        if kind == "video" and not self.ffmpeg:
            self.status("Videos need ffmpeg. Install it and add it to PATH, then restart.", "err")
            return
        self.settings["last_dir"] = str(path.parent)
        if self.mode != "file":
            self.set_mode("file")
        self.source = Source(path, kind, key=file_key(path))
        self.src_name.configure(text=path.name)
        self.src_info.configure(text="Loading…")
        self.last_output = None
        self.result.pack_forget()
        self.load_source(self.source)

    def close_source(self):
        self.source = None
        self.src_thumb.configure(image="")
        self.src_thumb.photo = None
        self.src_thumb.pack_forget()
        self.src_name.configure(text="No file yet")
        self.src_info.configure(text="The preview shows a sample picture until you open one of "
                                     "your own.")
        self.video_box.pack_forget()
        self.clear_btn.pack_forget()
        self.load_source(self.current())

    def load_source(self, src: Source):
        if not src.key:
            src.key = file_key(src.path)
        self.gen += 1
        self.prepared = None
        self.refresh_actions()
        threading.Thread(target=self._prepare, args=(src, self.gen), daemon=True).start()

    def _prepare(self, src: Source, gen: int):
        try:
            if src.kind == "image":
                im, src.rotated = load_picture(src.path)
                src.width, src.height = im.size
                key = src.key
            else:
                if not src.probed:
                    src.__dict__.update(probe_video(self.ffprobe, src.path, self.env))
                    src.probed = True
                im = video_frame(self.ffmpeg, src.path, src.t, self.env)
                src.width, src.height = im.size          # upright, as ffmpeg decodes it
                key = f"{src.key}_{src.t:.1f}"
            prev = im.copy()
            prev.thumbnail((PREVIEW_SIDE, PREVIEW_SIDE), Image.LANCZOS)
            th = prev.copy()
            th.thumbnail((THUMB_SIDE, THUMB_SIDE), Image.LANCZOS)
            CACHE.mkdir(parents=True, exist_ok=True)
            pp, tp = CACHE / f"src_{key}_p.bmp", CACHE / f"src_{key}_t.bmp"
            prev.save(pp)
            th.save(tp)
            self.post("prepared", gen, src, dict(key=key, preview=str(pp), thumb=str(tp),
                                                 image=prev))
        except Exception as e:  # noqa: BLE001
            self.post("prepare_failed", gen, src, str(e))

    def on_prepared(self, gen, src: Source, info):
        if gen != self.gen:
            return
        self.prepared = info
        if not src.sample:
            self._show_source_card(src, info["image"])
        if not self.painted_for_current():
            self.show(info["image"], dim=True)
        self._caption()
        self.request_preview()
        self.request_thumbs()
        self.refresh_actions()

    def on_prepare_failed(self, gen, src, msg):
        if gen != self.gen:
            return
        self.status(f"Could not open {src.path.name}: {msg}", "err")
        if not src.sample:
            self.close_source()

    def _show_source_card(self, src: Source, im: Image.Image):
        w = self.px(268)
        t = im.copy()
        t.thumbnail((w, self.px(200)), Image.LANCZOS)
        photo = ImageTk.PhotoImage(t)
        self.src_thumb.configure(image=photo)
        self.src_thumb.photo = photo
        self.src_thumb.pack(fill="x", before=self.src_name)
        if src.kind == "image":
            self.src_info.configure(text=f"Photo · {src.width} × {src.height}")
            self.video_box.pack_forget()
        else:
            bits = [f"Video · {src.width} × {src.height}"]
            if src.fps:
                bits.append(f"{src.fps:.3g} fps")
            if src.duration:
                bits.append(fmt_time(src.duration))
            bits.append("with sound" if src.audio else "no sound")
            self.src_info.configure(text=" · ".join(bits))
            self.time_slider.configure(to=max(0.1, src.duration - 0.1))
            self.video_box.pack(fill="x", pady=(self.px(12), 0))
        self.clear_btn.pack(anchor="w", pady=(self.px(10), 0))

    def on_time(self):
        src = self.source
        if src and src.kind == "video" and abs(src.t - self.time_var.get()) > 0.05:
            src.t = self.time_var.get()
            self.load_source(src)

    # ── previews ───────────────────────────────────────────────────────────

    def preview_key(self):
        if not self.prepared:
            return None
        return (self.prepared["key"], self.look_key(),
                1.0 if self.saved_look else round(self.mult.get(), 2))

    def painted_for_current(self):
        k = self.preview_key()
        return k is not None and k in self.previews

    def request_preview(self):
        if not (self.ready and self.prepared and self.renderer):
            return
        key = self.preview_key()
        if key in self.previews:
            self.previews.move_to_end(key)
            self._show_painted(self.previews[key])
            return
        job = dict(self._job_look(), inp=self.prepared["preview"],
                   out=str(CACHE / f"pv_{uuid.uuid4().hex}.bmp"), load=True, key=key)
        self.renderer.submit("preview", job)
        self.badge.place(x=self.px(10), y=self.px(10))

    def request_thumbs(self):
        if not (self.ready and self.prepared and self.renderer):
            return
        mult = round(self.mult.get(), 2)
        jobs = []
        for s in self.styles:
            key = (self.prepared["key"], s["key"], mult)
            if key in self.thumbs:
                self._set_card(s["key"], self.thumbs[key])
            else:
                jobs.append(dict(style=s["key"], params="", mult=mult, inp=self.prepared["thumb"],
                                 out=str(CACHE / f"th_{uuid.uuid4().hex}.bmp"), load=True,
                                 key=key))
        self.renderer.set_thumbs(jobs)

    def _set_card(self, style, im):
        card = self.cards.get(style)
        if card:
            card.set_thumb(ImageTk.PhotoImage(ImageOps.fit(im, (self.tw, self.th), Image.LANCZOS)))

    def on_rendered(self, kind, job, img, ms):
        if kind == "preview":
            self.previews[job["key"]] = img
            while len(self.previews) > 40:
                self.previews.popitem(last=False)
            if job["key"] == self.preview_key():
                self._show_painted(img)
                self.log(f"preview {job['style'] or 'saved look'} in {ms:.0f} ms")
        elif kind == "thumb":
            self.thumbs[job["key"]] = img
            if self.prepared and job["key"][0] == self.prepared["key"] and \
                    job["key"][2] == round(self.mult.get(), 2):
                self._set_card(job["style"], img)
        elif kind == "still":
            final = Path(job["final"])
            if job["out"] != str(final):
                shutil.move(job["out"], final)
            self.save_btn.set(text="Save painting…", enabled=True)
            self.done(final, f"Saved {final.name} ({job['size']}) in {ms / 1000:.1f} s")

    def on_render_failed(self, kind, job, msg):
        self.log(f"{kind} failed: {msg}")
        if kind == "preview":
            self.badge.place_forget()
            self.status(f"Preview failed: {msg}", "err")
        elif kind == "still":
            self.save_btn.set(text="Save painting…", enabled=True)
            self.status(f"Saving failed: {msg}", "err")

    def _show_painted(self, img):
        self.badge.place_forget()
        self.painted = img
        if not self.comparing:
            self.show(img)
        self._caption()

    def _caption(self):
        src = self.current()
        if not self.prepared:
            return
        pw, ph = self.prepared["image"].size
        if self.mode == "live":
            text = (f"The style on a still picture. Live, the painting goes back to TouchDesigner "
                    f"as “{SPOUT_OUT}”.")
        elif src.sample:
            text = "Sample picture. Open your own image or video in step 1."
        elif src.kind == "image":
            text = f"Preview at {pw} × {ph}. Saving paints the full {src.width} × {src.height}."
        else:
            text = (f"Preview of the frame at {fmt_time(src.t)}, at {pw} × {ph}. "
                    f"The export paints every frame at {src.width} × {src.height}.")
        self.caption.configure(text=text)

    def show(self, img, dim=False):
        if dim:
            img = Image.blend(img, Image.new("RGB", img.size, C["well"]), 0.55)
        self.shown = img
        self._refit(now=True)

    def _refit(self, now=False):
        if self._fit_after:
            self.root.after_cancel(self._fit_after)
            self._fit_after = None
        if not now:
            self._fit_after = self.root.after(60, lambda: self._refit(now=True))
            return
        if self.shown is None:
            return
        W, H = self.well.winfo_width() - 2, self.well.winfo_height() - 2
        if W < 20 or H < 20:
            return
        iw, ih = self.shown.size
        s = min(W / iw, H / ih)
        im = self.shown.resize((max(1, int(iw * s)), max(1, int(ih * s))), Image.LANCZOS)
        self.view_photo = ImageTk.PhotoImage(im)
        self.view.configure(image=self.view_photo)

    def compare(self, on):
        if not self.prepared:
            return
        self.comparing = on
        if on:
            self.show(self.prepared["image"])
            self.cmp_badge.place(relx=1.0, x=-self.px(10), y=self.px(10), anchor="ne")
        else:
            self.cmp_badge.place_forget()
            if self.painted is not None and self.painted_for_current():
                self.show(self.painted)
            else:
                self.show(self.prepared["image"], dim=True)

    def _mult_text(self):
        self.mult_lbl.configure(text=f"{self.mult.get():.2f}×")

    def on_mult(self):
        self._mult_text()
        if self.saved_look:
            return
        self.request_preview()
        self.request_thumbs()
        self.live_changed()

    def reset_size(self):
        self.mult.set(1.0)
        self.on_mult()

    # ── modes and actions ──────────────────────────────────────────────────

    def set_mode(self, mode, initial=False):
        if mode == "file" and self.live:
            self.stop_live()
        self.mode = mode
        self.seg_file.set(kind="primary" if mode == "file" else "secondary")
        self.seg_live.set(kind="primary" if mode == "live" else "secondary")
        self.file_box.pack_forget()
        self.live_box.pack_forget()
        (self.file_box if mode == "file" else self.live_box).pack(fill="both", expand=True)
        if not initial:
            self.result.pack_forget()
            self.refresh_actions()
            self._caption()

    def refresh_actions(self):
        for w in (self.act_none, self.act_image, self.act_video, self.act_live):
            w.pack_forget()
        src = self.source
        if self.mode == "live":
            self.act_title.configure(text="Go live")
            self.act_live.pack(fill="x")
        elif src is None:
            self.act_title.configure(text="Save or play")
            self.act_none.pack(fill="x")
        elif src.kind == "image":
            self.act_title.configure(text="Save or fine-tune")
            self.act_image.pack(fill="x")
        else:
            self.act_title.configure(text="Export or play")
            self.act_video.pack(fill="x")
            self.audio_chk.label.configure(fg=C["text"] if src.audio or not src.probed
                                           else C["faint"])
        busy = self.export is not None
        ready = self.ready and self.prepared is not None
        self.save_btn.set(enabled=ready)
        self.edit_btn.set(enabled=ready)
        self.play_btn.set(enabled=ready)
        self.export_btn.set(enabled=ready and not busy,
                            text="Exporting…" if busy else "Export painted video…")
        self.live_btn.set(enabled=self.ready)

    def primary_action(self):
        if self.mode == "live":
            self.toggle_live()
        elif self.source and self.source.kind == "image":
            self.save_still()
        elif self.source:
            self.export_video()

    def _full_input(self, src: Source) -> str:
        """A path the renderer can read: upright, a format stb reads, ASCII."""
        p = src.path
        if src.kind == "image" and (p.suffix.lower() not in STB_EXT or src.rotated
                                    or not str(p).isascii()):
            out = CACHE / f"full_{src.key}.bmp"
            if not out.exists():
                load_picture(p)[0].save(out)
            return str(out)
        return ascii_alias(p, src.key)

    def default_name(self, ext):
        look = self.saved_look.stem if self.saved_look else self.style_key
        return f"{self.source.path.stem}_{look}{ext}"

    def save_still(self):
        src = self.source
        if not (src and src.kind == "image" and self.ready):
            return
        f = filedialog.asksaveasfilename(
            title="Save the painting", initialdir=str(src.path.parent),
            initialfile=self.default_name(".png"), defaultextension=".png",
            filetypes=[("PNG image", "*.png"), ("JPEG image", "*.jpg")])
        if not f:
            return
        final = Path(f)
        if final.suffix.lower() not in (".png", ".jpg", ".jpeg"):
            final = final.with_suffix(".png")
        self.save_btn.set(text="Painting…", enabled=False)
        self.status(f"Painting {src.width} × {src.height}…")
        look = self._job_look()

        def work():
            try:
                inp = self._full_input(src)
                out = str(final) if str(final).isascii() else \
                    str(CACHE / f"still_{uuid.uuid4().hex}{final.suffix.lower()}")
                self.renderer.submit("still", dict(look, inp=inp, out=out, final=str(final),
                                                   size=f"{src.width} × {src.height}"))
            except Exception as e:  # noqa: BLE001
                self.post("render_failed", "still", {}, str(e))
        threading.Thread(target=work, daemon=True).start()

    def open_editor(self):
        src = self.source
        if not (src and self.ready):
            return
        look = self.look_args()
        self.status("Opening the editor window…" if src.kind == "image" else
                    "Opening a window that plays the video painted (loops; Esc closes it)…")

        def work():
            try:
                args = [str(EXE), "--in", self._full_input(src)] + look
                if src.kind == "video":
                    args.append("--play")
                self.post("log", "> " + subprocess.list2cmdline(args))
                subprocess.Popen(args, cwd=str(ROOT), env=self.env, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, creationflags=NO_WINDOW)
            except Exception as e:  # noqa: BLE001
                self.post("status_err", f"Could not open the editor: {e}")
        threading.Thread(target=work, daemon=True).start()

    def on_status_err(self, msg):
        self.status(msg, "err")

    # ── video export ───────────────────────────────────────────────────────

    def export_video(self):
        src = self.source
        if not (src and src.kind == "video" and self.ready) or self.export:
            return
        f = filedialog.asksaveasfilename(
            title="Export the painted video", initialdir=str(src.path.parent),
            initialfile=self.default_name(".mp4"), defaultextension=".mp4",
            filetypes=[("MP4 video", "*.mp4")])
        if not f:
            return
        final = Path(f).with_suffix(".mp4")
        if final.resolve() == src.path.resolve():
            self.status("Choose a different name: that would overwrite the original.", "err")
            return
        target = final if str(final).isascii() else CACHE / f"export_{uuid.uuid4().hex}.mp4"
        args = [str(EXE), "--video", ascii_alias(src.path, src.key), "--out", str(target)]
        args += self.look_args()
        args += ["--temporal-diff", "12", "--flow", "4"] if self.steady.get() else \
            ["--temporal-diff", "0"]
        if not self.keep_audio.get():
            args.append("--no-audio")
        self.log("> " + subprocess.list2cmdline(args))
        proc = subprocess.Popen(args, cwd=str(ROOT), env=self.env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        self.export = dict(proc=proc, target=target, final=final, start=time.time(),
                           cancelled=False)
        self.result.pack_forget()
        self.prog_bar.configure(value=0)
        self.prog_lbl.configure(text="Starting…", fg=C["text"])
        self.prog.pack(fill="x")
        self.status(f"Exporting {final.name}. You can keep trying styles meanwhile; the export "
                    f"keeps the look it started with.")
        self.refresh_actions()
        threading.Thread(target=self._read_export, args=(proc,), daemon=True).start()

    def _read_export(self, proc):
        pat = re.compile(r"(\d+)/(\d+)\s+(\d+)%\s+([\d.]+) ms/frame")
        buf = b""
        last = 0.0
        while True:
            chunk = proc.stdout.read1(4096)
            if not chunk:
                break
            buf += chunk
            parts = re.split(rb"[\r\n]", buf)
            buf = parts.pop()
            for raw in parts:
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue
                m = pat.search(line)
                if m:
                    if time.time() - last > 0.1:
                        last = time.time()
                        self.post("export_progress", int(m.group(1)), int(m.group(2)))
                else:
                    self.post("log", line)
        self.post("export_done", proc.wait())

    def on_export_progress(self, done, total):
        ex = self.export
        if not ex:
            return
        frac = done / total if total else 0
        self.prog_bar.configure(value=int(frac * 1000))
        el = time.time() - ex["start"]
        eta = f" · about {fmt_time(el / frac - el)} left" if frac > 0.02 else ""
        self.prog_lbl.configure(text=f"Painting frame {done} of {total} · {frac:.0%}{eta}")

    def on_export_done(self, rc):
        ex, self.export = self.export, None
        self.prog.pack_forget()
        self.refresh_actions()
        if ex["cancelled"]:
            self.status("Export cancelled.")
            threading.Thread(target=self._remove_later, args=(ex["target"],), daemon=True).start()
            return
        if rc != 0 or not Path(ex["target"]).exists():
            self.status("The export failed. Show details for the renderer's message.", "err")
            return
        if ex["target"] != ex["final"]:
            shutil.move(str(ex["target"]), ex["final"])
        self.done(ex["final"], f"Exported {ex['final'].name} in "
                               f"{fmt_time(time.time() - ex['start'])}")

    def cancel_export(self):
        if self.export:
            self.export["cancelled"] = True
            self.export["proc"].terminate()
            self.prog_lbl.configure(text="Cancelling…")

    @staticmethod
    def _remove_later(path):
        for _ in range(20):
            time.sleep(0.5)
            try:
                Path(path).unlink(missing_ok=True)
                return
            except OSError:
                pass

    def done(self, path: Path, text):
        self.last_output = path
        self.result_lbl.configure(text="✓  " + text)
        self.result.pack(fill="x", pady=(self.px(12), 0))
        self.status(text, "ok")

    # ── live ───────────────────────────────────────────────────────────────

    def toggle_live(self):
        if self.live:
            self.stop_live()
        else:
            self.start_live()

    def start_live(self):
        if not self.ready or self.live:
            return
        stop = CACHE / "live.stop"
        stop.unlink(missing_ok=True)
        args = [str(EXE), "--live-spout"] + self.look_args() + [
            "--spout-in", SPOUT_IN, "--spout-out", SPOUT_OUT, "--target-fps", "30",
            "--temporal-diff", "12", "--flow", "4",
            "--live-parent-pid", str(os.getpid()), "--live-stop-file", str(stop)]
        self.log("> " + subprocess.list2cmdline(args))
        proc = subprocess.Popen(args, cwd=str(ROOT), env=self.env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                errors="replace", bufsize=1, creationflags=NO_WINDOW)
        self.live = dict(proc=proc, stop=stop, frames=None, t=None, stopping=False)
        self.live_btn.set(text="Stop live", kind="danger")
        self.live_lbl.configure(text="Starting…", fg=C["muted"])
        threading.Thread(target=self._read_live, args=(proc,), daemon=True).start()

    def _read_live(self, proc):
        pat = re.compile(r"Brushkit live: (\d+) frames, (\d+)x(\d+), GPU ([\d.]+) ms")
        for line in proc.stdout:
            line = line.strip()
            m = pat.search(line)
            if m:
                self.post("live_stats", proc, int(m.group(1)), int(m.group(2)), int(m.group(3)),
                          float(m.group(4)))
            elif line:
                self.post("log", line)
                if line.startswith("Waiting for Spout sender"):
                    self.post("live_waiting", proc)
        self.post("live_exit", proc, proc.wait())

    def on_live_waiting(self, proc):
        if self.live and self.live["proc"] is proc:
            self.live_lbl.configure(text=f"Waiting for TouchDesigner to send “{SPOUT_IN}”…",
                                    fg=C["accent"])

    def on_live_stats(self, proc, frames, w, h, gpu):
        lv = self.live
        if not lv or lv["proc"] is not proc:
            return
        now = time.time()
        if lv["frames"] is not None and now > lv["t"] and frames > lv["frames"]:
            fps = (frames - lv["frames"]) / (now - lv["t"])
            self.live_lbl.configure(text=f"Live · {w} × {h} · {fps:.1f} fps · GPU {gpu:.1f} ms",
                                    fg=C["ok"])
        elif lv["frames"] is None:
            self.live_lbl.configure(text=f"Live · {w} × {h}", fg=C["ok"])
        lv["frames"], lv["t"] = frames, now

    def on_live_exit(self, proc, rc):
        if not self.live or self.live["proc"] is not proc:
            return
        stopping = self.live["stopping"]
        self.live["stop"].unlink(missing_ok=True)
        self.live = None
        self.live_btn.set(text="Start live", kind="primary")
        if stopping:
            self.live_lbl.configure(text="Stopped", fg=C["muted"])
            if getattr(self, "_restart_live", False):
                self._restart_live = False
                self.start_live()
        else:
            self.live_lbl.configure(text="Stopped: the input went away" if rc == 0 else
                                    "The live painter stopped unexpectedly (see details)",
                                    fg=C["err"] if rc else C["muted"])

    def stop_live(self, wait=False):
        lv = self.live
        if not lv:
            return
        lv["stopping"] = True
        try:
            lv["stop"].touch()
        except OSError:
            lv["proc"].terminate()
        self.live_lbl.configure(text="Stopping…", fg=C["muted"])
        if wait:
            try:
                lv["proc"].wait(timeout=3)
            except subprocess.TimeoutExpired:
                lv["proc"].kill()
        else:
            self.root.after(3000, lambda p=lv["proc"]: p.poll() is None and p.kill())

    def live_changed(self):
        """Style or size changed: a running live painter restarts with it."""
        if not self.live:
            return
        if self._live_after:
            self.root.after_cancel(self._live_after)

        def restart():
            self._live_after = None
            if self.live:
                self._restart_live = True
                self.stop_live()
        self._live_after = self.root.after(400, restart)

    # ── misc ───────────────────────────────────────────────────────────────

    def copy(self, text):
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        self.status(f"Copied “{text}”", "ok")

    def open_td_guide(self):
        if TD_README.exists():
            os.startfile(TD_README)

    def header(self, text, color):
        self.hdr_status.configure(text=text, fg=C[color])

    def status(self, text, kind="info"):
        self.status_lbl.configure(text=text, fg={"info": C["muted"], "ok": C["ok"],
                                                 "err": C["err"]}[kind])
        if kind == "err":
            self.log("! " + text)

    def log(self, line):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line.rstrip() + "\n")
        if int(self.log_text.index("end-1c").split(".")[0]) > 600:
            self.log_text.delete("1.0", "200.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def toggle_log(self):
        if self.log_frame.winfo_ismapped():
            self.log_frame.pack_forget()
            self.log_btn.set(text="Show details ▸")
        else:
            self.log_frame.pack(side="bottom", fill="x", padx=self.px(20))
            self.log_btn.set(text="Hide details ▾")

    def _wheel(self, e):
        try:
            w = self.root.winfo_containing(e.x_root, e.y_root)
        except (KeyError, tk.TclError):
            return
        while w is not None:
            if w is self.gal_canvas:
                self.gal_canvas.yview_scroll(int(-e.delta / 120) or (-1 if e.delta > 0 else 1),
                                             "units")
                return
            w = w.master

    def on_close(self):
        if self.export:
            if not messagebox.askyesno("Brushkit", "A video export is still running. "
                                                    "Stop it and quit?"):
                return
            self.cancel_export()
        self.stop_live(wait=True)
        if self.renderer:
            self.renderer.stop()
        self._save_settings()
        self.root.destroy()


def main():
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:  # noqa: BLE001 - older Windows: stay DPI-unaware
            pass
    clean_cache()
    root = tk.Tk()
    initial = sys.argv[1] if len(sys.argv) > 1 else None
    App(root, initial)
    root.mainloop()


if __name__ == "__main__":
    main()
