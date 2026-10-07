# Brushkit for TouchDesigner

A Windows TouchDesigner component with one TOP input and one TOP output. It paints the incoming image or video in any brushkit style on the GPU, in realtime, and exchanges textures with TouchDesigner through Spout.
The component uses `brushkit.exe --live-spout` for GPU painting and exposes a *Painting style* menu.

## Build the component

1. Build the renderer by running `tools\build.bat` in this repository.
2. Open a blank TouchDesigner project.
3. Open **Dialogs → Textport**, switch it to Python, and run:

   ```python
   import runpy; runpy.run_path(r'C:\Users\I3row\Brushkit\touchdesigner\install_brushkit.py')
   ```

4. The script creates `/project1/brushkit` and saves `Brushkit.tox` in the repository root.
   The TOX bundles the renderer, its DLLs and its shaders. On first use it extracts them to `%LOCALAPPDATA%\Brushkit\<bundle-id>` and points the renderer at the extracted shaders with `BRUSHKIT_SHADER_DIR`.

## Use it

1. Connect a *Movie File In* or *Video Device In* TOP to the component's input.
2. Connect its output to a *Null* or a viewer.

| Page | Parameter | Meaning |
|---|---|---|
| Brushkit | Painting style | Any of the 23 styles, or *Classic renderer* to use the web presets |
| | Stroke size | -1 follows the input resolution (1.0 at a 1000 px long side) |
| | Painted frames / sec | Target painting rate (default 30) |
| | Repaint threshold | Temporal coherence: repaint only what changed (default 12; 0 repaints every frame) |
| | Optical flow levels | Carries paint along camera motion (default 4) |
| Classic renderer | Look, relaxation, brush texture, impasto | The web-preset controls, used when the style is *Classic renderer* |
| Advanced | radii, threshold, curvature, ... | Override the style's values; -1 keeps the style's own |

Changing any parameter restarts the renderer, which takes about half a second.

## Measured

On an RTX 3060 Ti, `build\brushkit-spout-smoke.exe --live --style <name>` round-trips 1280×720 frames through `--live-spout`:

| Style | Painted fps |
|---|---|
| turner | 27.1 |
| vangogh | 29.8 |
| cezanne | 28.7 |
| watercolor | 26.3 |

The target was 30 fps. TouchDesigner Non-Commercial limits images to 1280×1280.

## Troubleshooting

- **Blank output.** Open the Textport: the component prints the renderer's command line and log path. The log is `brushkit-live.log` next to the extracted renderer.
- **Two components at once.** Their Spout names derive from their operator paths, so they do not collide with each other or with `Paintify.tox`.
