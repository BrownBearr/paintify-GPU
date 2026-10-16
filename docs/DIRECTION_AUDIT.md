# Direction source audit

Audited on Windows, NVIDIA GeForce RTX 3060 Ti, OpenGL 4.6, using
`build-review/paintify.exe` and the editable source shaders. All tests use hidden
contexts or headless exports. The user's running installed app was not operated,
closed, or used as a test context.

## Result

All six direction sources passed the full renderer and settings audit:
**136 exported cases, 213 assertions, zero failures**. A separate persistent
`Pipeline` integration harness passed **285 assertions**, covering changes in
one live renderer rather than starting a new process for every change.

The independent direct GPU audit passed **1,481 assertions** with zero failures.
Its CPU-reference matrix covers **1,044 shader dispatches and 4,276,224 per-pixel
checks**, including every mode, zero/intermediate/full strength, UI control
endpoints and linear/Sobel/ETF/tensor/flat/weak/strong/curved source fields. Exact
singular and cardinal-angle cases supplement that matrix.

| Source | Tested controls and behavior | Result |
|---|---|---|
| Follow image | Horizontal/vertical low contrast image gradients; wobble 0/1.5; classic/style-off equivalence; tensor/ETF preservation; flat input | Correct source tangent; no invented strokes in a truly flat image |
| Swirl | Strength 0/0.5/1; spiral twist -1.5/1.5; radius 0.05/1; automatic centre off/on with an off-centre brightness peak | Controls reach tracing and export; automatic centre changes the painted vortex |
| Patches | Strength 0/0.5/1; cell size 4/160; jitter 0/1.5; quantization 0/1.5 | Patch controls change rendered paths; zero strength restores source paths |
| Curl | Strength 0/0.5/1; scale 8/600 | Scale changes rendered paths; zero strength restores source paths |
| Waves | Strength 0/0.5/1; wavelength 8/1000; amplitude 0/2; direction 0/90 degrees; -180/180 degree periodicity | Controls work, angle units survive save/load, full-turn endpoints agree |
| Fixed angle | Strength 0/0.5/1; angles 0/45/90/-45/180 degrees; orthogonal conflict at strengths 0.499/0.5/0.501 | Rasterized geometry follows requested axis; exact cancellation has a defined incoming-direction fallback |

Every mode was tested at sizes **1, 3, 17 and 1024 pixels square**. Every export
had the requested dimensions. Both classic tile and procedural brush models were
tested with identical source, radius, path and opacity settings. Tensor smoothing,
ETF and their combination were tested with all five patterns at zero and full
strength. Smoothing amount 0.5/8, ETF iterations 0/8 and ETF radius 1/12 reached
the renderer and changed the textured fixture as expected.

## Geometry evidence

Image differences alone are insufficient to show that tracing obeys the field.
The audit paints long strokes over white ground and measures the dominant axis
of their silhouettes. These axes are unoriented and expressed in image
coordinates, where positive y runs down the image.

| Requested geometry | Measured silhouette axis |
|---|---:|
| Follow image, brightness increases along x: vertical tangent | 91.27 degrees |
| Follow image, brightness increases along y: horizontal tangent | 1.34 degrees |
| Fixed 0 degrees | 8.35 degrees |
| Fixed 45 degrees | 46.53 degrees |
| Fixed 90 degrees | 91.27 degrees |
| Fixed -45 degrees, equivalent axis 135 degrees | 137.90 degrees |
| Fixed 180 degrees, equivalent axis 0 degrees | 8.35 degrees |
| Fixed 0 plus per-layer rotation 90 degrees | 91.26 degrees |

Silhouette estimates include stroke caps, overlaps and the seed grid, so they
are approximate. Direct shader readback tests in `tools/flow_regression.cpp`
provide the exact field checks. The fixed-angle silhouette assertions allow 12
degrees of error and require measurable anisotropy.

On a constant source, Follow image traced **zero** strokes. Pattern modes traced
**390–393** strokes on the same 160-pixel fixture because their explicit
procedural fields supply directions and a magnitude floor. That difference is
intentional. With the normal blurred underpaint, the flat region is already
covered even when Follow image has no strokes to add.

## Defined behavior and the guarded edge cases

Curl and Waves retain image-confidence gating: a full pattern amount can still
preserve a confident image tangent. Swirl also has a spatial influence radius;
full strength is not a promise to replace the field uniformly across the image.
Patches sample the image at each patch seed and apply its fallback, jitter and
quantization there. Wobble and per-layer rotation remain explicit modifications
to Follow image and to zero-strength patterns.

Direction blending treats angles separated by 180 degrees as the same axis.
For two orthogonal axes, equal weights cancel in doubled-angle space. The guard
keeps the incoming direction at that tie. In the rendered orthogonal test,
strengths 0.499, 0.5 and 0.501 produced silhouette axes of 91.27, 91.90 and 8.35
degrees. This is the branch change inherent in this axis blend; it does not
promise a smooth rotation through 45 degrees.

The narrow shader correction gives defined directions to zero-vector image
samples, curl stagnation points and the exact swirl centre, and gives a defined
incoming-direction fallback to doubled-angle cancellations. This addresses
undefined GLSL angle inputs and portability. The [GLSL 4.60 specification's
two-argument atan definition](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html)
does not define the result when both arguments are zero.

The full-render sweep did **not** reproduce a GPU crash or visibly corrupt
painting. The targeted raw-field GPU tests did reproduce different finite axes
at exact singularities: the pre-guard run failed 14 assertions in the expanded
deterministic-direction suite. For example, at the exact vortex eye it
returned 90 degrees for an incoming 0-degree axis; exact orthogonal 0.5 blends
selected axes influenced by floating-point round-off. A half-confidence patch
seed cancellation also discarded the incoming 45-degree axis. The guards pass
these tests by defining their incoming-direction policy explicitly. The curl
probe keeps the production `fbm` and `curlAngle` calculations and instruments
the exact-zero condition for readback in both guarded and unguarded shaders. It found 324 exact-zero finite-difference
candidates in a bounded coordinate scan; their lattice indices stay inside
signed 32-bit range. This is a reproduced singular-axis and portability
correction, not a reproduced NVIDIA crash fix. The unguarded curl helper happened to return zero at these exact-zero samples on this GPU; its explicit guard is for defined behavior across drivers.

## Settings and state checks

The audit inspected `src/main.cpp`, `src/pipeline.cpp` and `src/paramfile.cpp`:

- Direction selection and all pattern widgets write the corresponding style
  fields, enable the style block and mark settings changed. Wave angle converts
  UI degrees to stored radians. Fixed angle stores degrees directly.
- The render key hashes the entire style block, tuning parameters, ETF settings,
  radii and per-layer overrides. Mode and strength changes cause a repaint;
  repeating the same settings reuses the cached image.
- The persistent tests switch through all five patterns, return to Follow image,
  and check that zero-strength pixels equal the original source-path render.
  They repeat this for classic/procedural brushes and all four combinations of
  tensor smoothing and ETF.
- Saved looks preserve the direction block, tensor amount, ETF settings and
  per-layer rotation/curvature. A saved and reloaded look reproduces identical
  pixels in the persistent renderer.
- The current UI preserves tensor amount when toggling smoothing and resyncs
  the control state when loading a look. No additional UI wiring defect was
  identified in this audit.

Live mouse interaction with the user's open app was deliberately outside this
audit. The checks exercise the same underlying renderer, save/load code and
parameter layout, combined with inspection of the widget wiring.

## Reproduce

From the repository root, after building the application:

```powershell
python tools/direction_render_audit.py
$env:PAINTIFY_BUILD_DIR = 'build'
tools\direction_render_audit.bat
```

The Python sweep requires NumPy and Pillow. With no `--exe`, it selects
`build/paintify.exe`, then falls back to `build-review/paintify.exe`. The batch
script locates MSVC using vswhere and links the persistent renderer harness
against the existing vcpkg dependencies in `PAINTIFY_BUILD_DIR`. It defaults to
`build`, then falls back to `build-review` when the default build has no vcpkg
headers. Set `PAINTIFY_BUILD_DIR` to select another build directory. Neither
script has a user-specific absolute path.

Generated fixtures, looks, exports and measurements are under
`audit/direction-deep/`. `metrics.json` contains every rendered case and failed
assertion; `metrics.csv` holds geometry, coverage and stroke-count measurements.
Per-case logs include GPU, seed count, traced count and mean points per stroke.
The direct GPU logs are `gpu-root-before.log` for the frozen pre-guard run
(1,467 passes, 14 failures) and `gpu-root-final.log` for the passing instrumented
run (1,481 passes, no failures). Reproduce those checks with
`tools\flow_regression.bat`; see `tools/flow_regression.md` for its options.
All outputs are outside OneDrive. The scripts exit nonzero on failed assertions,
renderer errors, timeouts, or setup errors.

This validates Windows OpenGL execution and source-level portability guards.
It is not a macOS execution claim.
