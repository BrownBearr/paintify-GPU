# Building and running Paintify

This is the developer guide: building from source, the command line, and how the renderer is organised. For what the app does, see the [README](../README.md).

A GPU painting editor for images and video, for Windows and macOS. The editor combines a Hertzmann stroke renderer with editable, era-inspired **brush textures**. Choosing an era changes the shape and surface of the mark: tip, bristles, load, dryout, edges, and raised paint. It does not apply that era's palette, composition, ground, or finishing effects.

## Start

Run tools\build.bat, then double-click run.bat. Drag an image or video onto run.bat to open it directly. The initial synthetic image is a demo; load a file to enable export.

To launch it like any installed app, run tools\install.bat. It builds Paintify, installs it to %LOCALAPPDATA%\Programs\Paintify (no admin rights), and adds a Start menu shortcut, so it opens from Start or by typing "Paintify" in Windows search. Run it again after pulling changes to update; tools\install.bat -Uninstall removes it. The shortcut starts in Pictures\Paintify, so the default export folder is created there.

The editor is organized as Source → Brush texture → Stroke size → Stroke placement and path → Paint and colour → Video frame coherence → Export. **Size preset** changes only the coarse-to-fine brush radii. **Save look** and **Load look** save the complete editable settings to an .sbr file. Old .sbr files retain their saved full-look settings. Choosing a new brush changes the mark while preserving the look's colour, ground, finish, and per-layer stroke settings.

The **stroke spacing** control sets candidate cell pitch to approximately radius × spacing. Higher values yield fewer candidate strokes. Stroke path controls alter geometry: maximum and minimum path length, curvature, and the image direction field govern tracing. **Keep strokes between frames** appears for video, reuses stable paint, and repaints moving areas. Switching it off also disables coherence for video export.

**Follow image** preserves the source direction and magnitude, including soft gradients. Brush textures share that path. Select Swirl, Patches, Curl, Waves, or Fixed angle to add a direction pattern; Pattern strength zero restores the image field. Run `tools\flow_regression.bat` for the GPU direction regression checks; see [the regression guide](../tools/flow_regression.md).

## macOS

Paintify runs natively on macOS 12 or later, on Apple silicon and Intel Macs. macOS has no OpenGL compute shaders, so on the Mac the renderer runs on Metal: the same `pipeline.cpp` drives a small GL-on-Metal layer (`src/mac/gl_metal.mm`), and the same GLSL shaders are translated to Metal Shading Language at load time with glslang and SPIRV-Cross. Cmd+R reloads edited shaders exactly as F5 does on Windows.

Build it with the Xcode command line tools, CMake and vcpkg:

    xcode-select --install
    brew install cmake ninja
    git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
    tools/build_mac.sh              # build/Paintify.app
    tools/build_mac.sh --install    # also copies it to /Applications
    tools/build_mac.sh --dmg        # also writes dist/Paintify-<version>-macOS-<arch>.dmg

It behaves like a Mac app: a full menu bar (⌘O open, ⇧⌘O open look, ⌘S save look, ⌘E export, ⌘1/2/3 views, ⌘0 actual size, ⌘9 fit, ⌘R reload shaders, ⌃⌘F full screen), Open Recent, system open and save panels, Open With from Finder and drops on the Dock icon (images, videos, folders and `.sbr` looks), the file's proxy icon in the title bar and the unsaved dot while a look has changes, pinch to zoom and two-finger scroll to pan, Retina-sharp text, and no redrawing while nothing changes. Launched from Finder it works in ~/Pictures/Paintify.

Video needs ffmpeg (`brew install ffmpeg`); the app finds Homebrew's copy even when launched from Finder. The app is ad-hoc signed, not notarized: the first time, right-click it and choose Open (or allow it under System Settings > Privacy & Security).

The command line is the executable inside the bundle:

    build/Paintify.app/Contents/MacOS/Paintify --headless --in assets/sample.jpg --style cezanne --out painted.png

Command-line jobs (`--headless`, `--video`, `--batch`, `--frames`) open no window on macOS.

## Portable release

On Windows this app requires an OpenGL 4.6 GPU. Build with MSVC Build Tools, CMake 3.21 or newer, Ninja, and vcpkg; dependencies are in vcpkg.json. tools\build.bat locates Visual Studio and accepts VCPKG_ROOT if vcpkg is elsewhere.

    tools\build.bat
    cmake --install build --prefix dist\Paintify
    cpack --config build\CPackConfig.cmake -B dist

The ZIP contains paintify.exe, its linked runtime DLLs, shaders, run.bat, and this README. Shader loading prefers the folder beside the executable, so the release can run outside the source checkout. Video import/export also requires ffmpeg and ffprobe on PATH; image editing does not.

## Command line

    build\paintify.exe --list-styles
    build\paintify.exe --headless --in assets\sample.jpg --style cezanne --out painted.png
    build\paintify.exe --in photo.jpg --preset Fine
    build\paintify.exe --video clip.mp4 --out painted.mp4 --style vangogh --temporal-diff 12 --flow 4
    build\paintify.exe --batch photos --outdir painted --format png

--style chooses a brush texture. --preset chooses a radius set; legacy names such as impressionist remain accepted. --style-scale is an optional procedural brush width multiplier. --help lists the rest of the painting, video, and export flags.

## Frame painting for video

In Look > Video motion, Frame painting offers two modes:

- Keep stable strokes: reuse paint where the image has not changed.
- Repaint whole frame: rebuild the painting from the current source frame, carrying no paint forward. Enable Vary strokes each frame for fresh stroke variation even in still areas.

The selection applies to playback and export and is stored in saved looks. Variation advances with decoded video frames; pausing keeps the preview still.

For command-line exports, use --temporal-diff 0 with --jitter-per-frame for an animated repaint, or --no-jitter-per-frame for repeatable strokes. Loading a saved look preserves its variation choice unless either flag explicitly overrides it.

The Windows regression check is python tools/motion_repaint_audit.py --exe build-review/paintify.exe. It compares decoded frames from four lossless exports: repeatable repaint, varied repaint, a saved look, and an explicit override.

## Functional audit

| Workflow | Status |
|---|---|
| Still image rendering and PNG export | Working |
| Batch images and frame sequences | Working; write failures now stop the job |
| Video playback and export | Working when ffmpeg and ffprobe are installed |
| .sbr save/load | Working with the existing version 1 layout |
| Per-era brush marks | Working; historical full looks remain loadable from older saved files |
| Field, colour, ground, and finish controls | Advanced controls; some effects depend on their parent mode |
| Portable Windows ZIP | Builds with executable, runtime DLL, shaders, and launcher |
| macOS app and DMG | Metal backend; built, painted with every brush style and packaged by CI on arm64 and Intel |

The renderer was ported from [PainterlyImageCreatorWeb](https://github.com/BrownBearr/PainterlyImageCreatorWeb). See [the comparison notes](comparison.md) for the original GPU/CPU reference analysis.
