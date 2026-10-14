@echo off
setlocal
REM Launch from either the source checkout or a portable release folder.
set "PAINTIFY_EXE=%~dp0paintify.exe"
if not exist "%PAINTIFY_EXE%" set "PAINTIFY_EXE=%~dp0dist\Paintify\paintify.exe"
if not exist "%PAINTIFY_EXE%" set "PAINTIFY_EXE=%~dp0build\paintify.exe"
if not exist "%PAINTIFY_EXE%" (
    echo Paintify is not built. Run tools\build.bat first.
    pause
    exit /b 1
)
if "%~1"=="" (
    start "" "%PAINTIFY_EXE%" --style cezanne
) else (
    start "" "%PAINTIFY_EXE%" --style cezanne --in "%~1"
)
