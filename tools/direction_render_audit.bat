@echo off
setlocal
cd /d "%~dp0.."
if not defined PAINTIFY_BUILD_DIR set "PAINTIFY_BUILD_DIR=build"
if not exist "%PAINTIFY_BUILD_DIR%\vcpkg_installed\x64-windows\include\glad\glad.h" if "%PAINTIFY_BUILD_DIR%"=="build" if exist "build-review\vcpkg_installed\x64-windows\include\glad\glad.h" set "PAINTIFY_BUILD_DIR=build-review"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 2
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not defined VS_ROOT exit /b 2
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 2
if not exist "%PAINTIFY_BUILD_DIR%\vcpkg_installed\x64-windows\include\glad\glad.h" (
    echo Build the application first and set PAINTIFY_BUILD_DIR to its build directory.
    exit /b 2
)
if not exist "audit\direction-deep" mkdir "audit\direction-deep"
if not exist "%PAINTIFY_BUILD_DIR%\direction-render-audit" mkdir "%PAINTIFY_BUILD_DIR%\direction-render-audit"
cl /nologo /EHsc /std:c++17 /MD /O2 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DSBR_ROOT_DIR=\".\" /Isrc /I"%PAINTIFY_BUILD_DIR%\vcpkg_installed\x64-windows\include" tools\direction_render_audit.cpp src\pipeline.cpp src\gl_util.cpp src\brush_atlas.cpp src\paramfile.cpp /Fo"%PAINTIFY_BUILD_DIR%\direction-render-audit\\" /Fe"%PAINTIFY_BUILD_DIR%\direction-render-audit\direction_render_audit.exe" /link /LIBPATH:"%PAINTIFY_BUILD_DIR%\vcpkg_installed\x64-windows\lib" glad.lib glfw3dll.lib user32.lib gdi32.lib shell32.lib
if errorlevel 1 exit /b 2
set "PATH=%CD%\%PAINTIFY_BUILD_DIR%;%PATH%"
"%PAINTIFY_BUILD_DIR%\direction-render-audit\direction_render_audit.exe"
exit /b %errorlevel%
