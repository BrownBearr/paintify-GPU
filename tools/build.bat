@echo off
setlocal
REM Build Brushkit with an installed MSVC toolchain, Ninja, and vcpkg.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer with C++ Build Tools is required.
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not defined VS_ROOT (
    echo No Visual Studio C++ toolchain was found.
    exit /b 1
)
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "NINJA=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%NINJA%" (
    for %%I in (ninja.exe) do set "NINJA=%%~$PATH:I"
)
if not exist "%NINJA%" (
    echo Ninja was not found.
    exit /b 1
)
if not defined VCPKG_ROOT set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo Set VCPKG_ROOT to your vcpkg installation directory.
    exit /b 1
)
if not defined BRUSHKIT_BUILD_DIR set "BRUSHKIT_BUILD_DIR=build"
cd /d "%~dp0.."
cmake -S . -B "%BRUSHKIT_BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" || exit /b 1
cmake --build "%BRUSHKIT_BUILD_DIR%" || exit /b 1
