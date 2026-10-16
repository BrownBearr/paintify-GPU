# Paintify feature and slider audit

Audit date: 2026-10-15. The source/render audit used `assets/sample.jpg`, the local Windows GPU build, and the current shader sources. The two repeat renders for each baseline were pixel-identical on this machine.

## Render checks

The classic sweep varied one control at a time against a render with the same prerequisites enabled. It covered 31 controls at 1000×740. All 31 changed pixels beyond the repeated-baseline difference of 0 pixels (mean absolute RGB difference 0).

The procedural brush sweep varied one `StyleParams` value at a time against a saved `.sbr` with the same brush mode, finish, ground, palette, or direction mode enabled. It covered 43 controls at 256×189. All 43 changed pixels beyond a repeated-baseline difference of 0 pixels (mean absolute RGB difference 0).

Run the repeatable sweeps from the repository root with PowerShell 7:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/audit_classic_controls.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/audit_style_controls.ps1
```

The scripts write their PNGs, `.sbr` fixtures, and per-render logs under `audit/` by default. Pass `-Executable`, `-Source`, or `-OutputDirectory` to use another local build, fixture, or output path. They select `build/paintify.exe` when present and otherwise use `build/brushkit.exe`.

## Control matrix

| Area | Controls | Where each one works | Evidence / state |
|---|---|---|---|
| Stroke size and placement | Stroke radii, Detail, Spacing, Min length, Max length, Curvature, Opacity | All still renders; per-layer stroke overrides replace the corresponding global length/detail/curvature values while a look defines those overrides | GPU cases `stroke-radii`, `detail`, `spacing`, `minimum-length`, `maximum-length`, `curvature`, `opacity`; all passed. The UI warns when per-layer overrides are active. |
| Classic tile texture | Brush texture strength, Bristle density, Tip taper, Fades along stroke | Classic tile brush. These are inactive while the procedural era brush is selected. Bristle density rebuilds the tile atlas when edited; the render path also rebuilds when radius or density changes | GPU cases `brush-texture`, `bristle-density` (texture enabled in both context and variant), `tip-taper`, `dry-brush`; all passed. |
| Classic relief | Thickness, Light strength, Light direction | Classic tile brush. Thickness contributes visible lighting when light strength or finish relief lighting is enabled; thickness can be set before lighting is turned on | GPU cases `impasto-height-lit`, `impasto-light`, `light-direction` compare with lighting and height prerequisites enabled; all passed. The Thickness help text now explains the lighting dependency. |
| Procedural brush basics | Width, Bristle count, Paint load, Paint thickness | Procedural/era brush only. Paint thickness is a height input used by relief lighting | `width`, `bristle-count`, `paint-load`, `paint-thickness`; all passed. |
| Procedural brush advanced | Bristle clumping, Streak contrast, Runs dry, Skips canvas texture, Edge roughness, Ragged ends, Edge ridges, Bristle grooves, Picks up wet paint, Drags paint underneath | Procedural/era brush only. Pickup and underpaint smear use the pre-layer canvas snapshot; dry-tooth depends on the selected support texture | `bristle-clumping`, `streak-contrast`, `runs-dry`, `skips-canvas-texture`, `edge-roughness`, `ragged-ends`, `edge-ridges`, `bristle-grooves`, `wet-pickup`, `underpaint-smear`; all passed. |
| Stroke variation | Size variation, Angle variation, Opacity variation, Hue variation, Saturation variation, Brightness variation | Per-stroke variation on the classic brush; color jitter is carried into the style path as well | `size-variation`, `angle-variation`, `opacity-variation`, `hue-variation`, `saturation-variation`, `brightness-variation`; all passed. |
| Direction source | Follow image, Swirl, Patches, Curl, Waves, Fixed angle | Both brush families when a direction effect is selected. Pattern strength is inapplicable to Follow image and is now hidden there. Swirl settings are disabled outside Swirl; each other pattern now shows its own controls | Mode and field parameters passed the `.sbr` sweep. GUI checks confirmed Follow image hides Pattern strength and Fixed angle rotates the visible strokes when Angle changes from −55° to 64°. |
| Direction parameters | Pattern strength, Wobble, Centre swirl on brightest area, Spiral twist, Swirl size, Patch size, Patch jitter, Patch quantization, Curl scale, Wave length, Wave amplitude, Wave angle, Fixed angle | Pattern strength and each pattern-specific value only affect their applicable direction mode. Wobble applies after the selected field. The wave angle is displayed in degrees and converted to radians for the shader | All 13 relevant values were isolated in mode-specific saved looks and changed rendered pixels. GUI checks exercised Pattern strength visibility and Fixed angle. |
| Direction smoothing | Smooth directions toggle, Amount | Structure-tensor field replaces Sobel when enabled. ETF passes/radius further smooth the image field, and radius only applies with passes above zero | Classic cases `structure-tensor`, `edge-aware-flow`, `edge-aware-radius`; all passed. The GUI toggle now remembers an enabled Amount when turned off and on; loaded positive sigma values are preserved. Final GUI check loaded sigma 0.1, changed Amount to 2.5, toggled off/on, and confirmed 2.5 was retained. The integrated executable also saved/loaded sigma 0.1 and 2.5 with pixel-identical results. |
| Stroke refinement | Refinement passes, Economy, Move distance, Tries per stroke, Remove weak strokes | Economy/move/candidates/removal are used only when refinement passes are greater than zero; the UI disables those values while refinement is off | `relaxation`, `relaxation-economy`, `relaxation-move`, `relaxation-candidates`, `relaxation-removal`; all passed with refinement enabled for dependent controls. |
| Color and palette | Saturation, Warm/cool, Lightness variation, Hue variation, Palette pull | Palette pull is shown only for a look with a palette; editing other color controls enables the style block for either brush family | `saturation`, `warm-cool`, `broken-lightness`, `broken-hue`, `palette-pull` (with three valid palette colors); all passed. |
| Ground and finish | Ground selector, Photo over ground, Relief strength, Relief depth, Gloss, Canvas weave, Varnish, Cracks, Vignette, Wet smear, Outline strokes, Rain | Photo over ground requires Toned ground. Relief lighting and finish overlays require Finish effects or a brush recipe that lights its relief. Finish sliders are disabled while finish is off | `photo-over-ground` and the 10 finish values passed with their ground/finish prerequisites enabled. |
| Video motion | Steady strokes, Repaint sensitivity, Fresh paint on movement, Motion tracking range, Tracking accuracy | Video only. Repaint sensitivity and fresh paint require Steady strokes; tracking accuracy requires motion tracking levels above zero | The workflow check passed a four-frame video render, `temporal-diff` off/on, and look save/load. The per-frame controls are not included in the still-image pixel sweeps. |
| Look persistence and brush selection | Save look, Load look, Brush selector, custom radii, size preset | Settings are saved in `.sbr`; choosing a brush changes brush-mark parameters while preserving editable color, ground, finish, and per-layer stroke settings | `.sbr` round-trip was exercised by the style sweep and the workflow check. Brush selection now preserves a loaded look and its per-layer paths; Final integrated GUI check loaded a look with Fixed angle 20 degrees and saturation 0.4, changed Flat hatch to Broad flat brush, and confirmed both independent values remained active. |

## Fixes from the audit

- Follow image no longer exposes an inactive Pattern strength control. The direction panel now exposes fixed-angle and mode-specific patch, curl, and wave parameters already present in `StyleParams`.
- Smooth directions now starts at a useful 2.0 sigma when first enabled, remembers its amount while disabled, and keeps positive loaded values intact. The toggle marks the look modified and resets temporal state during playback.
- Choosing a new brush no longer clears a loaded look's artistic settings or per-layer stroke overrides. The README and in-app notice describe that behavior.
- The Thickness help text states when paint height becomes visible.

The test sweeps verify that renderer parameters reach the GPU and change pixels in an applicable context. They do not automate pointer/keyboard interaction for every individual GUI slider. GUI checks exercised brush selection, width, Follow image visibility, Fixed angle, smoothing amount memory, and loaded-look setting preservation on the integrated Windows build. The video checks cover successful exports, not exhaustive motion quality, audio, every codec, or every UI interaction. macOS runtime validation requires CI or a Mac.
