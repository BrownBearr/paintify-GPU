# Brushkit

A Windows GPU painting editor for images and video. The editor combines a Hertzmann stroke renderer with editable, era-inspired **brush textures**. Choosing an era changes the shape and surface of the mark: tip, bristles, load, dryout, edges, and raised paint. It does not apply that era's palette, composition, ground, or finishing effects.

## Start

Run tools\build.bat, then double-click run.bat. Drag an image or video onto run.bat to open it directly. The initial synthetic image is a demo; load a file to enable export.

The editor is organized as Source → Brush texture → Stroke size → Stroke placement and path → Paint and colour → Video frame coherence → Export. **Size preset** changes only the coarse-to-fine brush radii. **Save look** and **Load look** save the complete editable settings to an .sbr file. Old .sbr files retain their saved full-look settings. Choosing a new brush clears their legacy artistic effects and per-layer overrides; radius and core path controls remain editable.

The **stroke spacing** control sets candidate cell pitch to approximately radius × spacing. Higher values yield fewer candidate strokes. Stroke path controls alter geometry: maximum and minimum path length, curvature, and the image direction field govern tracing. **Keep strokes between frames** appears for video, reuses stable paint, and repaints moving areas. Switching it off also disables coherence for video export.

## Portable release

This app requires Windows and an OpenGL 4.6 GPU. Build with MSVC Build Tools, CMake 3.21 or newer, Ninja, and vcpkg; dependencies are in vcpkg.json. tools\build.bat locates Visual Studio and accepts VCPKG_ROOT if vcpkg is elsewhere.

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

The renderer was ported from [PainterlyImageCreatorWeb](https://github.com/BrownBearr/PainterlyImageCreatorWeb). See [the comparison notes](docs/comparison.md) for the original GPU/CPU reference analysis.
