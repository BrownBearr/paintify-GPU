# Install Brushkit for the current user so it launches from Start / Windows search.
#
#   tools\install.bat              build, install, add Start menu shortcut
#   tools\install.bat -Uninstall   remove the install and the shortcut
#
# Installs to %LOCALAPPDATA%\Programs\Brushkit (no admin rights needed). The
# shortcut starts in Pictures\Brushkit, so exports that use the default
# relative folder land there rather than inside the program folder.
param([switch]$Uninstall, [switch]$NoBuild)
$ErrorActionPreference = 'Stop'

$repo     = Split-Path -Parent $PSScriptRoot
$prefix   = Join-Path $env:LOCALAPPDATA 'Programs\Brushkit'
$startDir = Join-Path ([Environment]::GetFolderPath('MyPictures')) 'Brushkit'
$lnk      = Join-Path ([Environment]::GetFolderPath('Programs')) 'Brushkit.lnk'

if (Get-Process brushkit -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($prefix, 'OrdinalIgnoreCase') }) {
    throw "Brushkit is running from $prefix. Close it and run this again."
}

if ($Uninstall) {
    if (Test-Path $lnk)    { Remove-Item $lnk -Force }
    if (Test-Path $prefix) { Remove-Item $prefix -Recurse -Force }
    Write-Host "Removed Brushkit from $prefix and the Start menu."
    exit 0
}

if (-not $NoBuild) {
    & cmd /c "`"$repo\tools\build.bat`""
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

# Fresh copy each time so removed files do not linger.
if (Test-Path $prefix) { Remove-Item $prefix -Recurse -Force }
& cmake --install (Join-Path $repo 'build') --prefix $prefix
if ($LASTEXITCODE -ne 0) { throw 'cmake --install failed.' }

$exe = Join-Path $prefix 'brushkit.exe'
New-Item -ItemType Directory -Force $startDir | Out-Null

$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnk)
$s.TargetPath       = $exe
$s.Arguments        = '--style cezanne'
$s.WorkingDirectory = $startDir
$s.IconLocation     = "$exe,0"
$s.Description      = 'Brushkit - GPU painting editor'
$s.Save()

Write-Host "Installed to $prefix"
Write-Host "Start menu shortcut: $lnk  (search 'Brushkit')"
