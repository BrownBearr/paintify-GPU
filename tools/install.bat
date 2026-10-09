@echo off
REM Build Brushkit and install it for this user with a Start menu shortcut.
REM Pass -Uninstall to remove it, or -NoBuild to install the existing build.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
