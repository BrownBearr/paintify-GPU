# GPU direction regression

After `tools\build.bat`, run `tools\flow_regression.bat` from a Windows terminal.
The script locates MSVC using vswhere, builds a standalone hidden-window GPU
harness against the existing vcpkg dependencies, and runs it. No test framework
or old shader fixture is required. Build outputs stay inside `build/flow-regression`.

The harness dispatches the actual Sobel, structure-tensor, ETF and style-field
compute shaders and reads back all pixels. Low contrast horizontal, vertical,
reversed and diagonal ramps have analytic tangents. A constant image checks
that Follow image does not invent a direction. Follow image and pattern
strength zero must preserve the input field exactly, including its magnitude.
Tiny pattern strengths must converge in vector space; all five full patterns
must be finite and change the field. It also checks fixed angle, explicit layer
rotation, and wobble with unchanged magnitude. Exit code 0 means every check
passed; 1 means an assertion failed; 2 means setup or compilation failed.

An optional frozen pre-fix shader directory can print the original field angles:
`tools\flow_regression.bat --baseline-dir audit/baseline/shaders`.
The directory is relative to the repository root and is not needed for any
analytic regression assertion. The optional baseline only adds the comparison
of full fixed-angle axes and magnitudes against the previous implementation.
