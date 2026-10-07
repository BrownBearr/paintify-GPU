@echo off
REM Double-click to open brushkit. You can also drag an image or video
REM file onto this .bat to open it directly.
if "%~1"=="" (
  start "" "%~dp0build\brushkit.exe" --style cezanne
) else (
  start "" "%~dp0build\brushkit.exe" --style cezanne --in "%~1"
)
