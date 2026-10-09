@echo off
setlocal
REM Launch from either the source checkout or a portable release folder.
set "BRUSHKIT_EXE=%~dp0brushkit.exe"
if not exist "%BRUSHKIT_EXE%" set "BRUSHKIT_EXE=%~dp0dist\Brushkit\brushkit.exe"
if not exist "%BRUSHKIT_EXE%" set "BRUSHKIT_EXE=%~dp0build\brushkit.exe"
if not exist "%BRUSHKIT_EXE%" (
    echo Brushkit is not built. Run tools\build.bat first.
    pause
    exit /b 1
)
if "%~1"=="" (
    start "" "%BRUSHKIT_EXE%" --style cezanne
) else (
    start "" "%BRUSHKIT_EXE%" --style cezanne --in "%~1"
)
