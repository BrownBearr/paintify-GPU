# Brushkit

A GPU painting editor for images and video, for Windows and macOS. The editor combines a Hertzmann stroke renderer with editable, era-inspired **brush textures**. Choosing an era changes the shape and surface of the mark: tip, bristles, load, dryout, edges, and raised paint. It does not apply that era's palette, composition, ground, or finishing effects.

## Start

Run tools\build.bat, then double-click run.bat. Drag an image or video onto run.bat to open it directly. The initial synthetic image is a demo; load a file to enable export.

To launch it like any installed app, run tools\install.bat. It builds Brushkit, installs it to %LOCALAPPDATA%\Programs\Brushkit (no admin rights), and adds a Start menu shortcut, so it opens from Start or by typing "Brushkit" in Windows search. Run it again after pulling changes to update; tools\install.bat -Uninstall removes it. The shortcut starts in Pictures\Brushkit, so the default export folder is created there.

The editor is organized as Source → Brush texture → Stroke size → Stroke placement and path → Paint and colour → Video frame coherence → Export. **Size preset** changes only the coarse-to-fine brush radii. **Save look** and **Load look** save the complete editable settings to an .sbr file. Old .sbr files retain their saved full-look settings. Choosing a new brush clears their legacy artistic effects and per-layer overrides; radius and core path controls remain editable.

The **stroke spacing** control sets candidate cell pitch to approximately radius × spacing. Higher values yield fewer candidate strokes. Stroke path controls alter geometry: maximum and minimum path length, curvature, and the image direction field govern tracing. **Keep strokes between frames** appears for video, reuses stable paint, and repaints moving areas. Switching it off also disables coherence for video export.

## macOS

Brushkit runs natively on macOS 12 or later, on Apple silicon and Intel Macs. macOS has no OpenGL compute shaders, so on the Mac the renderer runs on Metal: the same `pipeline.cpp` drives a small GL-on-Metal layer (`src/mac/gl_metal.mm`), and the same GLSL shaders are translated to Metal Shading Language at load time with glslang and SPIRV-Cross. Cmd+R reloads edited shaders exactly as F5 does on Windows.

Build it with the Xcode command line tools, CMake and vcpkg:

    xcode-select --install
    brew install cmake ninja
    git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
    tools/build_mac.sh              # build/Brushkit.app
    tools/build_mac.sh --install    # also copies it to /Applications
    tools/build_mac.sh --dmg        # also writes dist/Brushkit-<version>-macOS-<arch>.dmg

It behaves like a Mac app: a full menu bar (⌘O open, ⇧⌘O open look, ⌘S save look, ⌘E export, ⌘1/2/3 views, ⌘0 actual size, ⌘9 fit, ⌘R reload shaders, ⌃⌘F full screen), Open Recent, system open and save panels, Open With from Finder and drops on the Dock icon (images, videos, folders and `.sbr` looks), the file's proxy icon in the title bar and the unsaved dot while a look has changes, pinch to zoom and two-finger scroll to pan, Retina-sharp text, and no redrawing while nothing changes. Launched from Finder it works in ~/Pictures/Brushkit.

Video needs ffmpeg (`brew install ffmpeg`); the app finds Homebrew's copy even when launched from Finder. The app is ad-hoc signed, not notarized: the first time, right-click it and choose Open (or allow it under System Settings > Privacy & Security).

The command line is the executable inside the bundle:

    build/Brushkit.app/Contents/MacOS/Brushkit --headless --in assets/sample.jpg --style cezanne --out painted.png

Command-line jobs (`--headless`, `--video`, `--batch`, `--frames`) open no window on macOS.

## Portable release

On Windows this app requires an OpenGL 4.6 GPU. Build with MSVC Build Tools, CMake 3.21 or newer, Ninja, and vcpkg; dependencies are in vcpkg.json. tools\build.bat locates Visual Studio and accepts VCPKG_ROOT if vcpkg is elsewhere.

    tools\build.bat
    cmake --install build --prefix dist\Brushkit
    cpack --config build\CPackConfig.cmake -B dist

The ZIP contains brushkit.exe, its linked runtime DLLs, shaders, run.bat, and this README. Shader loading prefers the folder beside the executable, so the release can run outside the source checkout. Video import/export also requires ffmpeg and ffprobe on PATH; image editing does not.

## Command line

    build\brushkit.exe --list-styles
    build\brushkit.exe --headless --in assets\sample.jpg --style cezanne --out painted.png
    build\brushkit.exe --in photo.jpg --preset Fine
    build\brushkit.exe --video clip.mp4 --out painted.mp4 --style vangogh --temporal-diff 12 --flow 4
    build\brushkit.exe --batch photos --outdir painted --format png

--style chooses a brush texture. --preset chooses a radius set; legacy names such as impressionist remain accepted. --style-scale is an optional procedural brush width multiplier. --help lists the rest of the painting, video, and export flags.

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

The renderer was ported from [PainterlyImageCreatorWeb](https://github.com/BrownBearr/PainterlyImageCreatorWeb). See [the comparison notes](docs/comparison.md) for the original GPU/CPU reference analysis.
