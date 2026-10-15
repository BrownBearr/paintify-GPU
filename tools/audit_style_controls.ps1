param(
    [string]$Executable = $(if (Test-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\paintify.exe')) { Join-Path (Split-Path -Parent $PSScriptRoot) 'build\paintify.exe' } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'build\brushkit.exe' }),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\sample.jpg'),
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'audit\style-checks')
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

$smallSource = Join-Path $OutputDirectory 'source-256.png'
$srcBitmap = [System.Drawing.Bitmap]::new((Resolve-Path $Source).Path)
$smallBitmap = [System.Drawing.Bitmap]::new(256, [Math]::Max(1, [int][Math]::Round(256 * $srcBitmap.Height / $srcBitmap.Width)))
$graphics = [System.Drawing.Graphics]::FromImage($smallBitmap)
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.DrawImage($srcBitmap, 0, 0, $smallBitmap.Width, $smallBitmap.Height)
$smallBitmap.Save($smallSource, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $smallBitmap.Dispose(); $srcBitmap.Dispose()

$baselineLook = Join-Path $OutputDirectory 'cezanne.sbr'
$baseline = Join-Path $OutputDirectory 'baseline.png'
& $Executable --headless --in $smallSource --style cezanne --save-params $baselineLook --out $baseline *> (Join-Path $OutputDirectory 'baseline.log')
if ($LASTEXITCODE -ne 0) { throw "Styled baseline render failed with exit code $LASTEXITCODE" }
$repeat = Join-Path $OutputDirectory 'baseline-repeat.png'
& $Executable --headless --in $smallSource --params $baselineLook --out $repeat *> (Join-Path $OutputDirectory 'baseline-repeat.log')
if ($LASTEXITCODE -ne 0) { throw "Styled repeat render failed with exit code $LASTEXITCODE" }
$noise = [BrushkitImageCompare]::Compare($baseline, $repeat)

$baseLines = [System.Collections.Generic.List[string]]::new()
Get-Content -LiteralPath $baselineLook | ForEach-Object { $baseLines.Add($_) }
$styleLineIndex = -1
for ($i = 0; $i -lt $baseLines.Count; $i++) {
    if ($baseLines[$i].StartsWith('style-block ')) { $styleLineIndex = $i; break }
}
if ($styleLineIndex -lt 0) { throw 'Saved look has no style-block.' }
$baseTokens = $baseLines[$styleLineIndex].Split([char[]]@(' '), [System.StringSplitOptions]::RemoveEmptyEntries)
if ($baseTokens.Count -ne 261) { throw "Expected 260 style values, found $($baseTokens.Count - 1)." }

function New-StyleLook([string]$Name, [object[]]$Changes) {
    $tokens = [string[]]$baseTokens.Clone()
    foreach ($change in $Changes) { $tokens[1 + [int]$change.Index] = [string]$change.Value }
    $baseLines[$styleLineIndex] = $tokens -join ' '
    $path = Join-Path $OutputDirectory ($Name + '.sbr')
    [System.IO.File]::WriteAllLines($path, $baseLines)
    return $path
}

# StyleParams float offsets follow the s0..s32 groups in src/style.h.
$cases = @(
    @{ Name='width'; Context=@(); Change=@{Index=3;Value='1.9'} },
    @{ Name='bristle-count'; Context=@(); Change=@{Index=10;Value='48'} },
    @{ Name='paint-load'; Context=@(); Change=@{Index=15;Value='0.25'} },
    @{ Name='paint-thickness'; Context=@(@{Index=76;Value='1'},@{Index=78;Value='1.5'}); Change=@{Index=26;Value='1.5'} },
    @{ Name='bristle-clumping'; Context=@(); Change=@{Index=11;Value='0.95'} },
    @{ Name='streak-contrast'; Context=@(); Change=@{Index=12;Value='0.95'} },
    @{ Name='runs-dry'; Context=@(); Change=@{Index=16;Value='0.95'} },
    @{ Name='skips-canvas-texture'; Context=@(); Change=@{Index=17;Value='0.95'} },
    @{ Name='edge-roughness'; Context=@(); Change=@{Index=20;Value='0.58'} },
    @{ Name='ragged-ends'; Context=@(); Change=@{Index=22;Value='0.95'} },
    @{ Name='edge-ridges'; Context=@(); Change=@{Index=27;Value='0.95'} },
    @{ Name='bristle-grooves'; Context=@(); Change=@{Index=30;Value='1.9'} },
    @{ Name='wet-pickup'; Context=@(); Change=@{Index=23;Value='0.95'} },
    @{ Name='underpaint-smear'; Context=@(); Change=@{Index=24;Value='0.95'} },
    @{ Name='pattern-strength-swirl'; Context=@(@{Index=33;Value='1'}); Change=@{Index=34;Value='0.15'} },
    @{ Name='wobble'; Context=@(); Change=@{Index=45;Value='1.4'} },
    @{ Name='centre-swirl-on-brightest'; Context=@(@{Index=33;Value='1'}); Change=@{Index=128;Value='1'} },
    @{ Name='spiral-twist'; Context=@(@{Index=33;Value='1'}); Change=@{Index=40;Value='1.45'} },
    @{ Name='swirl-size'; Context=@(@{Index=33;Value='1'}); Change=@{Index=41;Value='0.9'} },
    @{ Name='patch-size'; Context=@(@{Index=33;Value='2'}); Change=@{Index=42;Value='12'} },
    @{ Name='patch-jitter'; Context=@(@{Index=33;Value='2'}); Change=@{Index=43;Value='1.4'} },
    @{ Name='patch-quantization'; Context=@(@{Index=33;Value='2'}); Change=@{Index=44;Value='1.2'} },
    @{ Name='curl-scale'; Context=@(@{Index=33;Value='3'}); Change=@{Index=47;Value='420'} },
    @{ Name='wave-length'; Context=@(@{Index=33;Value='4'}); Change=@{Index=48;Value='22'} },
    @{ Name='wave-amplitude'; Context=@(@{Index=33;Value='4'}); Change=@{Index=49;Value='1.9'} },
    @{ Name='wave-angle'; Context=@(@{Index=33;Value='4'}); Change=@{Index=50;Value='1.8'} },
    @{ Name='fixed-angle'; Context=@(@{Index=33;Value='5'}); Change=@{Index=35;Value='65'} },
    @{ Name='saturation'; Context=@(); Change=@{Index=56;Value='0.2'} },
    @{ Name='warm-cool'; Context=@(); Change=@{Index=60;Value='0.9'} },
    @{ Name='broken-lightness'; Context=@(); Change=@{Index=54;Value='18'} },
    @{ Name='broken-hue'; Context=@(); Change=@{Index=55;Value='18'} },
    @{ Name='palette-pull'; Context=@(@{Index=52;Value='3'},@{Index=132;Value='1'},@{Index=133;Value='0.1'},@{Index=134;Value='0.1'},@{Index=136;Value='0.1'},@{Index=137;Value='1'},@{Index=138;Value='0.1'},@{Index=140;Value='0.1'},@{Index=141;Value='0.1'},@{Index=142;Value='1'}); Change=@{Index=53;Value='1'} },
    @{ Name='photo-over-ground'; Context=@(@{Index=75;Value='1'}); Change=@{Index=71;Value='0'} },
    @{ Name='relief-strength'; Context=@(@{Index=76;Value='1'}); Change=@{Index=78;Value='1.9'} },
    @{ Name='relief-depth'; Context=@(@{Index=76;Value='1'}); Change=@{Index=77;Value='3.8'} },
    @{ Name='gloss'; Context=@(@{Index=76;Value='1'}); Change=@{Index=79;Value='0.58'} },
    @{ Name='canvas-weave'; Context=@(@{Index=76;Value='1'}); Change=@{Index=70;Value='0.95'} },
    @{ Name='varnish'; Context=@(@{Index=76;Value='1'}); Change=@{Index=84;Value='0.95'} },
    @{ Name='cracks'; Context=@(@{Index=76;Value='1'}); Change=@{Index=88;Value='0.95'} },
    @{ Name='vignette'; Context=@(@{Index=76;Value='1'}); Change=@{Index=90;Value='0.95'} },
    @{ Name='wet-smear'; Context=@(@{Index=76;Value='1'}); Change=@{Index=94;Value='0.95'} },
    @{ Name='outline-strokes'; Context=@(@{Index=76;Value='1'}); Change=@{Index=96;Value='0.95'} },
    @{ Name='rain'; Context=@(@{Index=76;Value='1'}); Change=@{Index=116;Value='0.95'} }
)

$results = foreach ($case in $cases) {
    $contextLook = New-StyleLook ($case.Name + '-context') $case.Context
    $contextImage = Join-Path $OutputDirectory ($case.Name + '-context.png')
    & $Executable --headless --in $smallSource --params $contextLook --out $contextImage *> (Join-Path $OutputDirectory ($case.Name + '-context.log'))
    if ($LASTEXITCODE -ne 0) { throw "Context render for $($case.Name) failed with exit code $LASTEXITCODE" }
    $look = New-StyleLook $case.Name (@($case.Context) + @($case.Change))
    $output = Join-Path $OutputDirectory ($case.Name + '.png')
    & $Executable --headless --in $smallSource --params $look --out $output *> (Join-Path $OutputDirectory ($case.Name + '.log'))
    if ($LASTEXITCODE -ne 0) { throw "Render for $($case.Name) failed with exit code $LASTEXITCODE" }
    $diff = [BrushkitImageCompare]::Compare($contextImage, $output)
    [pscustomobject]@{
        Control = $case.Name
        ChangedPixels = $diff.ChangedPixels
        MeanAbsRgb = [Math]::Round($diff.MeanAbsRgb, 4)
        AboveRepeatNoise = ($diff.MeanAbsRgb -gt ($noise.MeanAbsRgb + 0.002))
    }
}

[pscustomobject]@{
    Source = $smallSource
    RepeatBaselineChangedPixels = $noise.ChangedPixels
    RepeatBaselineMeanAbsRgb = [Math]::Round($noise.MeanAbsRgb, 4)
    Controls = $results.Count
} | Format-List
$results | Format-Table -AutoSize
$results | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'results.csv') -NoTypeInformation
if (($results | Where-Object { -not $_.AboveRepeatNoise }).Count -gt 0) {
    Write-Warning 'At least one style control did not exceed repeated-baseline variation; inspect its mode or dependency.'
}
