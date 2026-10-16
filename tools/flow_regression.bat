@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 2
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not defined VS_ROOT exit /b 2
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 2
if not defined PAINTIFY_BUILD_DIR set "PAINTIFY_BUILD_DIR=build"
if not exist "%PAINTIFY_BUILD_DIR%\vcpkg_installed\x64-windows\include\glad\glad.h" (
    echo Run tools\build.bat first.
    exit /b 2
)
if not exist "%PAINTIFY_BUILD_DIR%\flow-regression" mkdir "%PAINTIFY_BUILD_DIR%\flow-regression"
cl /nologo /EHsc /std:c++17 /MD /O2 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DSBR_ROOT_DIR=\".\" /Isrc /I"%PAINTIFY_BUILD_DIR%\vcpkg_installed\x64-windows\include" tools\flow_regression.cpp src\gl_util.cpp /Fo"%PAINTIFY_BUILD_DIR%\flow-regression/" /Fe"%PAINTIFY_BUILD_DIR%\flow-regression\flow_regression.exe" /link /LIBPATH:"%PAINTIFY_BUILD_DIR%\vcpkg_installed\x64-windows\lib" glad.lib glfw3dll.lib user32.lib gdi32.lib shell32.lib
if errorlevel 1 exit /b 2
set "PATH=%CD%\%PAINTIFY_BUILD_DIR%;%PATH%"
"%PAINTIFY_BUILD_DIR%\flow-regression\flow_regression.exe" %*
exit /b %errorlevel%
