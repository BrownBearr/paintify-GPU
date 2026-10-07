@echo off
REM Opens the Brushkit launcher: pick a picture or video, pick a style, save or play.
REM Drop an image or video onto this file to open it straight away.
where pythonw >nul 2>nul
if %errorlevel%==0 (
  start "" pythonw "%~dp0launcher\brushkit_launcher.py" %*
) else (
  start "" python "%~dp0launcher\brushkit_launcher.py" %*
)
