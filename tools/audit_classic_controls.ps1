param(
    [string]$Executable = $(if (Test-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\paintify.exe')) { Join-Path (Split-Path -Parent $PSScriptRoot) 'build\paintify.exe' } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'build\brushkit.exe' }),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\sample.jpg'),
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'audit\slider-checks')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Executable)) { throw "Executable not found: $Executable" }
if (-not (Test-Path -LiteralPath $Source)) { throw "Source image not found: $Source" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

Add-Type -AssemblyName System.Drawing
$drawingAssembly = [System.Drawing.Bitmap].Assembly.Location
$drawingDir = Split-Path -Parent $drawingAssembly
$drawingReferences = @(
    $drawingAssembly,
    (Join-Path $drawingDir 'System.Drawing.Primitives.dll'),
    (Join-Path $drawingDir 'System.Private.Windows.GdiPlus.dll'),
    (Join-Path $drawingDir 'System.Private.Windows.Core.dll')
)
Add-Type -ReferencedAssemblies $drawingReferences -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public sealed class BrushkitDiff {
    public long ChangedPixels { get; set; }
    public double MeanAbsRgb { get; set; }
}

public static class BrushkitImageCompare {
    public static BrushkitDiff Compare(string aPath, string bPath) {
        using (var aSrc = new Bitmap(aPath))
        using (var bSrc = new Bitmap(bPath))
        using (var a = new Bitmap(aSrc.Width, aSrc.Height, PixelFormat.Format32bppArgb))
        using (var b = new Bitmap(bSrc.Width, bSrc.Height, PixelFormat.Format32bppArgb)) {
            if (aSrc.Width != bSrc.Width || aSrc.Height != bSrc.Height)
                throw new InvalidOperationException("Image sizes differ.");
            using (var g = Graphics.FromImage(a)) g.DrawImageUnscaled(aSrc, 0, 0);
            using (var g = Graphics.FromImage(b)) g.DrawImageUnscaled(bSrc, 0, 0);
            var rect = new Rectangle(0, 0, a.Width, a.Height);
            var da = a.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var db = b.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                int bytes = Math.Abs(da.Stride) * a.Height;
                var aa = new byte[bytes]; var bb = new byte[bytes];
                Marshal.Copy(da.Scan0, aa, 0, bytes); Marshal.Copy(db.Scan0, bb, 0, bytes);
                long changed = 0, sum = 0;
                long pixels = (long)a.Width * a.Height;
                for (int i = 0; i < bytes; i += 4) {
                    int d0 = Math.Abs(aa[i] - bb[i]);
                    int d1 = Math.Abs(aa[i + 1] - bb[i + 1]);
                    int d2 = Math.Abs(aa[i + 2] - bb[i + 2]);
                    if ((d0 | d1 | d2) != 0) changed++;
                    sum += d0 + d1 + d2;
                }
                return new BrushkitDiff { ChangedPixels = changed, MeanAbsRgb = (double)sum / (3 * pixels) };
            } finally {
                a.UnlockBits(da); b.UnlockBits(db);
            }
        }
    }
}
'@

$baseArgs = @('--headless', '--in', $Source, '--style', 'none')
$cases = @(
    @{ Name = 'stroke-radii'; ContextArgs = @(); ChangeArgs = @('--radii', '12,6,3') },
    @{ Name = 'detail'; ContextArgs = @(); ChangeArgs = @('--threshold', '95') },
    @{ Name = 'spacing'; ContextArgs = @(); ChangeArgs = @('--grid-factor', '1.6') },
    @{ Name = 'minimum-length'; ContextArgs = @(); ChangeArgs = @('--min-len', '1') },
    @{ Name = 'maximum-length'; ContextArgs = @(); ChangeArgs = @('--max-len', '28') },
    @{ Name = 'curvature'; ContextArgs = @(); ChangeArgs = @('--curvature', '0.2') },
    @{ Name = 'opacity'; ContextArgs = @(); ChangeArgs = @('--opacity', '0.45') },
    @{ Name = 'structure-tensor'; ContextArgs = @(); ChangeArgs = @('--tensor-sigma', '2.5') },
    @{ Name = 'edge-aware-flow'; ContextArgs = @(); ChangeArgs = @('--etf', '2') },
    @{ Name = 'edge-aware-radius'; ContextArgs = @('--etf', '2'); ChangeArgs = @('--etf-radius', '9') },
    @{ Name = 'brush-texture'; ContextArgs = @(); ChangeArgs = @('--brush-texture', '1') },
    @{ Name = 'bristle-density'; ContextArgs = @('--brush-texture', '1'); ChangeArgs = @('--bristle-density', '24') },
    @{ Name = 'tip-taper'; ContextArgs = @(); ChangeArgs = @('--texture-taper', '1') },
    @{ Name = 'dry-brush'; ContextArgs = @(); ChangeArgs = @('--dry-brush', '1') },
    @{ Name = 'size-variation'; ContextArgs = @(); ChangeArgs = @('--size-jitter', '0.5') },
    @{ Name = 'angle-variation'; ContextArgs = @(); ChangeArgs = @('--angle-jitter', '35') },
    @{ Name = 'opacity-variation'; ContextArgs = @(); ChangeArgs = @('--opacity-jitter', '0.5') },
    @{ Name = 'hue-variation'; ParamKey = 'jitter-hue'; ParamValue = '0.25' },
    @{ Name = 'saturation-variation'; ParamKey = 'jitter-sat'; ParamValue = '0.45' },
    @{ Name = 'brightness-variation'; ParamKey = 'jitter-val'; ParamValue = '0.45' },
    @{ Name = 'impasto-height-lit'; ContextArgs = @('--impasto-light', '0.4'); ChangeArgs = @('--impasto', '0.4') },
    @{ Name = 'impasto-light'; ContextArgs = @('--impasto', '0.4'); ChangeArgs = @('--impasto-light', '0.4') },
    @{ Name = 'light-direction'; ContextArgs = @('--impasto', '0.4', '--impasto-light', '0.4'); ChangeArgs = @('--light-angle', '210') },
    @{ Name = 'average-underpaint'; ContextArgs = @(); ChangeArgs = @('--underpaint', 'average') },
    @{ Name = 'plain-underpaint'; ContextArgs = @(); ChangeArgs = @('--underpaint', 'none') },
    @{ Name = 'fewer-layer-passes'; ContextArgs = @(); ChangeArgs = @('--passes', '3') },
    @{ Name = 'relaxation'; ContextArgs = @(); ChangeArgs = @('--relax', '1') },
    @{ Name = 'relaxation-economy'; ContextArgs = @('--relax', '1'); ChangeArgs = @('--relax-area', '0.2') },
    @{ Name = 'relaxation-move'; ContextArgs = @('--relax', '1'); ChangeArgs = @('--relax-move', '1.6') },
    @{ Name = 'relaxation-candidates'; ContextArgs = @('--relax', '1'); ChangeArgs = @('--relax-candidates', '12') },
    @{ Name = 'relaxation-removal'; ContextArgs = @('--relax', '1'); ChangeArgs = @('--relax-remove', '1.2') }
)

$baseline = Join-Path $OutputDirectory 'baseline.png'
$baselineLog = Join-Path $OutputDirectory 'baseline.log'
$baselineLook = Join-Path $OutputDirectory 'baseline.sbr'
& $Executable @baseArgs '--save-params' $baselineLook '--out' $baseline *> $baselineLog
if ($LASTEXITCODE -ne 0) { throw "Baseline render failed with exit code $LASTEXITCODE" }
$repeat = Join-Path $OutputDirectory 'baseline-repeat.png'
& $Executable @baseArgs '--out' $repeat *> (Join-Path $OutputDirectory 'baseline-repeat.log')
if ($LASTEXITCODE -ne 0) { throw "Repeat baseline render failed with exit code $LASTEXITCODE" }
$noise = [BrushkitImageCompare]::Compare($baseline, $repeat)
$baseLookLines = [string[]](Get-Content -LiteralPath $baselineLook)

function Write-ParamVariant([string]$Name, [string]$Key, [string]$Value) {
    $lines = [string[]]$baseLookLines.Clone()
    $found = $false
    for ($i = 0; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match ('^' + [regex]::Escape($Key) + '\s')) {
            $lines[$i] = "$Key $Value"
            $found = $true
            break
        }
    }
    if (-not $found) { throw "Parameter key was not present in the baseline look: $Key" }
    $path = Join-Path $OutputDirectory ($Name + '.sbr')
    [System.IO.File]::WriteAllLines($path, $lines)
    return $path
}

$results = foreach ($case in $cases) {
    $context = Join-Path $OutputDirectory ($case.Name + '-context.png')
    if ($case.ContainsKey('ParamKey')) {
        $contextLook = Join-Path $OutputDirectory ($case.Name + '-context.sbr')
        Copy-Item -LiteralPath $baselineLook -Destination $contextLook -Force
        & $Executable --headless --in $Source --params $contextLook '--out' $context *> (Join-Path $OutputDirectory ($case.Name + '-context.log'))
    } else {
        & $Executable @baseArgs @($case.ContextArgs) '--out' $context *> (Join-Path $OutputDirectory ($case.Name + '-context.log'))
    }
    if ($LASTEXITCODE -ne 0) { throw "Context render for $($case.Name) failed with exit code $LASTEXITCODE" }
    $output = Join-Path $OutputDirectory ($case.Name + '.png')
    if ($case.ContainsKey('ParamKey')) {
        $variantLook = Write-ParamVariant $case.Name $case.ParamKey $case.ParamValue
        & $Executable --headless --in $Source --params $variantLook '--out' $output *> (Join-Path $OutputDirectory ($case.Name + '.log'))
    } else {
        & $Executable @baseArgs @($case.ContextArgs) @($case.ChangeArgs) '--out' $output *> (Join-Path $OutputDirectory ($case.Name + '.log'))
    }
    if ($LASTEXITCODE -ne 0) { throw "Render for $($case.Name) failed with exit code $LASTEXITCODE" }
    $diff = [BrushkitImageCompare]::Compare($context, $output)
    [pscustomobject]@{
        Control = $case.Name
        ChangedPixels = $diff.ChangedPixels
        MeanAbsRgb = [Math]::Round($diff.MeanAbsRgb, 4)
        AboveRepeatNoise = ($diff.MeanAbsRgb -gt ($noise.MeanAbsRgb + 0.002))
        Output = $output
    }
}

$noiseSummary = [pscustomobject]@{
    RepeatBaselineChangedPixels = $noise.ChangedPixels
    RepeatBaselineMeanAbsRgb = [Math]::Round($noise.MeanAbsRgb, 4)
}
$noiseSummary | Format-List
$results | Format-Table -AutoSize
$results | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'results.csv') -NoTypeInformation
if (($results | Where-Object { -not $_.AboveRepeatNoise }).Count -gt 0) {
    Write-Warning 'At least one control did not exceed the repeated-baseline pixel difference; inspect that control or use another fixture.'
}
